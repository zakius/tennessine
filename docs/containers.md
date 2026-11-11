# Containers

A container keeps its own cookies and site storage within one profile, as Firefox's Multi-Account Containers do. A tab in the Work container is signed in to Work's accounts, and a tab outside containers doesn't see them. Incognito tabs have no containers.

The containers, their storage and Traffic Control are dubna's code, shared with oganesson (`dubna/src/chrome/browser/containers/`, `dubna/patches/containers.patch`; oganesson's [docs/containers.md](https://github.com/zakius/oganesson/blob/main/docs/containers.md) explains them). This page is about the Android side: `src/chrome/browser/containers/platform/` (dubna's `containers_platform.h`, and the JNI) and `src/chrome/browser/containers/android/` (Java).

## Using them

The first version is minimal, as goals.md planned:

- **Open in container**, in a link's long-press menu: a list of the containers, **New temporary container**, and **No container** when the page is in a container. The link opens in a new tab beside the page.
- **New tab in container**, after New tab in the tab switcher button's long-press menu, and a long press on the tab switcher's own New tab button: the containers, **New temporary container** and **No container**.
- **Reopen in container**, in a tab's menu in the tab switcher (long-press the tab): the containers but the tab's own, **New temporary container**, and **No container** when the tab is in one. The page opens in a new tab beside the tab, which closes.
- Each container shows with its icon, in its color, in these menus and in Settings: Firefox's icons (fingerprint, briefcase, dollar, cart, circle, gift, vacation, pet, chill, fence), drawn from the Chromium vector icons oganesson uses for them (`ContainerIcons.java`).
- In the tab switcher, a container tab's row has a line in the container's color along its bottom, as Firefox draws one under the tab.
- The tab switcher button has the current tab's container's icon, in its color, at the corner of its own icon, so you see where you are before you sign in somewhere. Screen readers say "In container Work".
- **Settings → Containers**, after Privacy and security: add a container, change its name, color or icon, or delete it; **Open new tabs in** (no container, a container, or a new temporary container each time); and Traffic Control's rules. A new profile has Firefox's four containers: Personal, Work, Banking and Shopping. Each rule change applies to the rules as they are then, so changes sync made while the screen was open stay; a rule whose container was deleted (by sync) shows Deleted container and keeps it until you pick another.

A link a container tab opens stays in the container: in the same tab, with Open in new tab, or with `target=_blank` and `window.open`, with or without an opener.

Sites in a container can show notifications, and a tap on one goes to the container's page or service worker. They can't subscribe to push messages: Chromium delivers those to the site outside containers, so the subscription fails with "push service not available" (dubna's `containers.patch`).

Deleting a container closes its tabs, without undo, and forgets its closed tabs. Its cookies and site data are deleted then. A temporary container's data is deleted once no tab or closed tab uses it, checked at each start and after a container is deleted.

Traffic Control: sites that match a rule open in its container, whichever tab they start in. A navigation that a rule sends elsewhere stops, and its page opens in a new tab beside its tab in the rule's container; the tab it started in closes when it was an empty new tab. In a custom tab or a web app, which have no tabs beside them, the page opens in the browser, where the rule moves it again.

## How it works

A tab stays in the storage partition its WebContents is created in (dubna's `containers_tabs.h`), so tennessine only has to create each tab's WebContents in the right container:

| Where a tab's WebContents is made | How it gets its container |
|---|---|
| Java, for a tab created without one: a link opened in a new tab, the New tab button | `TabImpl` asks `Containers.createWebContents()` in place of `WebContentsFactory`. A link's tab (`FROM_LINK`, `FROM_LONGPRESS_*`) takes its parent tab's container, a new tab from the browser's UI (`FROM_CHROME_UI`, `FROM_TAB_SWITCHER_UI`) the one Settings names. |
| A link opened with Open in new window or Open in other window | The new window's tab model doesn't have the link's tab, so `ChromeTabCreator` asks `Containers.createForOtherWindow()`, which finds the tab the intent names in any window and takes its container. |
| A window a page opens (`target=_blank`, `window.open`) | Content, as on the desktop: the opener's SiteInstance, or dubna's `web_contents_impl.cc` hook without an opener. A blocked popup allowed later is a tab Java creates, with its opener tab as parent. |
| A tab `Navigate()` opens from another tab (an extension's `tabs.create`) | `browser_navigator_android.cc` gives it the source tab's container (dubna's `InheritContainer()`). |
| Open in container, Traffic Control | `containers_android.cc` creates the WebContents with `SiteInstanceIn()` and hands it to the tab model (`TabModel::CreateTab()`). |
| A tab restored after a restart | `WebContentsState::RestoreContentsFromByteBuffer()` asks `SiteInstanceToRestore()`: the tab state saves each page's container as the page's extended info, as desktop sessions do. |
| A closed tab reopened (Recent tabs) | `AndroidLiveTabContext::AddRestoredTab()` and `SessionRestore::RestoreForeignSessionTab()` ask `SiteInstanceToRestore()`. |

dubna checks which containers are in use before it deletes a container's data. On Android, the saved session is the tabs themselves, restored by Java after the browser starts. So `GetContainersInUse()` waits until every tab is loaded: the open windows' first, then, after startup, the archived tabs and the tabs of windows that aren't open, which Chromium loads without a window (`TabWindowManager.isAllTabStateInitialized()`). It answers with the containers of all of them, the tabs waiting for Undo included. A tab without a WebContents (archived, or in a window that isn't open) tells its containers from its saved state (`WebContentsState`). With no window, as when a background task starts the browser, when a window closes before its tabs are restored, or when a tab without a WebContents has no saved state, it counts every container as in use, so nothing is deleted then.

The containers and the rules are dubna's prefs. Settings reads and writes them through `ContainersBridge` as JSON, and the native side checks each container and rule with dubna's own `FromDict()` and `IsValidUrlFilter()`.

## Chromium hooks

Lines added, includes and comments counted; TabImpl changes 8 more.

`patches/tennessine/containers.patch`:

| File | Lines | Why |
|---|---|---|
| `chrome/android/java/src/org/chromium/chrome/browser/tab/TabImpl.java` | 9 | Its four `WebContentsFactory.createWebContents()` calls go to `Containers.createWebContents()`: three for a new tab, and the empty page a tab gets when its saved state can't be restored. |
| `chrome/browser/ui/android/browser_navigator_android.cc` | 6 | A tab that `Navigate()` opens from a tab (extensions, the browser's own code) takes its container. |
| `chrome/android/java/src/org/chromium/chrome/browser/tabmodel/TabModelJniBridge.java` | 4 | A blocked popup the user allows later keeps its container tab as parent, also without an opener, so it opens in that container. |
| `chrome/android/java/src/org/chromium/chrome/browser/tabmodel/ChromeTabCreator.java` | 7 | A new tab that goes into a container doesn't take custom tabs' spare tab, which is outside containers. A link opened in another window gets its WebContents from `Containers.createForOtherWindow()`. |
| `chrome/browser/tab/web_contents_state.cc` | 5 | A tab restored after a restart gets its SiteInstance, asked for before its pages are built so they keep their container. |
| `chrome/browser/ui/android/tab_model/android_live_tab_context.cc` | 5 | A closed tab reopened in the background. |
| `chrome/browser/sessions/session_restore_android.cc` | 6 | A closed tab reopened in place of the current one. |
| `content/browser/browsing_instance.cc` | 5 | A SiteInstance in a container always gets a site, as a guest's does, so it keeps the container's partition. Android's native New tab page (`chrome-native:`) left one without a site, which reported the default partition, and the tab's next navigation crashed the browser on a CHECK. |
| `chrome/android/BUILD.gn` | 2 | `ContainersBridge.java`'s JNI. |
| `tennessine/chromium_src/chrome/android/chrome_java_ext_sources.gni` | 4 | Our Java in `chrome_java` (Vanadium's list). |
| `chrome/app/generated_resources.grd`, `.gritdeps` | 2 | dubna's strings (the default containers' names, "Temporary 2"). |
| `android_chrome_ext_strings.grdp`, `android_chrome_strings_pl.xtb` | 16 | The menu item, the Settings title and the chooser's two choices, in English and Polish. Settings' other text is English only, as in Web apps. |

`patches/tennessine/containers-ui.patch`:

| File | Lines | Why |
|---|---|---|
| `chrome/android/java/src/org/chromium/chrome/browser/contextmenu/ChromeContextMenuPopulator.java` | 16 | The link menu's item and its handler. The item has its own id (`Containers.MENU_ID`), so no upstream list of items changes. No Preview page in a container tab: the preview would load outside the container. |
| `chrome/android/features/tab_ui/java/src/org/chromium/chrome/browser/tasks/tab_management/TabGridViewBinder.java` | 3 | The tab switcher row's color line. |
| `chrome/android/java/res/xml/main_preferences.xml` | 5 | The Settings entry. |
| `chrome/browser/ui/android/toolbar/java/src/org/chromium/chrome/browser/toolbar/top/TabSwitcherActionMenuCoordinator.java` | 6 | A slot after New tab in the tab switcher button's menu, which ToolbarManager fills with New tab in container (the toolbar module can't see our Java). |
| `chrome/android/java/src/org/chromium/chrome/browser/toolbar/ToolbarManager.java` | 16 | Fills that slot and handles the item: the chooser, then the menu's own New tab, whose tab takes the chosen container. Marks the tab switcher button when the current tab or its page changes. |
| `chrome/android/features/tab_ui/java/src/org/chromium/chrome/browser/tasks/tab_management/TabGridContextMenuCoordinator.java` | 7 | Reopen in container in a tab's menu, and its handler. |
| `chrome/browser/hub/internal/android/java/src/org/chromium/chrome/browser/hub/HubToolbarCoordinator.java` | 7 | A long press on the tab switcher's New tab button goes to the focused pane, when it takes one. |
| `chrome/android/features/tab_ui/java/src/org/chromium/chrome/browser/tasks/tab_management/TabSwitcherPane.java` | 13 | The regular tab switcher's pane takes it: the chooser, then its own New tab. Incognito's doesn't, so its button keeps the tooltip. |

## Not done

- The tab strip on tablets shows no color: it is drawn by the compositor (`StripLayoutTab`), which needs a new property and its drawing code.
- New tab in container on the tablet tab strip's New tab button.
