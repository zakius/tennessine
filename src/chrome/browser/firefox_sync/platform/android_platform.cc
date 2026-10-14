// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Firefox Sync's Android side (see dubna's firefox_sync_platform.h). Android
// has no synced tabs views and no account picture on the profile, so those
// stay Platform's no-ops.

#include <memory>
#include <string>
#include <utility>

#include "base/android/bundle_utils.h"
#include "base/memory/raw_ptr.h"
#include "chrome/browser/firefox_sync/firefox_sync_platform.h"
#include "chrome/browser/firefox_sync/platform/tabs_adapter_android.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/android/tab_model/tab_model.h"
#include "chrome/browser/ui/android/tab_model/tab_model_list.h"
#include "url/gurl.h"

namespace firefox_sync {

namespace {

class AndroidPlatform : public Platform {
 public:
  explicit AndroidPlatform(Profile* profile) : profile_(profile) {}

  std::unique_ptr<SyncAdapter> CreateTabsAdapter(
      base::RepeatingClosure local_changed) override {
    return std::make_unique<TabsAdapter>(profile_, std::move(local_changed));
  }

  // Android's Navigate() needs a browser window, which a profile alone
  // doesn't give: the tab comes from the profile's tab model.
  content::WebContents* OpenTab(const GURL& url) override {
    for (TabModel* model : TabModelList::models()) {
      if (model->GetProfile() == profile_ && !model->IsOffTheRecord()) {
        return model->CreateNewTabForDevTools(url, /*new_window=*/false);
      }
    }
    return nullptr;
  }

 private:
  const raw_ptr<Profile> profile_;
};

}  // namespace

std::unique_ptr<Platform> CreatePlatform(Profile* profile) {
  return std::make_unique<AndroidPlatform>(profile);
}

base::FilePath SyncLibraryPath() {
  // The linker namespace libchrome.so is in doesn't search the APK, so the
  // path comes from the class loader, as for Chromium's own module libraries.
  // The library brings its own NSS (Chromium on Android has none).
  std::string path =
      base::android::BundleUtils::ResolveLibraryPath("oganesson_sync", "");
  return base::FilePath(path.empty() ? "liboganesson_sync.so" : path);
}

}  // namespace firefox_sync
