// Copyright 2026 The oganesson Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/firefox_sync/android/tabs_adapter_android.h"

#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/android/tab_android.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/android/tab_model/tab_model_list.h"
#include "url/gurl.h"

namespace firefox_sync {

TabsAdapter::TabsAdapter(Profile* profile, base::RepeatingClosure local_changed)
    : profile_(profile), local_changed_(std::move(local_changed)) {
  TabModelList::AddObserver(this);
  for (TabModel* model : TabModelList::models()) {
    OnTabModelAdded(model);
  }
}

TabsAdapter::~TabsAdapter() {
  for (TabModel* model : models_) {
    model->RemoveObserver(this);
  }
  TabModelList::RemoveObserver(this);
}

bool TabsAdapter::ShouldTrack(TabModel* model) const {
  // Incognito and archived tabs stay local.
  return model->GetProfile() == profile_ && !model->IsOffTheRecord() &&
         model->GetTabModelType() == TabModel::TabModelType::kStandard;
}

void TabsAdapter::GetLocal(base::OnceCallback<void(base::Value)> done) {
  base::ListValue tabs;
  for (TabModel* model : models_) {
    for (int i = 0; i < model->GetTabCount(); ++i) {
      // Null for a tab that isn't initialized yet (restored but never shown).
      TabAndroid* tab = model->GetTabAt(i);
      if (!tab) {
        continue;
      }
      const GURL url = tab->GetURL();
      if (!url.SchemeIsHTTPOrHTTPS()) {
        continue;
      }
      base::DictValue entry;
      entry.Set("url", url.spec());
      entry.Set("title", base::UTF16ToUTF8(tab->GetTitle()));
      entry.Set("lastUsed",
                static_cast<double>(tab->GetLastShownTimestamp()
                                        .InMillisecondsSinceUnixEpoch()));
      entry.Set("pinned", tab->IsPinned());
      tabs.Append(std::move(entry));
    }
  }
  std::move(done).Run(base::Value(std::move(tabs)));
}

void TabsAdapter::Apply(const base::ListValue& ops) {}

void TabsAdapter::OnTabModelAdded(TabModel* model) {
  if (ShouldTrack(model) && models_.insert(model).second) {
    model->AddObserver(this);
    local_changed_.Run();
  }
}

void TabsAdapter::OnTabModelRemoved(TabModel* model) {
  if (models_.erase(model)) {
    model->RemoveObserver(this);
    local_changed_.Run();
  }
}

void TabsAdapter::DidAddTab(TabAndroid* tab, TabModel::TabLaunchType type) {
  local_changed_.Run();
}

void TabsAdapter::DidMoveTab(TabAndroid* tab, int new_index, int old_index) {
  local_changed_.Run();
}

void TabsAdapter::DidSelectTab(TabAndroid* tab,
                               TabModel::TabSelectionType type) {
  local_changed_.Run();
}

void TabsAdapter::OnTabCloseCommitted(const std::vector<TabAndroid*>& tabs,
                                      bool is_all_tabs,
                                      bool can_restore,
                                      TabModel::TabClosingSource source) {
  local_changed_.Run();
}

}  // namespace firefox_sync
