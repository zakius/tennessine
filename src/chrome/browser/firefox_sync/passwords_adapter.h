// Copyright 2026 The oganesson Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_FIREFOX_SYNC_PASSWORDS_ADAPTER_H_
#define CHROME_BROWSER_FIREFOX_SYNC_PASSWORDS_ADAPTER_H_

#include <set>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/firefox_sync/sync_adapter.h"
#include "components/password_manager/core/browser/password_store/password_store_consumer.h"
#include "components/password_manager/core/browser/password_store/password_store_interface.h"

namespace firefox_sync {

// `passwords`: the profile's password store (not the account store). Web
// form and HTTP auth logins map to Firefox login records; blocked sites,
// federated and app credentials stay local. Ops are keyed by Firefox's
// (hostname, httpRealm, username), and apply to every local login with that
// key.
class PasswordsAdapter : public SyncAdapter,
                         public password_manager::PasswordStoreInterface::Observer,
                         public password_manager::PasswordStoreConsumer {
 public:
  PasswordsAdapter(scoped_refptr<password_manager::PasswordStoreInterface> store,
                   base::RepeatingClosure local_changed);
  ~PasswordsAdapter() override;

  // SyncAdapter:
  void GetLocal(base::OnceCallback<void(base::Value)> done) override;
  void Apply(const base::ListValue& ops) override;

 private:
  // PasswordStoreInterface::Observer:
  void OnLoginsChanged(
      password_manager::PasswordStoreInterface* store,
      const password_manager::PasswordStoreChangeList& changes) override;
  void OnLoginsRetained(password_manager::PasswordStoreInterface* store,
                        const std::vector<password_manager::StoredCredential>&
                            retained) override;

  // PasswordStoreConsumer:
  void OnGetPasswordStoreResultsOrErrorFrom(
      password_manager::PasswordStoreInterface* store,
      password_manager::LoginsResultOrError results) override;

  scoped_refptr<password_manager::PasswordStoreInterface> store_;
  base::RepeatingClosure local_changed_;
  base::OnceCallback<void(base::Value)> get_local_;
  // The snapshot the running sync was given; ops refer to its logins.
  std::vector<password_manager::StoredCredential> snapshot_;
  // Between the snapshot and Apply: the engine keys of logins changed locally.
  // Apply leaves them alone, so a newer local edit isn't overwritten with the
  // merge of the older snapshot; the change schedules the next sync anyway.
  bool syncing_ = false;
  std::set<std::string> changed_during_sync_;
  base::WeakPtrFactory<PasswordsAdapter> weak_factory_{this};
};

}  // namespace firefox_sync

#endif  // CHROME_BROWSER_FIREFOX_SYNC_PASSWORDS_ADAPTER_H_
