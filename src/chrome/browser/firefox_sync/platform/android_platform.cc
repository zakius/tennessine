// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Firefox Sync's Android side (see dubna's firefox_sync_platform.h), and the
// JNI for SendTab.java, which sends tabs to other devices. Android has no
// synced tabs views and no account picture on the profile, so those stay
// Platform's no-ops.

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/android/bundle_utils.h"
#include "base/android/jni_android.h"
#include "base/android/jni_string.h"
#include "base/json/json_writer.h"
#include "base/memory/raw_ptr.h"
#include "chrome/browser/android/tab_android.h"
#include "chrome/browser/firefox_sync/firefox_sync_platform.h"
#include "chrome/browser/firefox_sync/firefox_sync_service.h"
#include "chrome/browser/firefox_sync/firefox_sync_service_factory.h"
#include "chrome/browser/firefox_sync/platform/tabs_adapter_android.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/android/tab_model/tab_model.h"
#include "chrome/browser/ui/android/tab_model/tab_model_list.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

// Must come after all headers that specialize FromJniType() / ToJniType().
#include "chrome/browser/firefox_sync/android/jni_headers/SendTab_jni.h"

namespace firefox_sync {

namespace {

FirefoxSyncService* GetService(Profile* profile) {
  return profile ? FirefoxSyncServiceFactory::GetForProfile(profile) : nullptr;
}

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
    TabModel* model = MainTabModel();
    return model ? model->CreateNewTabForDevTools(url, /*new_window=*/false)
                 : nullptr;
  }

  // With no window open, only the notification: tapping it opens the tab.
  void OpenReceivedTab(const std::string& from,
                       const GURL& url,
                       const std::string& title) override {
    if (TabModel* model = MainTabModel()) {
      std::unique_ptr<content::WebContents> owned =
          content::WebContents::Create(
              content::WebContents::CreateParams(profile_));
      content::WebContents* contents = owned.get();
      if (model->CreateTab(/*parent=*/nullptr, std::move(owned),
                           TabModel::kInvalidIndex,
                           TabModel::TabLaunchType::FROM_SYNC_BACKGROUND,
                           /*should_pin=*/false)) {
        contents->GetController().LoadURLWithParams(
            content::NavigationController::LoadURLParams(url));
      }
    }
    Java_SendTab_showReceivedTab(base::android::AttachCurrentThread(), from,
                                 url.spec(), title);
  }

  void CloseTabs(const std::string& from,
                 const std::vector<GURL>& urls) override {
    for (TabModel* model : TabModelList::models()) {
      if (!IsMainTabModel(model)) {
        continue;
      }
      // Backwards: closing a tab moves the ones after it.
      for (int i = model->GetTabCount() - 1; i >= 0; --i) {
        // Null for a tab that isn't initialized yet, which Firefox never
        // saw: the tabs engine skips it too.
        TabAndroid* tab = model->GetTabAt(i);
        if (tab && std::ranges::contains(urls, tab->GetURL())) {
          model->CloseTabAt(i);
        }
      }
    }
  }

 private:
  // The profile's tabs in a browser window, not incognito's or archived ones:
  // the tabs Firefox sees.
  bool IsMainTabModel(TabModel* model) const {
    return model->GetProfile() == profile_ && !model->IsOffTheRecord() &&
           model->GetTabModelType() == TabModel::TabModelType::kStandard;
  }

  // Null with no window open.
  TabModel* MainTabModel() const {
    for (TabModel* model : TabModelList::models()) {
      if (IsMainTabModel(model)) {
        return model;
      }
    }
    return nullptr;
  }

  const raw_ptr<Profile> profile_;
};

}  // namespace

static std::string JNI_SendTab_GetDevices(JNIEnv* env, Profile* profile) {
  FirefoxSyncService* service = GetService(profile);
  return service ? base::WriteJson(service->devices()).value_or("[]") : "[]";
}

static void JNI_SendTab_SendTab(JNIEnv* env,
                                Profile* profile,
                                const std::string& device_id,
                                const std::string& url,
                                const std::string& title) {
  if (auto* service = GetService(profile)) {
    service->SendTab(device_id, GURL(url), title);
  }
}

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

DEFINE_JNI(SendTab)
