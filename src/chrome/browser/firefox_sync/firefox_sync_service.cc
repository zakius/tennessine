// Copyright 2026 The oganesson Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/firefox_sync/firefox_sync_service.h"

#include <utility>

#include "base/android/bundle_utils.h"
#include "base/barrier_closure.h"
#include "base/base64.h"
#include "base/base_paths.h"
#include "base/compiler_specific.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/native_library.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/autofill/personal_data_manager_factory.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/firefox_sync/autofill_adapter.h"
#include "chrome/browser/firefox_sync/bookmarks_adapter.h"
#include "chrome/browser/firefox_sync/history_adapter.h"
#include "chrome/browser/firefox_sync/passwords_adapter.h"
#include "chrome/browser/firefox_sync/tabs_adapter.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/password_manager/factories/profile_password_store_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/android/tab_model/tab_model.h"
#include "chrome/browser/ui/android/tab_model/tab_model_list.h"
#include "components/autofill/core/browser/data_manager/personal_data_manager.h"
#include "components/keyed_service/core/service_access_type.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "net/base/net_errors.h"
#include "net/base/url_util.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace firefox_sync {

namespace {

constexpr char kEmailPref[] = "oganesson.firefox_sync.email";
constexpr char kSecretsPref[] = "oganesson.firefox_sync.secrets";
constexpr char kLastSyncPref[] = "oganesson.firefox_sync.last_sync";
constexpr char kDisabledTypesPref[] = "oganesson.firefox_sync.disabled_types";
// The accounts server's origin; empty until the user sets one.
constexpr char kServerPref[] = "oganesson.firefox_sync.server";
// A self-hosted sync storage server's URL; empty for the accounts server's.
constexpr char kSyncServerPref[] = "oganesson.firefox_sync.sync_server";
constexpr char kMozillaServer[] = "https://accounts.firefox.com";

// The types Firefox Sync has and application-services has engines for, by
// Sync collection name, in settings order. `supported` ones have a browser
// adapter and a library engine; the rest are shown, switched off.
struct DataType {
  const char* name;
  const char* label;
  bool supported;
};
constexpr DataType kTypes[] = {
    {"passwords", "Passwords", true},
    {"bookmarks", "Bookmarks", true},
    {"history", "History", true},
    {"tabs", "Open tabs", true},
    {"addresses", "Addresses", true},
    {"creditcards", "Payment methods", true},
};

constexpr base::TimeDelta kSyncInterval = base::Minutes(30);
constexpr base::TimeDelta kStartupDelay = base::Seconds(20);
constexpr base::TimeDelta kLocalChangeDelay = base::Seconds(30);

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("oganesson_firefox_sync", R"(
      semantics {
        sender: "Firefox Sync"
        description:
          "Signs in to the user's Mozilla account and syncs saved passwords, "
          "bookmarks, history, open tabs, addresses and payment methods with Firefox "
          "Sync servers, end-to-end encrypted."
        trigger: "The user signed in to Firefox Sync in settings."
        data: "Mozilla account tokens and encrypted password, bookmark, "
              "history, tab, address and payment card records."
        destination: OTHER
      }
      policy {
        cookies_allowed: NO
        setting: "Sign out of Firefox Sync in Autofill and passwords settings."
        policy_exception_justification: "Not implemented."
      })");

// The library's C API; see rust/oganesson_sync/src/lib.rs.
using HttpSetFn = void (*)(void* out, const char* response_json);
using HttpFn = void (*)(void* ctx,
                        const char* request_json,
                        void* out,
                        HttpSetFn set);
struct Library {
  void (*init)(HttpFn http);
  char* (*call)(void* ctx, const char* method, const char* args);
  void (*free)(char* result);
};

void HttpCallback(void* ctx,
                  const char* request_json,
                  void* out,
                  HttpSetFn set);

// Why the library didn't load, for the settings screen.
std::string& LoadError() {
  static base::NoDestructor<std::string> error;
  return *error;
}

// Loads liboganesson_sync.so from the APK's native libraries. The linker
// namespace libchrome.so is in doesn't search the APK, so the path comes from
// the class loader, as for Chromium's own module libraries. Blocks. The library brings its own NSS (Chromium
// on Android has none) and initializes it. Calls into the library are
// indirect calls CFI can't check, hence DISABLE_CFI_ICALL.
DISABLE_CFI_ICALL const Library* LoadSyncLibrary() {
  base::NativeLibraryLoadError error;
  std::string path =
      base::android::BundleUtils::ResolveLibraryPath("oganesson_sync", "");
  base::NativeLibrary native = base::LoadNativeLibrary(
      base::FilePath(path.empty() ? "liboganesson_sync.so" : path), &error);
  if (!native) {
    LoadError() = error.ToString();
    LOG(WARNING) << "Firefox Sync is unavailable: " << LoadError();
    return nullptr;
  }
  auto* lib = new Library{
      reinterpret_cast<void (*)(HttpFn)>(
          base::GetFunctionPointerFromNativeLibrary(native, "og_init")),
      reinterpret_cast<char* (*)(void*, const char*, const char*)>(
          base::GetFunctionPointerFromNativeLibrary(native, "og_call")),
      reinterpret_cast<void (*)(char*)>(
          base::GetFunctionPointerFromNativeLibrary(native, "og_free")),
  };
  if (!lib->init || !lib->call || !lib->free) {
    LoadError() = "bad liboganesson_sync.so";
    LOG(WARNING) << "Firefox Sync is unavailable: " << LoadError();
    return nullptr;
  }
  lib->init(&HttpCallback);
  return lib;
}

// Once; call it on the sync sequence.
const Library* GetLibrary() {
  static const Library* library = LoadSyncLibrary();
  return library;
}


std::string ErrorJson(std::string_view message) {
  base::DictValue error;
  error.Set("error", message);
  return base::WriteJson(error).value_or("{}");
}

}  // namespace

// Sends the library's HTTP requests with the profile's network stack. The
// library calls Send() on the sync sequence, which waits for the UI thread.
class HttpBridge : public base::RefCountedThreadSafe<HttpBridge> {
 public:
  explicit HttpBridge(scoped_refptr<network::SharedURLLoaderFactory> factory)
      : factory_(std::move(factory)) {}

  std::string Send(std::string request) {
    base::WaitableEvent done;
    std::string response;
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(&HttpBridge::Start, this, std::move(request),
                       base::Unretained(&response), base::Unretained(&done)));
    // The sync sequence allows waiting; at shutdown it is abandoned.
    done.Wait();
    return response;
  }

 private:
  friend class base::RefCountedThreadSafe<HttpBridge>;
  ~HttpBridge() = default;

  static void Finish(std::string* response,
                     base::WaitableEvent* done,
                     std::string json) {
    *response = std::move(json);
    done->Signal();
  }

  void Start(std::string request,
             std::string* response,
             base::WaitableEvent* done) {
    std::optional<base::DictValue> req = base::JSONReader::ReadDict(
        request, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
    const std::string* url = req ? req->FindString("url") : nullptr;
    const std::string* method = req ? req->FindString("method") : nullptr;
    if (!url || !method) {
      Finish(response, done, ErrorJson("bad request"));
      return;
    }
    auto resource = std::make_unique<network::ResourceRequest>();
    resource->url = GURL(*url);
    resource->method = *method;
    resource->credentials_mode = network::mojom::CredentialsMode::kOmit;
    std::string content_type = "application/json";
    if (const base::DictValue* headers = req->FindDict("headers")) {
      for (auto [name, value] : *headers) {
        if (!value.is_string()) {
          continue;
        }
        if (base::EqualsCaseInsensitiveASCII(name, "content-type")) {
          content_type = value.GetString();
        } else {
          resource->headers.SetHeader(name, value.GetString());
        }
      }
    }
    std::unique_ptr<network::SimpleURLLoader> loader =
        network::SimpleURLLoader::Create(std::move(resource),
                                         kTrafficAnnotation);
    if (const std::string* body = req->FindString("body")) {
      loader->AttachStringForUpload(*body, content_type);
    }
    loader->SetAllowHttpErrorResults(true);
    loader->SetTimeoutDuration(base::Minutes(1));
    network::SimpleURLLoader* raw = loader.get();
    // Like Chrome Sync's HttpBridge: sync responses have no fixed size bound.
    raw->DownloadToStringOfUnboundedSizeUntilCrashAndDie(
        factory_.get(),
        base::BindOnce(&HttpBridge::OnResponse, std::move(loader), response,
                       done));
  }

  static void OnResponse(std::unique_ptr<network::SimpleURLLoader> loader,
                         std::string* response,
                         base::WaitableEvent* done,
                         std::optional<std::string> body) {
    const network::mojom::URLResponseHead* head = loader->ResponseInfo();
    if (!head || !head->headers) {
      Finish(response, done, ErrorJson(net::ErrorToString(loader->NetError())));
      return;
    }
    base::DictValue result;
    result.Set("status", head->headers->response_code());
    base::DictValue headers;
    size_t iter = 0;
    std::string name, value;
    while (head->headers->EnumerateHeaderLines(&iter, &name, &value)) {
      headers.Set(base::ToLowerASCII(name), value);
    }
    result.Set("headers", std::move(headers));
    result.Set("body", body.value_or(""));
    Finish(response, done, base::WriteJson(result).value_or("{}"));
  }

  scoped_refptr<network::SharedURLLoaderFactory> factory_;
};

namespace {

DISABLE_CFI_ICALL void HttpCallback(void* ctx,
                                    const char* request_json,
                                    void* out,
                                    HttpSetFn set) {
  std::string response =
      ctx ? static_cast<HttpBridge*>(ctx)->Send(request_json)
          : ErrorJson("no network context");
  set(out, response.c_str());
}

// On the sync sequence.
DISABLE_CFI_ICALL base::DictValue CallLibrary(scoped_refptr<HttpBridge> http,
                            std::string method,
                            std::string args) {
  const Library* lib = GetLibrary();
  if (!lib) {
    base::DictValue error;
    error.Set("error", "liboganesson_sync.so didn't load: " + LoadError());
    return error;
  }
  char* raw = lib->call(http.get(), method.c_str(), args.c_str());
  std::optional<base::DictValue> result = base::JSONReader::ReadDict(
      raw, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  lib->free(raw);
  if (!result) {
    result.emplace();
    result->Set("error", "bad result");
  }
  return std::move(*result);
}

}  // namespace

// Watches the sign-in tab for the redirect that carries the OAuth code.
class SigninWatcher : public content::WebContentsObserver {
 public:
  SigninWatcher(content::WebContents* contents,
                GURL redirect,
                FirefoxSyncService* service)
      : content::WebContentsObserver(contents),
        redirect_(std::move(redirect)),
        service_(service) {}

  void DidStartNavigation(content::NavigationHandle* handle) override {
    Check(handle->GetURL());
  }
  void DidRedirectNavigation(content::NavigationHandle* handle) override {
    Check(handle->GetURL());
  }

 private:
  void Check(const GURL& url) {
    GURL::Replacements clear;
    clear.ClearQuery();
    clear.ClearRef();
    if (url.ReplaceComponents(clear) != redirect_) {
      return;
    }
    std::string code, state;
    net::GetValueForKeyInQuery(url, "code", &code);
    net::GetValueForKeyInQuery(url, "state", &state);
    if (code.empty() || state.empty()) {
      return;
    }
    web_contents()->ClosePage();
    service_->CompleteSignIn(code, state);
  }

  GURL redirect_;
  raw_ptr<FirefoxSyncService> service_;
};

FirefoxSyncService::FirefoxSyncService(Profile* profile)
    : profile_(profile),
      sync_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::WithBaseSyncPrimitives(),
           base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN})),
      http_(base::MakeRefCounted<HttpBridge>(
          profile->GetDefaultStoragePartition()
              ->GetURLLoaderFactoryForBrowserProcess())) {
  // Accounts signed in before the server setting were Mozilla's.
  PrefService* prefs = profile_->GetPrefs();
  if (!prefs->GetString(kSecretsPref).empty() &&
      !prefs->HasPrefPath(kServerPref)) {
    prefs->SetString(kServerPref, kMozillaServer);
  }
  // Loads the library and proves it works.
  Call("version", {}, base::BindOnce([](base::DictValue result) {
         if (const std::string* version = result.FindString("ok")) {
           VLOG(1) << "Firefox Sync: " << *version;
         }
       }));
  g_browser_process->os_crypt_async()->GetInstance(base::BindOnce(
      &FirefoxSyncService::OnEncryptor, weak_factory_.GetWeakPtr()));
}

FirefoxSyncService::~FirefoxSyncService() = default;

// static
void FirefoxSyncService::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  registry->RegisterStringPref(kEmailPref, std::string());
  registry->RegisterStringPref(kSecretsPref, std::string());
  registry->RegisterStringPref(kServerPref, std::string());
  registry->RegisterStringPref(kSyncServerPref, std::string());
  registry->RegisterTimePref(kLastSyncPref, base::Time());
  registry->RegisterListPref(kDisabledTypesPref);
  registry->RegisterTimePref(kHistorySincePref, base::Time());
  registry->RegisterListPref(kHistoryDeletionsPref);
}

void FirefoxSyncService::Shutdown() {
  adapters_.clear();
  signin_watcher_.reset();
  periodic_.Stop();
  soon_.Stop();
  account_weak_factory_.InvalidateWeakPtrs();
  weak_factory_.InvalidateWeakPtrs();
}

void FirefoxSyncService::OnEncryptor(
    scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  encryptor_ = std::move(encryptor);
  std::string saved;
  if (base::Base64Decode(profile_->GetPrefs()->GetString(kSecretsPref),
                         &saved) &&
      !saved.empty()) {
    std::string json;
    std::optional<base::DictValue> secrets;
    if (encryptor_->DecryptString(saved, &json)) {
      secrets = base::JSONReader::ReadDict(json,
                                           base::JSON_PARSE_CHROMIUM_EXTENSIONS);
    }
    if (secrets) {
      secrets_ = std::move(*secrets);
    } else {
      error_ = "Couldn't decrypt the saved account; sign in again.";
      needs_sign_in_ = true;
    }
  }
  CreateAdapters();
  periodic_.Start(FROM_HERE, kSyncInterval,
                  base::BindRepeating(&FirefoxSyncService::SyncNow,
                                      base::Unretained(this)));
  if (signed_in()) {
    ScheduleSync(kStartupDelay);
  }
  NotifyStatus();
}

void FirefoxSyncService::Call(const std::string& method,
                              base::DictValue args,
                              base::OnceCallback<void(base::DictValue)> reply) {
  args.Set("profile", profile_->GetPath().value());
  sync_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&CallLibrary, http_, method,
                     base::WriteJson(args).value_or("{}")),
      std::move(reply));
}

bool FirefoxSyncService::HasServer() const {
  return !profile_->GetPrefs()->GetString(kServerPref).empty() ||
         !base::Environment::Create()
              ->GetVar("OGANESSON_FXA_SERVER")
              .value_or("")
              .empty();
}

void FirefoxSyncService::SetServer(std::string_view input, bool sync_storage) {
  const char* pref = sync_storage ? kSyncServerPref : kServerPref;
  std::string text(base::TrimWhitespaceASCII(input, base::TRIM_ALL));
  std::string server;
  if (!text.empty()) {
    GURL url(text.find("://") == std::string::npos ? "https://" + text : text);
    if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS() || !url.has_host()) {
      error_ = "Not a server URL: " + text;
      NotifyStatus();
      return;
    }
    // The sync server can live under a path; the accounts server is an
    // origin.
    server = sync_storage ? std::string(base::TrimString(url.spec(), "/",
                                                         base::TRIM_TRAILING))
                          : url::Origin::Create(url).Serialize();
  }
  if (server == profile_->GetPrefs()->GetString(pref)) {
    NotifyStatus();
    return;
  }
  // Keys and tokens belong to one server; this also drops a sign-in that's
  // under way.
  SignOut();
  profile_->GetPrefs()->SetString(pref, server);
  error_.clear();
  NotifyStatus();
}

void FirefoxSyncService::SignIn() {
  if (!HasServer()) {
    error_ = "Set a sync server to sign in.";
    NotifyStatus();
    return;
  }
  base::DictValue args;
  args.Set("server", profile_->GetPrefs()->GetString(kServerPref));
  args.Set("token_server", profile_->GetPrefs()->GetString(kSyncServerPref));
  Call("begin_signin", std::move(args),
       base::BindOnce(&FirefoxSyncService::OnBeginSignIn,
                      account_weak_factory_.GetWeakPtr()));
}

void FirefoxSyncService::OnBeginSignIn(base::DictValue result) {
  const base::DictValue* ok = result.FindDict("ok");
  const std::string* url = ok ? ok->FindString("url") : nullptr;
  const std::string* redirect = ok ? ok->FindString("redirect") : nullptr;
  if (!url || !redirect) {
    SetError(result);
    return;
  }
  // Android's Navigate() needs a browser window, which a profile alone
  // doesn't give: the tab comes from the profile's tab model.
  for (TabModel* model : TabModelList::models()) {
    if (model->GetProfile() != profile_ || model->IsOffTheRecord()) {
      continue;
    }
    content::WebContents* contents =
        model->CreateNewTabForDevTools(GURL(*url), /*new_window=*/false);
    if (contents) {
      signin_watcher_ =
          std::make_unique<SigninWatcher>(contents, GURL(*redirect), this);
      error_.clear();
    }
    NotifyStatus();
    return;
  }
  error_ = "No browser window to sign in from.";
  NotifyStatus();
}

void FirefoxSyncService::CompleteSignIn(const std::string& code,
                                        const std::string& state) {
  // Called from the watcher, which must not be deleted under itself.
  base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
      FROM_HERE, std::move(signin_watcher_));
  base::DictValue args;
  args.Set("code", code);
  args.Set("state", state);
  Call("complete_signin", std::move(args),
       base::BindOnce(&FirefoxSyncService::OnCompleteSignIn,
                      account_weak_factory_.GetWeakPtr()));
}

void FirefoxSyncService::OnCompleteSignIn(base::DictValue result) {
  const base::DictValue* ok = result.FindDict("ok");
  const std::string* account = ok ? ok->FindString("account") : nullptr;
  if (!account) {
    SetError(result);
    return;
  }
  // A new sign-in starts a new sync state: the engine merges both sides.
  secrets_ = base::DictValue();
  secrets_.Set("account", *account);
  const std::string* email = ok->FindString("email");
  profile_->GetPrefs()->SetString(kEmailPref, email ? *email : "");
  error_.clear();
  needs_sign_in_ = false;
  SaveSecrets();
  SyncNow();
}

void FirefoxSyncService::SignOut() {
  base::DictValue args;
  if (const std::string* account = secrets_.FindString("account")) {
    args.Set("account", *account);
  }
  Call("signout", std::move(args), base::DoNothing());
  // Replies to the signed-out account's sign-in and syncs, still running in
  // the library, would bring it back.
  account_weak_factory_.InvalidateWeakPtrs();
  signin_watcher_.reset();
  soon_.Stop();
  syncing_ = false;
  sync_again_ = false;
  retry_after_ = base::Time();
  pending_engines_ = base::DictValue();
  secrets_ = base::DictValue();
  profile_->GetPrefs()->ClearPref(kSecretsPref);
  profile_->GetPrefs()->ClearPref(kEmailPref);
  profile_->GetPrefs()->ClearPref(kLastSyncPref);
  profile_->GetPrefs()->ClearPref(kHistorySincePref);
  profile_->GetPrefs()->ClearPref(kHistoryDeletionsPref);
  error_.clear();
  needs_sign_in_ = false;
  type_status_.clear();
  remote_tabs_.clear();
  NotifyStatus();
}

bool FirefoxSyncService::IsTypeEnabled(const std::string& name) const {
  const base::ListValue& disabled =
      profile_->GetPrefs()->GetList(kDisabledTypesPref);
  return !disabled.contains(name);
}

void FirefoxSyncService::SetTypeEnabled(const std::string& name,
                                        bool enabled) {
  // Adapters are only swapped between syncs.
  if (syncing_) {
    NotifyStatus();
    return;
  }
  {
    ScopedListPrefUpdate disabled(profile_->GetPrefs(), kDisabledTypesPref);
    disabled->EraseValue(base::Value(name));
    if (!enabled) {
      disabled->Append(name);
    }
  }
  CreateAdapters();
  if (name == "tabs" && !enabled) {
    remote_tabs_.clear();
  }
  if (enabled) {
    SyncNow();
  }
  NotifyStatus();
}

void FirefoxSyncService::CreateAdapters() {
  auto local_changed = base::BindRepeating(&FirefoxSyncService::OnLocalChange,
                                           base::Unretained(this));
  for (const DataType& type : kTypes) {
    if (!type.supported || !IsTypeEnabled(type.name)) {
      adapters_.erase(type.name);
      continue;
    }
    if (adapters_.contains(type.name)) {
      continue;
    }
    std::string name = type.name;
    if (name == "passwords") {
      auto store = ProfilePasswordStoreFactory::GetForProfile(
          profile_, ServiceAccessType::EXPLICIT_ACCESS);
      if (store) {
        adapters_[name] =
            std::make_unique<PasswordsAdapter>(std::move(store), local_changed);
      }
    } else if (name == "bookmarks") {
      adapters_[name] = std::make_unique<BookmarksAdapter>(
          BookmarkModelFactory::GetForBrowserContext(profile_), local_changed);
    } else if (name == "history") {
      if (auto* history = HistoryServiceFactory::GetForProfile(
              profile_, ServiceAccessType::EXPLICIT_ACCESS)) {
        adapters_[name] = std::make_unique<HistoryAdapter>(
            history, profile_->GetPrefs(), local_changed);
      }
    } else if (name == "tabs") {
      adapters_[name] = std::make_unique<TabsAdapter>(profile_, local_changed);
    } else if (name == "addresses") {
      if (auto* pdm = autofill::PersonalDataManagerFactory::GetForBrowserContext(
              profile_)) {
        adapters_[name] = std::make_unique<AddressesAdapter>(
            &pdm->address_data_manager(), local_changed);
      }
    } else if (name == "creditcards") {
      if (auto* pdm = autofill::PersonalDataManagerFactory::GetForBrowserContext(
              profile_)) {
        adapters_[name] = std::make_unique<CreditCardsAdapter>(
            &pdm->payments_data_manager(), local_changed);
      }
    }
  }
}

void FirefoxSyncService::SyncNow() {
  if (!signed_in() || !encryptor_ || adapters_.empty()) {
    return;
  }
  if (syncing_) {
    sync_again_ = true;
    return;
  }
  // Every sync, Sync now included, waits for the servers' backoff.
  if (const base::TimeDelta wait = retry_after_ - base::Time::Now();
      wait.is_positive()) {
    ScheduleSync(wait);
    return;
  }
  syncing_ = true;
  NotifyStatus();
  // Collect every adapter's snapshot, then sync them together.
  pending_engines_ = base::DictValue();
  base::RepeatingClosure all_local = base::BarrierClosure(
      adapters_.size(), base::BindOnce(&FirefoxSyncService::OnAllLocal,
                                       account_weak_factory_.GetWeakPtr()));
  for (auto& [name, adapter] : adapters_) {
    adapter->GetLocal(base::BindOnce(&FirefoxSyncService::OnLocal,
                                     account_weak_factory_.GetWeakPtr(), name,
                                     all_local));
  }
}

void FirefoxSyncService::OnLocal(const std::string& name,
                                 base::RepeatingClosure done,
                                 base::Value local) {
  if (local.is_none()) {
    type_status_[name].error = "Couldn't read the local data.";
  } else if (local.is_string()) {
    type_status_[name].error = local.GetString();
  } else {
    const base::DictValue* engines = secrets_.FindDict("engines");
    const std::string* state = engines ? engines->FindString(name) : nullptr;
    base::DictValue engine;
    engine.Set("state", state ? *state : "");
    engine.Set("local", std::move(local));
    pending_engines_.Set(name, std::move(engine));
  }
  done.Run();
}

void FirefoxSyncService::OnAllLocal() {
  base::DictValue args;
  for (const char* field : {"account", "global"}) {
    if (const std::string* value = secrets_.FindString(field)) {
      args.Set(field, *value);
    }
  }
  args.Set("engines", std::move(pending_engines_));
  Call("sync", std::move(args),
       base::BindOnce(&FirefoxSyncService::OnSynced,
                      account_weak_factory_.GetWeakPtr()));
}

void FirefoxSyncService::OnSynced(base::DictValue result) {
  syncing_ = false;
  const base::DictValue* reply = result.FindDict("ok");
  const std::optional<double> retry_after =
      (reply ? reply : &result)->FindDouble("retry_after");
  retry_after_ = retry_after
                     ? base::Time::FromMillisecondsSinceUnixEpoch(*retry_after)
                     : base::Time();
  if (base::DictValue* ok = result.FindDict("ok")) {
    for (const char* field : {"account", "global"}) {
      if (const std::string* value = ok->FindString(field)) {
        secrets_.Set(field, *value);
      }
    }
    base::DictValue* engine_states = secrets_.EnsureDict("engines");
    if (const base::DictValue* engines = ok->FindDict("engines")) {
      for (auto [name, value] : *engines) {
        const base::DictValue* engine = value.GetIfDict();
        TypeStatus& status = type_status_[name];
        const std::string* error =
            engine ? engine->FindString("error") : nullptr;
        if (!engine || error) {
          status.error = error ? *error : "Unknown error";
          LOG(WARNING) << "Firefox Sync: " << name << ": " << status.error;
          continue;
        }
        status.error.clear();
        status.count =
            static_cast<int>(engine->FindDouble("count").value_or(0));
        auto adapter = adapters_.find(name);
        const base::ListValue* ops = engine->FindList("ops");
        if (name == "tabs" && ops) {
          remote_tabs_ = ops->Clone();
        }
        if (adapter != adapters_.end() && ops) {
          adapter->second->Apply(*ops);
        }
        if (const std::string* state = engine->FindString("state")) {
          engine_states->Set(name, *state);
        }
      }
    }
    SaveSecrets();
    profile_->GetPrefs()->SetTime(kLastSyncPref, base::Time::Now());
    error_.clear();
    needs_sign_in_ = false;
  } else {
    SetError(result);
  }
  NotifyStatus();
  if (sync_again_) {
    sync_again_ = false;
    ScheduleSync(base::TimeDelta());
  }
}

void FirefoxSyncService::SaveSecrets() {
  std::string encrypted;
  if (!encryptor_ ||
      !encryptor_->EncryptString(base::WriteJson(secrets_).value_or("{}"),
                                 &encrypted)) {
    error_ = "Couldn't encrypt the account state.";
    return;
  }
  profile_->GetPrefs()->SetString(kSecretsPref,
                                  base::Base64Encode(encrypted));
}

void FirefoxSyncService::SetError(const base::DictValue& result) {
  const std::string* error = result.FindString("error");
  error_ = error ? *error : "Unknown error";
  needs_sign_in_ = result.FindBool("auth").value_or(false);
  LOG(WARNING) << "Firefox Sync: " << error_;
  NotifyStatus();
}

void FirefoxSyncService::ScheduleSync(base::TimeDelta delay) {
  soon_.Start(FROM_HERE, delay,
              base::BindOnce(&FirefoxSyncService::SyncNow,
                             base::Unretained(this)));
}

void FirefoxSyncService::OnLocalChange() {
  // Debounced. Applying a sync's own changes also lands here; that next
  // sync finds nothing to do.
  if (!signed_in()) {
    return;
  }
  if (syncing_) {
    sync_again_ = true;
  } else {
    ScheduleSync(kLocalChangeDelay);
  }
}

base::DictValue FirefoxSyncService::GetStatus() const {
  base::DictValue status;
  status.Set("signedIn", signed_in());
  status.Set("email", profile_->GetPrefs()->GetString(kEmailPref));
  status.Set("syncing", syncing_);
  base::Time last = profile_->GetPrefs()->GetTime(kLastSyncPref);
  status.Set("lastSync", last.is_null() ? 0.0
                                        : last.InMillisecondsFSinceUnixEpoch());
  status.Set("error", error_);
  status.Set("needsSignIn", needs_sign_in_);
  status.Set("server", profile_->GetPrefs()->GetString(kServerPref));
  status.Set("syncServer", profile_->GetPrefs()->GetString(kSyncServerPref));
  status.Set("canSignIn", HasServer());
  status.Set("signingIn", !!signin_watcher_);
  base::ListValue types;
  for (const DataType& type : kTypes) {
    base::DictValue entry;
    entry.Set("name", type.name);
    entry.Set("label", type.label);
    entry.Set("supported", type.supported);
    entry.Set("enabled", type.supported && IsTypeEnabled(type.name));
    auto status_it = type_status_.find(type.name);
    if (status_it != type_status_.end()) {
      entry.Set("count", status_it->second.count);
      entry.Set("error", status_it->second.error);
    }
    types.Append(std::move(entry));
  }
  status.Set("types", std::move(types));
  status.Set("remoteTabs", remote_tabs_.Clone());
  return status;
}

base::CallbackListSubscription FirefoxSyncService::AddStatusObserver(
    base::RepeatingClosure callback) {
  return status_observers_.Add(std::move(callback));
}

void FirefoxSyncService::NotifyStatus() {
  status_observers_.Notify();
}

}  // namespace firefox_sync
