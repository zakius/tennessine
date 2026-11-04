// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// dubna's app_links_platform.h on Android, through AppLinksBridge.java, and
// the JNI of the settings screens and the out-of-app bar.

#include <optional>
#include <string>

#include "base/android/jni_android.h"
#include "base/android/jni_string.h"
#include "chrome/browser/app_links/app_links_platform.h"
#include "chrome/browser/app_links/app_scope_rules.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

// Must come after all headers that specialize FromJniType() / ToJniType().
#include "chrome/android/chrome_jni_headers/AppLinksBridge_jni.h"

namespace app_links {

std::optional<std::string> PlatformAppId(content::WebContents* contents) {
  return Java_AppLinksBridge_getAppId(base::android::AttachCurrentThread(),
                                      contents->GetJavaWebContents());
}

bool PlatformIsUrlInScope(content::WebContents* contents, const GURL& url) {
  return Java_AppLinksBridge_isUrlInScope(base::android::AttachCurrentThread(),
                                          contents->GetJavaWebContents(),
                                          url.spec());
}

void OpenInDefaultBrowser(content::WebContents* contents,
                          const GURL& url,
                          bool background) {
  Java_AppLinksBridge_openInDefaultBrowser(
      base::android::AttachCurrentThread(), contents->GetJavaWebContents(),
      url.spec(), background);
}

// The out-of-app bar asks WebappVerifier, which applies the rules itself on
// the next navigation.
void OnScopeRulesChanged(content::WebContents* contents) {}

}  // namespace app_links

static std::string JNI_AppLinksBridge_GetRules(JNIEnv* env,
                                               Profile* profile,
                                               const std::string& app_id) {
  return app_links::GetUserAppScopeRules(*profile->GetPrefs(), app_id);
}

static std::string JNI_AppLinksBridge_SetRules(JNIEnv* env,
                                               Profile* profile,
                                               const std::string& app_id,
                                               const std::string& rules) {
  return app_links::SetUserAppScopeRules(*profile->GetPrefs(), app_id, rules);
}

static jboolean JNI_AppLinksBridge_IsStrict(JNIEnv* env,
                                            Profile* profile,
                                            const std::string& app_id) {
  return app_links::IsUserAppScopeStrict(*profile->GetPrefs(), app_id);
}

static void JNI_AppLinksBridge_SetStrict(JNIEnv* env,
                                         Profile* profile,
                                         const std::string& app_id,
                                         jboolean strict) {
  app_links::SetUserAppScopeStrict(*profile->GetPrefs(), app_id, strict);
}

static jint JNI_AppLinksBridge_ScopeOverride(JNIEnv* env,
                                             Profile* profile,
                                             const std::string& app_id,
                                             const std::string& url) {
  std::optional<bool> in_scope =
      app_links::UserAppScopeOverride(*profile->GetPrefs(), app_id, GURL(url));
  return in_scope ? *in_scope : -1;
}
DEFINE_JNI(AppLinksBridge)
