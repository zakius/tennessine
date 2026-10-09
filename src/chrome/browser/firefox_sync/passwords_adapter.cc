// Copyright 2026 The oganesson Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/firefox_sync/passwords_adapter.h"

#include <map>
#include <utility>

#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "components/password_manager/core/browser/password_form.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace firefox_sync {

namespace {

using password_manager::PasswordForm;
using password_manager::StoredCredential;

// Firefox's `hostname` for a URL: its origin, without a trailing slash.
std::string OriginString(const GURL& url) {
  return url::Origin::Create(url).Serialize();
}

// A saved login as a Firefox `passwords` record, if Firefox has an
// equivalent: web form and HTTP auth logins with a password. Blocked sites,
// federated and app credentials stay local.
std::optional<base::DictValue> ToFirefox(const StoredCredential& form) {
  if (form.blocked_by_user || form.federation_origin.IsValid() ||
      form.password_value.empty() || !form.url.SchemeIsHTTPOrHTTPS()) {
    return std::nullopt;
  }
  std::string hostname = OriginString(form.url);
  base::DictValue login;
  login.Set("hostname", hostname);
  if (form.scheme == PasswordForm::Scheme::kHtml) {
    login.Set("formSubmitURL", form.action.SchemeIsHTTPOrHTTPS()
                                   ? OriginString(form.action)
                                   : std::string());
    login.Set("httpRealm", base::Value());
  } else if (form.scheme == PasswordForm::Scheme::kBasic ||
             form.scheme == PasswordForm::Scheme::kDigest) {
    // signon_realm is "<origin>/<realm>".
    std::string prefix = hostname + "/";
    if (!base::StartsWith(form.signon_realm, prefix)) {
      return std::nullopt;
    }
    login.Set("formSubmitURL", base::Value());
    login.Set("httpRealm", form.signon_realm.substr(prefix.size()));
  } else {
    return std::nullopt;
  }
  login.Set("username", base::UTF16ToUTF8(form.username_value));
  login.Set("password", base::UTF16ToUTF8(form.password_value.value()));
  login.Set("usernameField", base::UTF16ToUTF8(form.username_element));
  login.Set("passwordField", base::UTF16ToUTF8(form.password_element));
  auto ms = [](base::Time t) {
    return t.is_null() ? 0.0 : t.InMillisecondsFSinceUnixEpoch();
  };
  login.Set("timeCreated", ms(form.date_created));
  login.Set("timePasswordChanged", ms(form.date_password_modified.is_null()
                                          ? form.date_created
                                          : form.date_password_modified));
  login.Set("timeLastUsed", ms(form.date_last_used));
  login.Set("timesUsed", form.times_used_in_html_form);
  return login;
}

// The engine's key (Key in passwords.rs) of a record or of a key object.
std::string KeyOf(const base::DictValue& login) {
  const std::string* hostname = login.FindString("hostname");
  const std::string* realm = login.FindString("httpRealm");
  const std::string* username = login.FindString("username");
  return base::StrCat({hostname ? *hostname : "", "\n",
                       realm ? "realm:" + *realm : "form", "\n",
                       username ? *username : ""});
}

base::Time FromMs(const base::DictValue& login, const char* field) {
  std::optional<double> ms = login.FindDouble(field);
  return ms && *ms > 0 ? base::Time::FromMillisecondsSinceUnixEpoch(*ms)
                       : base::Time();
}

// Sets `form`'s fields from a Firefox record.
void SetFromFirefox(const base::DictValue& login, StoredCredential& form) {
  auto str16 = [&](const char* field) {
    const std::string* value = login.FindString(field);
    return base::UTF8ToUTF16(value ? *value : "");
  };
  form.username_value = str16("username");
  form.password_value = password_manager::PasswordString(str16("password"));
  if (form.username_element.empty()) {
    form.username_element = str16("usernameField");
  }
  if (form.password_element.empty()) {
    form.password_element = str16("passwordField");
  }
  base::Time changed = FromMs(login, "timePasswordChanged");
  if (!changed.is_null()) {
    form.date_password_modified = changed;
  }
  form.date_last_used = std::max(form.date_last_used,
                                 FromMs(login, "timeLastUsed"));
}

// A new login from a Firefox record the engine checked (usable()).
StoredCredential NewFromFirefox(const base::DictValue& login) {
  StoredCredential form;
  GURL origin(*login.FindString("hostname"));
  form.url = origin.GetWithEmptyPath();
  if (const std::string* realm = login.FindString("httpRealm")) {
    form.scheme = PasswordForm::Scheme::kBasic;
    form.signon_realm = form.url.spec() + *realm;
  } else {
    form.scheme = PasswordForm::Scheme::kHtml;
    form.signon_realm = form.url.spec();
    const std::string* action = login.FindString("formSubmitURL");
    if (action && GURL(*action).SchemeIsHTTPOrHTTPS()) {
      form.action = GURL(*action);
    }
  }
  form.type = PasswordForm::Type::kImported;
  form.in_store = PasswordForm::Store::kProfileStore;
  form.date_created = FromMs(login, "timeCreated");
  if (form.date_created.is_null()) {
    form.date_created = base::Time::Now();
  }
  form.times_used_in_html_form =
      static_cast<int>(login.FindDouble("timesUsed").value_or(0));
  SetFromFirefox(login, form);
  return form;
}

// A form holding only `form`'s primary key fields.
StoredCredential PrimaryKeyOf(const StoredCredential& form) {
  StoredCredential key;
  key.signon_realm = form.signon_realm;
  key.url = form.url;
  key.username_element = form.username_element;
  key.username_value = form.username_value;
  key.password_element = form.password_element;
  key.scheme = form.scheme;
  return key;
}

}  // namespace

PasswordsAdapter::PasswordsAdapter(
    scoped_refptr<password_manager::PasswordStoreInterface> store,
    base::RepeatingClosure local_changed)
    : store_(std::move(store)), local_changed_(std::move(local_changed)) {
  store_->AddObserver(this);
}

PasswordsAdapter::~PasswordsAdapter() {
  store_->RemoveObserver(this);
}

void PasswordsAdapter::GetLocal(base::OnceCallback<void(base::Value)> done) {
  get_local_ = std::move(done);
  store_->GetAllLogins(weak_factory_.GetWeakPtr());
}

void PasswordsAdapter::OnGetPasswordStoreResultsOrErrorFrom(
    password_manager::PasswordStoreInterface* store,
    password_manager::LoginsResultOrError results) {
  if (!std::holds_alternative<password_manager::LoginsResult>(results)) {
    // Syncing without the local logins would read as deleting them.
    std::move(get_local_).Run(base::Value());
    return;
  }
  snapshot_ = std::move(std::get<password_manager::LoginsResult>(results));
  syncing_ = true;
  changed_during_sync_.clear();
  base::ListValue local;
  for (const StoredCredential& form : snapshot_) {
    if (std::optional<base::DictValue> login = ToFirefox(form)) {
      local.Append(std::move(*login));
    }
  }
  std::move(get_local_).Run(base::Value(std::move(local)));
}

void PasswordsAdapter::Apply(const base::ListValue& ops) {
  // The snapshot's logins by engine key.
  std::map<std::string, std::vector<StoredCredential*>> by_key;
  for (StoredCredential& form : snapshot_) {
    if (std::optional<base::DictValue> login = ToFirefox(form)) {
      by_key[KeyOf(*login)].push_back(&form);
    }
  }
  // Logins changed locally since the snapshot keep the local version.
  auto changed_since_snapshot = [&](const base::DictValue& key) {
    return changed_during_sync_.contains(KeyOf(key));
  };
  size_t added = 0, updated = 0, removed = 0, skipped = 0;
  for (const base::Value& value : ops) {
    const base::DictValue* op = value.GetIfDict();
    const std::string* kind = op ? op->FindString("op") : nullptr;
    if (!kind) {
      continue;
    }
    if (*kind == "delete") {
      const base::DictValue* key = op->FindDict("key");
      if (key && changed_since_snapshot(*key)) {
        ++skipped;
        continue;
      }
      for (StoredCredential* form : key ? by_key[KeyOf(*key)]
                                        : std::vector<StoredCredential*>()) {
        store_->RemoveLogin(FROM_HERE, *form);
        ++removed;
      }
      continue;
    }
    const base::DictValue* login = op->FindDict("login");
    if (*kind != "upsert" || !login) {
      continue;
    }
    const base::DictValue* replaces = op->FindDict("replaces");
    if (changed_since_snapshot(*login) ||
        (replaces && changed_since_snapshot(*replaces))) {
      ++skipped;
      continue;
    }
    std::vector<StoredCredential*>& forms =
        by_key[KeyOf(replaces ? *replaces : *login)];
    const std::string* hostname = login->FindString("hostname");
    const std::string* realm = login->FindString("httpRealm");
    bool same_site = replaces && hostname &&
                     *hostname == *replaces->FindString("hostname") &&
                     (realm ? *realm : "") ==
                         (replaces->FindString("httpRealm")
                              ? *replaces->FindString("httpRealm")
                              : "");
    if (forms.empty() || (replaces && !same_site)) {
      // New, or moved to another site: replace with a fresh login.
      for (StoredCredential* form : forms) {
        store_->RemoveLogin(FROM_HERE, *form);
      }
      store_->AddLogin(NewFromFirefox(*login));
      ++added;
      continue;
    }
    for (StoredCredential* form : forms) {
      StoredCredential old_key = PrimaryKeyOf(*form);
      SetFromFirefox(*login, *form);
      // SetFromFirefox can change any primary-key field (it fills empty field
      // names), and UpdateLogin finds the row by the new key.
      if (form->signon_realm != old_key.signon_realm ||
          form->url != old_key.url ||
          form->username_element != old_key.username_element ||
          form->username_value != old_key.username_value ||
          form->password_element != old_key.password_element) {
        store_->UpdateLoginWithPrimaryKey(std::move(*form), old_key);
      } else {
        store_->UpdateLogin(std::move(*form));
      }
      ++updated;
    }
    forms.clear();
  }
  snapshot_.clear();
  syncing_ = false;
  changed_during_sync_.clear();
  VLOG(1) << "Firefox Sync: passwords: " << added << " added, " << updated
          << " updated, " << removed << " removed, " << skipped
          << " left for the next sync";
}

void PasswordsAdapter::OnLoginsChanged(
    password_manager::PasswordStoreInterface* store,
    const password_manager::PasswordStoreChangeList& changes) {
  if (syncing_) {
    for (const password_manager::PasswordStoreChange& change : changes) {
      if (std::optional<base::DictValue> login =
              ToFirefox(change.credential())) {
        changed_during_sync_.insert(KeyOf(*login));
      }
    }
  }
  local_changed_.Run();
}

void PasswordsAdapter::OnLoginsRetained(
    password_manager::PasswordStoreInterface* store,
    const std::vector<StoredCredential>& retained) {}

}  // namespace firefox_sync
