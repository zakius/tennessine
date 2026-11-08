# Containers

A container keeps its own cookies and site storage within one profile, as Firefox's Multi-Account Containers do. A tab in the Work container is signed in to Work's accounts, and a tab outside containers doesn't see them. Incognito tabs have no containers.

The containers, their storage and Traffic Control are dubna's code, shared with oganesson (`dubna/src/chrome/browser/containers/`, `dubna/patches/containers.patch`; oganesson's [docs/containers.md](https://github.com/zakius/oganesson/blob/main/docs/containers.md) explains them). This page is about the Android side: `src/chrome/browser/containers/platform/` (dubna's `containers_platform.h`, and the JNI) and `src/chrome/browser/containers/android/` (Java).

## Using them

The first version is minimal, as goals.md planned:

- **Open in container**, in a link's long-press menu: a list of the containers, **New temporary container**, and **No container** when the page is in a container. The link opens in a new tab beside the page.
- In the tab switcher, a container tab's row has a line in the container's color along its bottom, as Firefox draws one under the tab.
- **Settings → Containers**, after Privacy and security: add a container, change its name or color, or delete it; **Open new tabs in** (no container, a container, or a new temporary container each time); and Traffic Control's rules. A new profile has Firefox's four containers: Personal, Work, Banking and Shopping.

A link a container tab opens stays in the container: in the same tab, with Open in new tab, or with `target=_blank` and `window.open`, with or without an opener.

Deleting a container closes its tabs, without undo, and forgets its closed tabs. Its cookies and site data are deleted then. A temporary container's data is deleted once no tab or closed tab uses it, checked at each start and after a container is deleted.

Traffic Control: sites that match a rule open in its container, whichever tab they start in. A navigation that a rule sends elsewhere stops, and its page opens in a new tab beside its tab in the rule's container; the tab it started in closes when it was an empty new tab. In a custom tab or a web app, which have no tabs beside them, the page opens in the browser, where the rule moves it again.

## How it works

A tab stays in the storage partition its WebContents is created in (dubna's `containers_tabs.h`), so tennessine only has to create each tab's WebContents in the right container:

| Where a tab's WebContents is made | How it gets its container |
|---|---|
| Java, for a tab created without one: a link opened in a new tab, the New tab button | `TabImpl` asks `Containers.createWebContents()` in place of `WebContentsFactory`. A link's tab (`FROM_LINK`, `FROM_LONGPRESS_*`) takes its parent tab's container, a new tab from the browser's UI (`FROM_CHROME_UI`, `FROM_TAB_SWITCHER_UI`) the one Settings names. |
| A window a page opens (`target=_blank`, `window.open`) | Content, as on the desktop: the opener's SiteInstance, or dubna's `web_contents_impl.cc` hook without an opener. |
| Open in container, Traffic Control | `containers_android.cc` creates the WebContents with `SiteInstanceIn()` and hands it to the tab model (`TabModel::CreateTab()`). |
| A tab restored after a restart | `WebContentsState::RestoreContentsFromByteBuffer()` asks `SiteInstanceToRestore()`: the tab state saves each page's container as the page's extended info, as desktop sessions do. |
| A closed tab reopened (Recent tabs) | `AndroidLiveTabContext::AddRestoredTab()` and `SessionRestore::RestoreForeignSessionTab()` ask `SiteInstanceToRestore()`. |

dubna checks which containers are in use before it deletes a container's data. On Android, the saved session is the open tabs themselves, restored by Java after the browser starts. So `GetContainersInUse()` waits until the windows' tabs are restored (`TabModelUtils.runOnTabStateInitialized()`) and answers with their containers. With no window, as when a background task starts the browser, it counts every container as in use, so nothing is deleted then.

The containers and the rules are dubna's prefs. Settings reads and writes them through `ContainersBridge` as JSON, and the native side checks each container and rule with dubna's own `FromDict()` and `IsValidUrlFilter()`.

## Chromium hooks

Lines added, includes and comments counted; TabImpl changes 5 more.

`patches/tennessine/containers.patch`:

| File | Lines | Why |
|---|---|---|
| `chrome/android/java/src/org/chromium/chrome/browser/tab/TabImpl.java` | 7 | Its three `WebContentsFactory.createWebContents()` calls for a new tab go to `Containers.createWebContents()`. |
| `chrome/android/java/src/org/chromium/chrome/browser/tabmodel/ChromeTabCreator.java` | 3 | A new tab that goes into a container doesn't take custom tabs' spare tab, which is outside containers. |
| `chrome/browser/tab/web_contents_state.cc` | 5 | A tab restored after a restart gets its SiteInstance, asked for before its pages are built so they keep their container. |
| `chrome/browser/ui/android/tab_model/android_live_tab_context.cc` | 5 | A closed tab reopened in the background. |
| `chrome/browser/sessions/session_restore_android.cc` | 6 | A closed tab reopened in place of the current one. |
| `chrome/android/BUILD.gn` | 2 | `ContainersBridge.java`'s JNI. |
| `tennessine/chromium_src/chrome/android/chrome_java_ext_sources.gni` | 3 | Our Java in `chrome_java` (Vanadium's list). |
| `chrome/app/generated_resources.grd`, `.gritdeps` | 2 | dubna's strings (the default containers' names, "Temporary 2"). |
| `android_chrome_ext_strings.grdp`, `android_chrome_strings_pl.xtb` | 16 | The menu item, the Settings title and the chooser's two choices, in English and Polish. Settings' other text is English only, as in Web apps. |

`patches/tennessine/containers-ui.patch`:

| File | Lines | Why |
|---|---|---|
| `chrome/android/java/src/org/chromium/chrome/browser/contextmenu/ChromeContextMenuPopulator.java` | 16 | The link menu's item and its handler. The item has its own id (`Containers.MENU_ID`), so no upstream list of items changes. No Preview page in a container tab: the preview would load outside the container. |
| `chrome/android/features/tab_ui/java/src/org/chromium/chrome/browser/tasks/tab_management/TabGridViewBinder.java` | 3 | The tab switcher row's color line. |
| `chrome/android/java/res/xml/main_preferences.xml` | 5 | The Settings entry. |

## Not done

- The tab strip on tablets shows no color: it is drawn by the compositor (`StripLayoutTab`), which needs a new property and its drawing code.
- A tab's container doesn't show in the toolbar.
- New tab in container and Reopen in container: the menus of the New tab button and of a tab, as oganesson has.
- Firefox's container icons: a container has a color only. The icon stays in the prefs, so oganesson still shows it.
- Archived tabs (Chromium's inactive tabs) have no WebContents until they are restored, so they don't count as using their container. A temporary container whose only tab is archived loses its data.
- Tabs of a window that isn't open (Android's multi-instance windows) don't count either, the same way.
