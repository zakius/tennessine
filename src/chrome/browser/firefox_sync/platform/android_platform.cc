// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Firefox Sync's Android side (see dubna's firefox_sync_platform.h), the JNI
// for SendTab.java, which sends tabs to other devices, and the fetches of the
// tabs other devices sent: when the browser comes to the front, in
// ReceiveTabsTask.java's background task, and on a push through Sunup
// (SunupPush.java). Other devices' tabs need nothing here: dubna's RemoteTabs
// feeds them to Recent tabs through Chromium's synced sessions. Android has no
// account picture on the profile, so that stays Platform's no-op.

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/android/application_status_listener.h"
#include "base/android/bundle_utils.h"
#include "base/android/jni_android.h"
#include "base/android/jni_string.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/values.h"
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
#include "chrome/browser/firefox_sync/android/jni_headers/ReceiveTabsTask_jni.h"
#include "chrome/browser/firefox_sync/android/jni_headers/SendTab_jni.h"
#include "chrome/browser/firefox_sync/android/jni_headers/SunupPush_jni.h"

namespace firefox_sync {

namespace {

FirefoxSyncService* GetService(Profile* profile) {
  return profile ? FirefoxSyncServiceFactory::GetForProfile(profile) : nullptr;
}

class AndroidPlatform : public Platform {
 public:
  explicit AndroidPlatform(Profile* profile)
      : profile_(profile),
        app_state_(base::android::ApplicationStatusListener::New(
            base::BindRepeating(&AndroidPlatform::OnApplicationState,
                                base::Unretained(this)))) {
    // Posted: the service creating this isn't its factory's yet. The
    // listener may have missed the browser's start, and a new version's first
    // start has no task scheduled.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&AndroidPlatform::Start,
                                  weak_factory_.GetWeakPtr()));
  }

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
  void OnApplicationState(base::android::ApplicationState state) {
    if (state != base::android::APPLICATION_STATE_HAS_RUNNING_ACTIVITIES) {
      return;
    }
    if (FirefoxSyncService* service = GetService(profile_)) {
      service->PollCommands();
    }
    ScheduleReceiveTabs();
  }

  void Start() {
    ScheduleReceiveTabs();
    std::string endpoint =
        Java_SunupPush_start(base::android::AttachCurrentThread());
    FirefoxSyncService* service = GetService(profile_);
    if (service && !endpoint.empty()) {
      service->SetPushEndpoint(endpoint);
    }
  }

  // The background task runs at the receive interval while signed in. The
  // settings screen replaces it when the interval changes or the account
  // signs out; this one only adds it, and cancels it once signed out.
  void ScheduleReceiveTabs() {
    FirefoxSyncService* service = GetService(profile_);
    if (!service) {
      return;
    }
    base::DictValue status = service->GetStatus();
    // The email, not signedIn: that waits for the saved account's
    // decryption.
    const std::string* email = status.FindString("email");
    Java_ReceiveTabsTask_schedule(
        base::android::AttachCurrentThread(),
        email && !email->empty() ? status.FindInt("receiveInterval").value_or(0)
                                 : 0,
        /*replace=*/false);
  }

  // The profile's tabs in a browser window, not incognito's or archived ones,
  // nor an installed web app's or a Custom Tab's: the tabs Firefox sees.
  bool IsMainTabModel(TabModel* model) const {
    return model->GetProfile() == profile_ && !model->IsOffTheRecord() &&
           model->GetTabModelType() == TabModel::TabModelType::kStandard &&
           model->activity_type() == chrome::android::ActivityType::kTabbed;
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
  std::unique_ptr<base::android::ApplicationStatusListener> app_state_;
  base::WeakPtrFactory<AndroidPlatform> weak_factory_{this};
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

static void JNI_SunupPush_SetPushEndpoint(JNIEnv* env,
                                          Profile* profile,
                                          const std::string& endpoint) {
  if (auto* service = GetService(profile)) {
    service->SetPushEndpoint(endpoint);
  }
}

static void JNI_SunupPush_OnPushReceived(JNIEnv* env, Profile* profile) {
  if (auto* service = GetService(profile)) {
    service->OnPushReceived();
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

DEFINE_JNI(ReceiveTabsTask)
DEFINE_JNI(SendTab)
DEFINE_JNI(SunupPush)
