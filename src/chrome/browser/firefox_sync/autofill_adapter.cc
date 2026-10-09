// Copyright 2026 The oganesson Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/firefox_sync/autofill_adapter.h"

#include <string_view>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "components/autofill/core/browser/data_model/addresses/autofill_profile.h"
#include "components/autofill/core/browser/data_model/payments/credit_card.h"
#include "components/autofill/core/browser/field_types.h"
#include "components/autofill/core/common/credit_card_network_identifiers.h"
#include "components/autofill/core/common/credit_card_number_validation.h"

namespace firefox_sync {

namespace {

using autofill::AutofillProfile;
using autofill::CreditCard;

// Firefox's address fields. Country first: it picks the address model the
// other fields go into.
constexpr std::pair<const char*, autofill::FieldType> kAddressFields[] = {
    {"country", autofill::ADDRESS_HOME_COUNTRY},
    {"name", autofill::NAME_FULL},
    {"organization", autofill::COMPANY_NAME},
    {"street-address", autofill::ADDRESS_HOME_STREET_ADDRESS},
    {"address-level3", autofill::ADDRESS_HOME_DEPENDENT_LOCALITY},
    {"address-level2", autofill::ADDRESS_HOME_CITY},
    {"address-level1", autofill::ADDRESS_HOME_STATE},
    {"postal-code", autofill::ADDRESS_HOME_ZIP},
    {"tel", autofill::PHONE_HOME_WHOLE_NUMBER},
    {"email", autofill::EMAIL_ADDRESS},
};

// Firefox's `cc-type` of a Chromium card network; Chromium derives the
// network from the number.
std::string_view FirefoxCardType(std::string_view network) {
  constexpr std::pair<std::string_view, std::string_view> kTypes[] = {
      {autofill::kAmericanExpressCard, "amex"},
      {autofill::kDinersCard, "diners"},
      {autofill::kDiscoverCard, "discover"},
      {autofill::kJCBCard, "jcb"},
      {autofill::kMasterCard, "mastercard"},
      {autofill::kMirCard, "mir"},
      {autofill::kUnionPay, "unionpay"},
      {autofill::kVisaCard, "visa"},
  };
  for (const auto& [chromium, firefox] : kTypes) {
    if (network == chromium) {
      return firefox;
    }
  }
  return "";
}

}  // namespace

AddressesAdapter::AddressesAdapter(autofill::AddressDataManager* manager,
                                   base::RepeatingClosure local_changed)
    : manager_(manager), local_changed_(std::move(local_changed)) {
  manager_->AddObserver(this);
}

AddressesAdapter::~AddressesAdapter() {
  manager_->RemoveObserver(this);
}

void AddressesAdapter::GetLocal(base::OnceCallback<void(base::Value)> done) {
  // Syncing without all local addresses would read as deleting them, and
  // while address autofill is off (Helium's default), adding one does nothing.
  if (!manager_->IsAutofillProfileEnabled()) {
    std::move(done).Run(base::Value(
        "Turn on \"Save and fill addresses\" under Contact info to sync "
        "them."));
    return;
  }
  if (!manager_->has_initial_load_finished() ||
      manager_->IsAwaitingPendingAddressChanges()) {
    std::move(done).Run(base::Value());
    return;
  }
  base::ListValue local;
  snapshot_.clear();
  for (const AutofillProfile* profile : manager_->GetProfilesByRecordType(
           AutofillProfile::RecordType::kLocalOrSyncable)) {
    base::DictValue record;
    record.Set("id", profile->guid());
    for (const auto& [name, type] : kAddressFields) {
      record.Set(name, base::UTF16ToUTF8(profile->GetRawInfo(type)));
    }
    snapshot_.insert(profile->guid());
    local.Append(std::move(record));
  }
  std::move(done).Run(base::Value(std::move(local)));
}

void AddressesAdapter::Apply(const base::ListValue& ops) {
  size_t added = 0, updated = 0, removed = 0;
  for (const base::Value& value : ops) {
    const base::DictValue* op = value.GetIfDict();
    const std::string* kind = op ? op->FindString("op") : nullptr;
    const std::string* id = op ? op->FindString("id") : nullptr;
    if (!kind || !id) {
      continue;
    }
    const AutofillProfile* existing = manager_->GetProfileByGUID(*id);
    if (existing && existing->record_type() !=
                        AutofillProfile::RecordType::kLocalOrSyncable) {
      continue;
    }
    if (*kind == "delete") {
      if (existing) {
        manager_->RemoveProfile(*id);
        ++removed;
      }
      continue;
    }
    if (*kind != "upsert" || (!existing && snapshot_.contains(*id))) {
      continue;  // Deleted here during the sync.
    }
    const std::string* country = op->FindString("country");
    AutofillProfile profile =
        existing ? *existing
                 : AutofillProfile(
                       *id, AutofillProfile::RecordType::kLocalOrSyncable,
                       autofill::AddressCountryCode(country ? *country : ""));
    // As the settings page saves an edited address.
    for (const auto& [name, type] : kAddressFields) {
      const std::string* field = op->FindString(name);
      profile.SetRawInfoWithVerificationStatus(
          type, base::UTF8ToUTF16(field ? *field : ""),
          autofill::VerificationStatus::kUserVerified);
    }
    profile.FinalizeAfterImport();
    if (existing) {
      manager_->UpdateProfile(profile);
      ++updated;
    } else {
      manager_->AddProfile(profile);
      ++added;
    }
  }
  snapshot_.clear();
  VLOG(1) << "Firefox Sync: addresses: " << added << " added, " << updated
          << " updated, " << removed << " removed";
}

void AddressesAdapter::OnAddressDataChanged() {
  local_changed_.Run();
}

CreditCardsAdapter::CreditCardsAdapter(autofill::PaymentsDataManager* manager,
                                       base::RepeatingClosure local_changed)
    : manager_(manager), local_changed_(std::move(local_changed)) {
  manager_->AddObserver(this);
}

CreditCardsAdapter::~CreditCardsAdapter() {
  manager_->RemoveObserver(this);
}

void CreditCardsAdapter::GetLocal(base::OnceCallback<void(base::Value)> done) {
  // As for addresses.
  if (!manager_->IsAutofillPaymentMethodsEnabled()) {
    std::move(done).Run(base::Value(
        "Turn on \"Save and fill payment methods\" under Payments to sync "
        "them."));
    return;
  }
  if (!manager_->is_payments_data_loaded() ||
      manager_->HasPendingPaymentQueries()) {
    std::move(done).Run(base::Value());
    return;
  }
  base::ListValue local;
  snapshot_.clear();
  for (const CreditCard* card : manager_->GetLocalCreditCards()) {
    base::DictValue record;
    record.Set("id", card->guid());
    record.Set("cc-name", base::UTF16ToUTF8(card->GetRawInfo(
                              autofill::CREDIT_CARD_NAME_FULL)));
    record.Set("cc-number", base::UTF16ToUTF8(
                                autofill::StripCardNumberSeparators(
                                    card->number())));
    record.Set("cc-exp-month", card->expiration_month());
    record.Set("cc-exp-year", card->expiration_year());
    record.Set("cc-type", FirefoxCardType(card->network()));
    snapshot_.insert(card->guid());
    local.Append(std::move(record));
  }
  std::move(done).Run(base::Value(std::move(local)));
}

void CreditCardsAdapter::Apply(const base::ListValue& ops) {
  size_t added = 0, updated = 0;
  std::vector<CreditCard> removed;
  for (const base::Value& value : ops) {
    const base::DictValue* op = value.GetIfDict();
    const std::string* kind = op ? op->FindString("op") : nullptr;
    const std::string* id = op ? op->FindString("id") : nullptr;
    if (!kind || !id) {
      continue;
    }
    const CreditCard* existing = manager_->GetCreditCardByGUID(*id);
    if (existing &&
        existing->record_type() != CreditCard::RecordType::kLocalCard) {
      continue;
    }
    if (*kind == "delete") {
      if (existing) {
        removed.push_back(*existing);
      }
      continue;
    }
    if (*kind != "upsert" || (!existing && snapshot_.contains(*id))) {
      continue;  // Deleted here during the sync.
    }
    CreditCard card = existing ? *existing : CreditCard(*id);
    auto str16 = [&](const char* field) {
      const std::string* value = op->FindString(field);
      return base::UTF8ToUTF16(value ? *value : "");
    };
    card.SetRawInfo(autofill::CREDIT_CARD_NAME_FULL, str16("cc-name"));
    card.SetRawInfo(autofill::CREDIT_CARD_NUMBER, str16("cc-number"));
    card.SetExpirationMonth(op->FindInt("cc-exp-month").value_or(0));
    card.SetExpirationYear(op->FindInt("cc-exp-year").value_or(0));
    if (existing) {
      manager_->UpdateCreditCard(card);
      ++updated;
    } else {
      manager_->AddCreditCard(card);
      ++added;
    }
  }
  if (!removed.empty()) {
    manager_->DeleteLocalCreditCards(removed);
  }
  snapshot_.clear();
  VLOG(1) << "Firefox Sync: creditcards: " << added << " added, " << updated
          << " updated, " << removed.size() << " removed";
}

void CreditCardsAdapter::OnPaymentsDataChanged() {
  local_changed_.Run();
}

}  // namespace firefox_sync
