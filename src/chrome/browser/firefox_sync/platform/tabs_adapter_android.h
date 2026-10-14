// Copyright 2026 The oganesson Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_FIREFOX_SYNC_PLATFORM_TABS_ADAPTER_ANDROID_H_
#define CHROME_BROWSER_FIREFOX_SYNC_PLATFORM_TABS_ADAPTER_ANDROID_H_

#include <set>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "chrome/browser/firefox_sync/sync_adapter.h"
#include "chrome/browser/ui/android/tab_model/tab_model.h"
#include "chrome/browser/ui/android/tab_model/tab_model_list_observer.h"
#include "chrome/browser/ui/android/tab_model/tab_model_observer.h"

class Profile;

namespace firefox_sync {

// Open tabs on Android: the profile's normal tab models, one per window.
class TabsAdapter : public SyncAdapter,
                    public TabModelListObserver,
                    public TabModelObserver {
 public:
  TabsAdapter(Profile* profile, base::RepeatingClosure local_changed);
  ~TabsAdapter() override;

  void GetLocal(base::OnceCallback<void(base::Value)> done) override;
  void Apply(const base::ListValue& ops) override;

 private:
  bool ShouldTrack(TabModel* model) const;

  // TabModelListObserver:
  void OnTabModelAdded(TabModel* model) override;
  void OnTabModelRemoved(TabModel* model) override;

  // TabModelObserver. A navigation inside a tab has no event here, so it
  // reaches Firefox with the next scheduled sync.
  // ponytail: no per-tab WebContentsObserver; add one if 30 minutes is too
  // stale for the tab list.
  void DidAddTab(TabAndroid* tab, TabModel::TabLaunchType type) override;
  void DidMoveTab(TabAndroid* tab, int new_index, int old_index) override;
  void DidSelectTab(TabAndroid* tab, TabModel::TabSelectionType type) override;
  void OnTabCloseCommitted(const std::vector<TabAndroid*>& tabs,
                           bool is_all_tabs,
                           bool can_restore,
                           TabModel::TabClosingSource source) override;

  const raw_ptr<Profile> profile_;
  base::RepeatingClosure local_changed_;
  std::set<raw_ptr<TabModel>> models_;
};

}  // namespace firefox_sync

#endif  // CHROME_BROWSER_FIREFOX_SYNC_PLATFORM_TABS_ADAPTER_ANDROID_H_
