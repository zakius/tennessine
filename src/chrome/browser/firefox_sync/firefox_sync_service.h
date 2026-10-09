// Copyright 2026 The oganesson Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_FIREFOX_SYNC_FIREFOX_SYNC_SERVICE_H_
#define CHROME_BROWSER_FIREFOX_SYNC_FIREFOX_SYNC_SERVICE_H_

#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "components/keyed_service/core/keyed_service.h"

class Profile;

namespace base {
class SequencedTaskRunner;
}

namespace os_crypt_async {
class Encryptor;
}

namespace user_prefs {
class PrefRegistrySyncable;
}

namespace firefox_sync {

class HttpBridge;
class SigninWatcher;
class SyncAdapter;

// Firefox Sync for a profile. Protocol, crypto and merging run in
// liboganesson_sync.so, Mozilla's application-services crates behind a small
// C API, on a background sequence. This class owns what all data types
// share: the Mozilla account (encrypted with OSCrypt in prefs), the sync
// state, per-type switches and scheduling. Each enabled type has a
// SyncAdapter that snapshots the browser's data for the library's engine
// and applies the changes it returns; all types sync in one pass.
class FirefoxSyncService : public KeyedService {
 public:
  explicit FirefoxSyncService(Profile* profile);
  ~FirefoxSyncService() override;

  static void RegisterProfilePrefs(user_prefs::PrefRegistrySyncable* registry);

  // Opens the Mozilla account sign-in page in a new tab; syncs once it
  // redirects back.
  void SignIn();
  void SignOut();
  // Sets the accounts server, or with `sync_storage` the sync (token) server,
  // from what the user typed: a URL, or a host (https:// assumed); empty for
  // none, which for the sync server means the one the accounts server names.
  // A new server signs out first.
  void SetServer(std::string_view input, bool sync_storage = false);
  void SyncNow();
  void SetTypeEnabled(const std::string& name, bool enabled);

  // {signedIn, email, syncing, lastSync (ms), error, needsSignIn, server,
  // syncServer, canSignIn, signingIn, types:
  // [{name, label, supported, enabled, count?, error?}]} for settings.
  base::DictValue GetStatus() const;
  base::CallbackListSubscription AddStatusObserver(
      base::RepeatingClosure callback);

  // Called by SigninWatcher with the redirect's query values.
  void CompleteSignIn(const std::string& code, const std::string& state);

  // KeyedService:
  void Shutdown() override;

 private:
  struct TypeStatus {
    int count = 0;
    std::string error;
  };

  void OnEncryptor(scoped_refptr<os_crypt_async::Encryptor> encryptor);
  // Runs `method` in the library on the sync sequence, then `reply` on the
  // UI thread with its {"ok"} or {"error"} result.
  void Call(const std::string& method,
            base::DictValue args,
            base::OnceCallback<void(base::DictValue)> reply);
  void OnBeginSignIn(base::DictValue result);
  void OnCompleteSignIn(base::DictValue result);
  bool IsTypeEnabled(const std::string& name) const;
  void CreateAdapters();
  void OnLocal(const std::string& name,
               base::RepeatingClosure done,
               base::Value local);
  void OnAllLocal();
  void OnSynced(base::DictValue result);
  void OnLocalChange();
  void SaveSecrets();
  void SetError(const base::DictValue& result);
  void ScheduleSync(base::TimeDelta delay);
  void NotifyStatus();
  bool signed_in() const { return secrets_.FindString("account"); }
  // A server is set, or OGANESSON_FXA_SERVER overrides it.
  bool HasServer() const;

  const raw_ptr<Profile> profile_;
  scoped_refptr<base::SequencedTaskRunner> sync_runner_;
  scoped_refptr<HttpBridge> http_;
  scoped_refptr<os_crypt_async::Encryptor> encryptor_;

  // {account, global, engines: {name: state}}: the library's saved account
  // (tokens, keys) and sync state. Only ever stored encrypted.
  base::DictValue secrets_;
  // By collection name, for the enabled supported types.
  std::map<std::string, std::unique_ptr<SyncAdapter>> adapters_;
  std::map<std::string, TypeStatus> type_status_;
  // The running sync's {name: {state, local}}, as snapshots come in.
  base::DictValue pending_engines_;
  base::ListValue remote_tabs_;

  bool syncing_ = false;
  bool sync_again_ = false;
  // No sync before this time: the servers asked to back off until then.
  base::Time retry_after_;
  std::string error_;
  bool needs_sign_in_ = false;

  std::unique_ptr<SigninWatcher> signin_watcher_;
  base::RepeatingTimer periodic_;
  base::OneShotTimer soon_;
  base::RepeatingClosureList status_observers_;
  // For replies of the signed-in account's operations (sign-in, syncs):
  // signing out invalidates them.
  base::WeakPtrFactory<FirefoxSyncService> account_weak_factory_{this};
  base::WeakPtrFactory<FirefoxSyncService> weak_factory_{this};
};

}  // namespace firefox_sync

#endif  // CHROME_BROWSER_FIREFOX_SYNC_FIREFOX_SYNC_SERVICE_H_
