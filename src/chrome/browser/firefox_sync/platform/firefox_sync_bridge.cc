// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// JNI for FirefoxSyncBridge.java, the Android settings screen's access to
// FirefoxSyncService, and DeviceName() (firefox_sync_platform.h), which needs
// the same JNI header.

#include <string>

#include "base/android/jni_android.h"
#include "base/android/jni_string.h"
#include "base/android/scoped_java_ref.h"
#include "base/callback_list.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "chrome/browser/firefox_sync/firefox_sync_platform.h"
#include "chrome/browser/firefox_sync/firefox_sync_service.h"
#include "chrome/browser/firefox_sync/firefox_sync_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/grit/oganesson_strings.h"
#include "ui/base/l10n/l10n_util.h"

// Must come after all headers that specialize FromJniType() / ToJniType().
#include "chrome/browser/firefox_sync/android/jni_headers/FirefoxSyncBridge_jni.h"

namespace firefox_sync {

namespace {

FirefoxSyncService* GetService(Profile* profile) {
  return profile ? FirefoxSyncServiceFactory::GetForProfile(profile) : nullptr;
}

}  // namespace

// Forwards status changes to the Java object, which owns this through
// mNativeFirefoxSyncBridge.
class FirefoxSyncBridge {
 public:
  FirefoxSyncBridge(JNIEnv* env,
                    const base::android::JavaRef<jobject>& obj,
                    FirefoxSyncService* service)
      : java_obj_(env, obj) {
    if (service) {
      subscription_ = service->AddStatusObserver(base::BindRepeating(
          &FirefoxSyncBridge::OnStatusChanged, base::Unretained(this)));
    }
  }

  void Destroy(JNIEnv* env) { delete this; }

 private:
  void OnStatusChanged() {
    Java_FirefoxSyncBridge_onStatusChanged(base::android::AttachCurrentThread(),
                                           java_obj_);
  }

  base::android::ScopedJavaGlobalRef<jobject> java_obj_;
  base::CallbackListSubscription subscription_;
};

static jlong JNI_FirefoxSyncBridge_Init(
    JNIEnv* env,
    const base::android::JavaRef<jobject>& caller,
    Profile* profile) {
  return reinterpret_cast<intptr_t>(
      new FirefoxSyncBridge(env, caller, GetService(profile)));
}

static std::string JNI_FirefoxSyncBridge_GetStatus(JNIEnv* env,
                                                   Profile* profile) {
  FirefoxSyncService* service = GetService(profile);
  return service ? base::WriteJson(service->GetStatus()).value_or("{}") : "{}";
}

static void JNI_FirefoxSyncBridge_SignIn(JNIEnv* env, Profile* profile) {
  if (auto* service = GetService(profile)) {
    service->SignIn();
  }
}

static void JNI_FirefoxSyncBridge_SignOut(JNIEnv* env, Profile* profile) {
  if (auto* service = GetService(profile)) {
    service->SignOut();
  }
}

static void JNI_FirefoxSyncBridge_SyncNow(JNIEnv* env, Profile* profile) {
  if (auto* service = GetService(profile)) {
    service->SyncNow();
  }
}

static void JNI_FirefoxSyncBridge_SetServer(JNIEnv* env,
                                            Profile* profile,
                                            const std::string& server,
                                            jboolean sync_storage) {
  if (auto* service = GetService(profile)) {
    service->SetServer(server, sync_storage);
  }
}

static void JNI_FirefoxSyncBridge_SetTypeEnabled(JNIEnv* env,
                                                 Profile* profile,
                                                 const std::string& name,
                                                 jboolean enabled) {
  if (auto* service = GetService(profile)) {
    service->SetTypeEnabled(name, enabled);
  }
}

static void JNI_FirefoxSyncBridge_PollCommands(JNIEnv* env, Profile* profile) {
  if (auto* service = GetService(profile)) {
    service->PollCommands();
  }
}

static void JNI_FirefoxSyncBridge_SetReceiveInterval(JNIEnv* env,
                                                     Profile* profile,
                                                     jint minutes) {
  if (auto* service = GetService(profile)) {
    service->SetReceiveInterval(minutes);
  }
}

// dubna's strings, which oganesson's settings use for the same dropdown.
static std::string JNI_FirefoxSyncBridge_GetReceiveIntervals(JNIEnv* env) {
  base::ListValue options;
  for (int minutes : {15, 30, 60, 0}) {
    std::string label;
    if (minutes == 0) {
      label = l10n_util::GetStringUTF8(IDS_FIREFOX_SYNC_RECEIVE_TABS_SYNC_ONLY);
    } else if (minutes == 60) {
      label = l10n_util::GetStringUTF8(IDS_FIREFOX_SYNC_RECEIVE_TABS_HOUR);
    } else {
      label = l10n_util::GetStringFUTF8(IDS_FIREFOX_SYNC_RECEIVE_TABS_MINUTES,
                                        base::NumberToString16(minutes));
    }
    options.Append(
        base::DictValue().Set("minutes", minutes).Set("label", label));
  }
  return base::WriteJson(base::DictValue()
                             .Set("title", l10n_util::GetStringUTF8(
                                               IDS_FIREFOX_SYNC_RECEIVE_TABS))
                             .Set("options", std::move(options)))
      .value_or("{}");
}

std::string DeviceName() {
  return "tennessine on " + Java_FirefoxSyncBridge_getDeviceName(
                                base::android::AttachCurrentThread());
}

}  // namespace firefox_sync

DEFINE_JNI(FirefoxSyncBridge)
