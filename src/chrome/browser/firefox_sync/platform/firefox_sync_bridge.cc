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
#include "chrome/browser/firefox_sync/firefox_sync_platform.h"
#include "chrome/browser/firefox_sync/firefox_sync_service.h"
#include "chrome/browser/firefox_sync/firefox_sync_service_factory.h"
#include "chrome/browser/profiles/profile.h"

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

std::string DeviceName() {
  return "tennessine on " + Java_FirefoxSyncBridge_getDeviceName(
                                base::android::AttachCurrentThread());
}

}  // namespace firefox_sync

DEFINE_JNI(FirefoxSyncBridge)
