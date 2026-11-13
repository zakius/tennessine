// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// dubna's containers_platform.h on Android, and the JNI of
// ContainersBridge.java: the tabs Java creates in a container, the tab
// switcher's color line, the link menu's chooser and Settings → Containers.

#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/android/jni_android.h"
#include "base/android/jni_bytebuffer.h"
#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/raw_ptr.h"
#include "base/values.h"
#include "chrome/browser/android/tab_android.h"
#include "chrome/browser/containers/container.h"
#include "chrome/browser/containers/containers_platform.h"
#include "chrome/browser/containers/containers_service.h"
#include "chrome/browser/containers/containers_service_factory.h"
#include "chrome/browser/containers/containers_tabs.h"
#include "chrome/browser/containers/traffic_control.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/tab/web_contents_state.h"
#include "chrome/browser/ui/android/tab_model/tab_model.h"
#include "chrome/browser/ui/android/tab_model/tab_model_list.h"
#include "chrome/common/webui_url_constants.h"
#include "components/prefs/pref_service.h"
#include "components/sessions/core/serialized_navigation_entry.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

// Must come after all headers that specialize FromJniType() / ToJniType().
#include "chrome/android/chrome_jni_headers/ContainersBridge_jni.h"

using base::android::AttachCurrentThread;
using base::android::ScopedJavaLocalRef;

namespace containers {

namespace {

using InUseCallback = base::OnceCallback<void(std::set<std::string>)>;

// A GetContainersInUse() call that waits for the tabs to be restored.
struct InUseRequest {
  raw_ptr<Profile> profile;
  InUseCallback callback;
};

// The tab models with `profile`'s tabs: its windows' and the archived tabs'.
std::vector<TabModel*> ModelsOf(Profile* profile) {
  std::vector<TabModel*> models;
  for (TabModel* model : TabModelList::models()) {
    if (model->GetProfile() == profile) {
      models.push_back(model);
    }
  }
  TabModel* archived = TabModelList::GetArchivedTabModel();
  if (archived && archived->GetProfile() == profile &&
      !std::ranges::contains(models, archived)) {
    models.push_back(archived);
  }
  return models;
}

// A tab with nothing to keep: no history, and a blank or new tab page.
bool IsEmptyTab(content::WebContents* contents) {
  content::NavigationController& controller = contents->GetController();
  if (controller.GetEntryCount() > 1) {
    return false;
  }
  const GURL& url = contents->GetLastCommittedURL();
  return controller.IsInitialBlankNavigation() || url.IsAboutBlank() ||
         url == GURL(chrome::kChromeUINativeNewTabURL);
}

// Opens `url` in a new tab beside `source`'s, in container `container_id`
// (outside containers when empty). False when `source` isn't in a tab of the
// browser's windows, as in a custom tab or a web app.
bool OpenInNewTab(content::WebContents* source,
                  const GURL& url,
                  const std::string& container_id,
                  ui::PageTransition transition,
                  const std::optional<url::Origin>& initiator,
                  bool foreground) {
  TabAndroid* parent = TabAndroid::FromWebContents(source);
  TabModel* model = TabModelList::GetTabModelForWebContents(source);
  if (!parent || !model ||
      model->GetTabModelType() != TabModel::TabModelType::kStandard) {
    return false;
  }
  content::BrowserContext* context = source->GetBrowserContext();
  content::WebContents::CreateParams params(
      context, container_id.empty()
                   ? nullptr
                   : SiteInstanceIn(context, container_id, url));
  params.initially_hidden = !foreground;
  tabs::TabInterface* tab = model->CreateTab(
      parent, content::WebContents::Create(params), TabModel::kInvalidIndex,
      foreground ? TabModel::TabLaunchType::FROM_LONGPRESS_FOREGROUND
                 : TabModel::TabLaunchType::FROM_LONGPRESS_BACKGROUND,
      /*should_pin=*/false);
  if (!tab || !tab->GetContents()) {
    return true;
  }
  content::NavigationController::LoadURLParams load(url);
  load.transition_type = transition;
  load.initiator_origin = initiator;
  tab->GetContents()->GetController().LoadURLWithParams(load);
  return true;
}

ContainersService* ServiceOf(Profile* profile) {
  return ContainersServiceFactory::GetForProfile(profile);
}

std::string ToJson(const base::ListValue& list) {
  return base::WriteJson(list).value_or("[]");
}

// The list in `json`, or nullopt when it isn't one.
std::optional<base::ListValue> ListFromJson(const std::string& json) {
  std::optional<base::Value> value = base::JSONReader::Read(
      json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!value || !value->is_list()) {
    return std::nullopt;
  }
  return std::move(*value).TakeList();
}

}  // namespace

void ReopenNavigation(content::WebContents* source,
                      const GURL& url,
                      const std::string& container_id,
                      ui::PageTransition transition,
                      const std::optional<url::Origin>& initiator) {
  TabModel* model = TabModelList::GetTabModelForWebContents(source);
  const bool empty = IsEmptyTab(source);
  const bool active = model && model->GetActiveWebContents() == source;
  if (!OpenInNewTab(source, url, container_id, transition, initiator,
                    active)) {
    // A custom tab or a web app has no tabs to open beside: the browser
    // opens the page in a new tab, where Traffic Control moves it again.
    Java_ContainersBridge_openInBrowser(AttachCurrentThread(), url.spec());
    return;
  }
  TabAndroid* tab = TabAndroid::FromWebContents(source);
  if (empty && tab) {
    model->CloseTab(tab->GetHandle());
  }
}

// Java answers once every tab is loaded (OnTabsRestored()). With no window,
// as when a background task started the browser, the tabs are unknown and
// every container counts as in use.
void GetContainersInUse(Profile* profile, InUseCallback callback) {
  auto* request = new InUseRequest{profile, std::move(callback)};
  Java_ContainersBridge_whenTabsRestored(AttachCurrentThread(),
                                         reinterpret_cast<jlong>(request));
}

void CloseTabsInContainer(Profile* profile, const std::string& id) {
  for (TabModel* model : ModelsOf(profile)) {
    // Backwards: closing a tab moves the ones after it.
    for (int i = model->GetTabCount() - 1; i >= 0; --i) {
      content::WebContents* contents = model->GetWebContentsAt(i);
      if (contents && ContainerIdOf(contents) == id) {
        model->CloseTabAt(i);
      }
    }
  }
}

}  // namespace containers

using containers::Container;
using containers::ContainersService;

// Java gives the containers of every tab once all are loaded: those of the
// open windows, of the windows that aren't open, the archived ones and the
// ones waiting for Undo. When it can't, every container counts as in use.
static void JNI_ContainersBridge_OnTabsRestored(
    JNIEnv* env,
    jlong request_ptr,
    jboolean restored,
    const std::vector<std::string>& tab_containers) {
  std::unique_ptr<containers::InUseRequest> request(
      reinterpret_cast<containers::InUseRequest*>(request_ptr));
  std::set<std::string> ids;
  if (restored) {
    for (const std::string& id : tab_containers) {
      if (!id.empty()) {
        ids.insert(id);
      }
    }
  } else {
    for (auto [id, value] :
         request->profile->GetPrefs()->GetDict(containers::kContainersUsed)) {
      ids.insert(id);
    }
  }
  std::move(request->callback).Run(std::move(ids));
}

// The containers of the pages in a saved tab state, as a tab that isn't
// loaded (archived, or of a window that isn't open) keeps it.
static std::vector<std::string> JNI_ContainersBridge_ContainersOfState(
    JNIEnv* env,
    const base::android::JavaRef<jobject>& state,
    int32_t version) {
  bool is_off_the_record = false;
  int current_entry_index = 0;
  std::vector<sessions::SerializedNavigationEntry> navigations;
  std::set<std::string> ids;
  if (WebContentsState::ExtractNavigationEntries(
          base::android::JavaByteBufferToSpan(env, state), version,
          &is_off_the_record, &current_entry_index, &navigations)) {
    containers::AddContainerIds(navigations, ids);
  }
  return std::vector<std::string>(ids.begin(), ids.end());
}

// The container a tab that isn't loaded opens in, from its saved state: its
// shown page's, else another page's, as SiteInstanceToRestore() picks it.
static std::string JNI_ContainersBridge_ContainerOfState(
    JNIEnv* env,
    const base::android::JavaRef<jobject>& state,
    int32_t version) {
  bool is_off_the_record = false;
  int current_entry_index = 0;
  std::vector<sessions::SerializedNavigationEntry> navigations;
  if (!WebContentsState::ExtractNavigationEntries(
          base::android::JavaByteBufferToSpan(env, state), version,
          &is_off_the_record, &current_entry_index, &navigations)) {
    return std::string();
  }
  std::set<std::string> ids;
  if (current_entry_index >= 0 &&
      static_cast<size_t>(current_entry_index) < navigations.size()) {
    containers::AddContainerIds(
        base::span_from_ref(navigations[current_entry_index]), ids);
  }
  if (ids.empty()) {
    containers::AddContainerIds(navigations, ids);
  }
  return ids.empty() ? std::string() : *ids.begin();
}

// A new tab's WebContents, in container `id`, or outside containers when
// empty. As WebContentsFactory.createWebContents() makes it.
static ScopedJavaLocalRef<jobject> JNI_ContainersBridge_CreateWebContents(
    JNIEnv* env,
    Profile* profile,
    const std::string& id,
    jboolean initially_hidden,
    jboolean initialize_renderer) {
  content::WebContents::CreateParams params(
      profile, containers::SiteInstanceIn(profile, id, GURL()));
  params.initially_hidden = initially_hidden;
  params.desired_renderer_state =
      initialize_renderer
          ? content::WebContents::CreateParams::
                kInitializeAndWarmupRendererProcess
          : content::WebContents::CreateParams::kOkayToHaveRendererProcess;
  // Ownership is passed into Java, and then to TabAndroid::InitWebContents.
  return content::WebContents::Create(params).release()->GetJavaWebContents();
}

static std::string JNI_ContainersBridge_ContainerOf(
    JNIEnv* env,
    content::WebContents* contents) {
  return contents ? containers::ContainerIdOf(contents) : std::string();
}

// The color of container `id` as ARGB, or 0 when it isn't one of the user's.
static jint JNI_ContainersBridge_ColorOfContainer(JNIEnv* env,
                                                  Profile* profile,
                                                  const std::string& id) {
  ContainersService* service =
      id.empty() ? nullptr : containers::ServiceOf(profile);
  std::optional<Container> container =
      service ? service->Get(id) : std::nullopt;
  return container ? static_cast<jint>(container->color_value()) : 0;
}

// The color of `contents`' container as ARGB, or 0 outside containers.
static jint JNI_ContainersBridge_ColorOf(JNIEnv* env,
                                         content::WebContents* contents) {
  if (!contents) {
    return 0;
  }
  return JNI_ContainersBridge_ColorOfContainer(
      env, Profile::FromBrowserContext(contents->GetBrowserContext()),
      containers::ContainerIdOf(contents));
}

static jint JNI_ContainersBridge_ColorValue(JNIEnv* env,
                                            const std::string& color) {
  return static_cast<jint>(Container{.color = color}.color_value());
}

static void JNI_ContainersBridge_OpenInNewTab(JNIEnv* env,
                                              content::WebContents* source,
                                              const std::string& url,
                                              const std::string& id,
                                              jboolean foreground) {
  if (!source) {
    return;
  }
  containers::OpenInNewTab(source, GURL(url), id, ui::PAGE_TRANSITION_LINK,
                           source->GetPrimaryMainFrame()->GetLastCommittedOrigin(),
                           foreground);
}

// The user's containers as JSON, with each one's color as ARGB ("argb").
static std::string JNI_ContainersBridge_GetContainers(JNIEnv* env,
                                                      Profile* profile) {
  base::ListValue list;
  if (ContainersService* service = containers::ServiceOf(profile)) {
    for (const Container& container : service->GetContainers()) {
      list.Append(container.ToDict().Set(
          "argb", static_cast<int>(container.color_value())));
    }
  }
  return containers::ToJson(list);
}

// Sets the user's containers from JSON; false, keeping them, when one isn't
// valid. A container left out is deleted.
static jboolean JNI_ContainersBridge_SetContainers(JNIEnv* env,
                                                   Profile* profile,
                                                   const std::string& json) {
  std::optional<base::ListValue> list = containers::ListFromJson(json);
  if (!list) {
    return false;
  }
  base::ListValue valid;
  for (const base::Value& value : *list) {
    std::optional<Container> container =
        value.is_dict() ? Container::FromDict(value.GetDict()) : std::nullopt;
    if (!container) {
      return false;
    }
    valid.Append(container->ToDict());
  }
  profile->GetPrefs()->SetList(containers::kContainersList, std::move(valid));
  return true;
}

static std::string JNI_ContainersBridge_GetRules(JNIEnv* env,
                                                 Profile* profile) {
  return containers::ToJson(
      profile->GetPrefs()->GetList(containers::kTrafficControlRules));
}

// Sets Traffic Control's rules from JSON; returns why one isn't valid,
// keeping the old ones, or "".
static std::string JNI_ContainersBridge_SetRules(JNIEnv* env,
                                                 Profile* profile,
                                                 const std::string& json) {
  std::optional<base::ListValue> list = containers::ListFromJson(json);
  if (!list) {
    return "Not a list of rules";
  }
  base::ListValue valid;
  for (const base::Value& value : *list) {
    std::optional<containers::TrafficRule> rule =
        value.is_dict() ? containers::TrafficRule::FromDict(value.GetDict())
                        : std::nullopt;
    if (!rule) {
      return "Not a rule";
    }
    if (!containers::IsValidUrlFilter(rule->url_filter)) {
      return "Each line must be a site or URL pattern, such as example.com or "
             "https://*.example.org/path";
    }
    valid.Append(rule->ToDict());
  }
  profile->GetPrefs()->SetList(containers::kTrafficControlRules,
                               std::move(valid));
  return std::string();
}

// The New tabs setting: a container's id, "temporary" or "".
static std::string JNI_ContainersBridge_GetNewTabSetting(JNIEnv* env,
                                                         Profile* profile) {
  return profile->GetPrefs()->GetString(containers::kNewTabContainer);
}

static void JNI_ContainersBridge_SetNewTabSetting(JNIEnv* env,
                                                  Profile* profile,
                                                  const std::string& value) {
  profile->GetPrefs()->SetString(containers::kNewTabContainer, value);
}

// The container a new tab opens in now: an id, a new temporary container's,
// or "".
static std::string JNI_ContainersBridge_NewTabContainer(JNIEnv* env,
                                                        Profile* profile) {
  ContainersService* service = containers::ServiceOf(profile);
  return service ? service->NewTabContainer() : std::string();
}

static std::string JNI_ContainersBridge_CreateTemporary(JNIEnv* env,
                                                        Profile* profile) {
  ContainersService* service = containers::ServiceOf(profile);
  return service ? service->CreateTemporary().id : std::string();
}

DEFINE_JNI(ContainersBridge)
