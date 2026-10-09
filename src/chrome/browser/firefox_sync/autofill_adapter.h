// Copyright 2026 The oganesson Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_FIREFOX_SYNC_AUTOFILL_ADAPTER_H_
#define CHROME_BROWSER_FIREFOX_SYNC_AUTOFILL_ADAPTER_H_

#include <set>
#include <string>

#include "base/memory/raw_ptr.h"
#include "chrome/browser/firefox_sync/sync_adapter.h"
#include "components/autofill/core/browser/data_manager/addresses/address_data_manager.h"
#include "components/autofill/core/browser/data_manager/payments/payments_data_manager.h"

namespace firefox_sync {

// `addresses`: the profile's local addresses (account addresses are
// Google's). Records carry the address's GUID and Firefox's field names;
// see autofill_sync.rs for the snapshot and the ops.
class AddressesAdapter : public SyncAdapter,
                         public autofill::AddressDataManager::Observer {
 public:
  AddressesAdapter(autofill::AddressDataManager* manager,
                   base::RepeatingClosure local_changed);
  ~AddressesAdapter() override;

  // SyncAdapter:
  void GetLocal(base::OnceCallback<void(base::Value)> done) override;
  void Apply(const base::ListValue& ops) override;

 private:
  // AddressDataManager::Observer:
  void OnAddressDataChanged() override;

  raw_ptr<autofill::AddressDataManager> manager_;
  base::RepeatingClosure local_changed_;
  // The GUIDs the running sync's snapshot had. One missing from the manager
  // was deleted since, and isn't recreated.
  std::set<std::string> snapshot_;
};

// `creditcards`: the profile's local cards (server cards are Google's), as
// `addresses`. Nicknames and CVCs stay local.
class CreditCardsAdapter : public SyncAdapter,
                           public autofill::PaymentsDataManager::Observer {
 public:
  CreditCardsAdapter(autofill::PaymentsDataManager* manager,
                     base::RepeatingClosure local_changed);
  ~CreditCardsAdapter() override;

  // SyncAdapter:
  void GetLocal(base::OnceCallback<void(base::Value)> done) override;
  void Apply(const base::ListValue& ops) override;

 private:
  // PaymentsDataManager::Observer:
  void OnPaymentsDataChanged() override;

  raw_ptr<autofill::PaymentsDataManager> manager_;
  base::RepeatingClosure local_changed_;
  std::set<std::string> snapshot_;
};

}  // namespace firefox_sync

#endif  // CHROME_BROWSER_FIREFOX_SYNC_AUTOFILL_ADAPTER_H_
