# Patch decisions

tennessine is Vanadium plus one patch series, `patches/series`. What matters most: the configurable toolbar, uBlock Origin (Helium's build, installed on first run), other Chrome extensions, page scaling, the list-mode tab switcher, JIT on, the full URL in the address bar, not saving history, and Firefox Sync, and links out of installed web apps. Other features stay only while they cost nothing on updates (narrowed with the maintainer on 2026-10-09; the wider set of 2026-10-04 was in the old tennessine repo, since deleted).

## Sources

| Source | How it's taken |
|---|---|
| [Vanadium](https://github.com/GrapheneOS/Vanadium) | The `vanadium` submodule; `tennessine.py apply` applies its patches renamed to tennessine, minus `patches/vanadium-drops.txt` (with reasons). tennessine follows Vanadium's releases, which follow Chromium stable. |
| [Titanium](https://github.com/jqssun/android-titanium-browser) | The `titanium` submodule, at the version `patches/titanium/*.patch` match: its `patch.sh` edits, one patch per group. The two that edit gclient dependencies are in `post-sync.sh`. Titanium's app icon is not used: tennessine draws its own (`tools/app_icon.py`). |
| [Helium](https://github.com/imputnet/helium) | Its uBlock Origin build ([imputnet/uBlock](https://github.com/imputnet/uBlock)), a separate program the browser downloads and installs on first run (`src/chrome/browser/ublock_installer`), with the ID Helium's built-in copy has, so its settings carry over. None of Helium's browser patches: they are GPL-3.0, which Vanadium's GPL-2.0-only license excludes (README, License). uBlock Origin's CNAME uncloaking uses our own DNS API (`patches/tennessine/dns-api.patch`), in place of Helium's. |
| [dubna](https://github.com/zakius/dubna) | The `dubna` submodule, shared with [desktop oganesson](https://github.com/zakius/oganesson): tab containers (`containers.patch`; our Android side is `src/chrome/browser/containers/`), no saved history (its core patch; the switches in Privacy, next to Close tabs on exit, are ours, `tennessine/history-saving-settings`), Firefox Sync's C++, strings, Polish translations and Rust library. Ours is Sync's Android side, `src/chrome/browser/firefox_sync/platform/` and `android/`. |
| [Cromite](https://github.com/uazo/cromite) | The ideas for the list-mode tab switcher (always on, without tab thumbnails) and the full URL in the address bar; the code is ours (`patches/tennessine/`), since Cromite's is GPL-3.0. |

## Vanadium

Kept: its hardening (compiler flags, memory tagging, W^X, strict site and origin isolation), partitioning, attack-surface cuts (sensors, WebGPU, payments), DuckDuckGo as default search with seven engines, the local password store (Firefox Sync passwords build on it), bookmark import and export, the barcode detector without Play services, the WebRTC settings screen, and the off-by-default toggles for opening external links and custom tabs in incognito.

Changed:

- JavaScript JIT runs by default (`tennessine/jit-allowed-by-default`). Vanadium blocks it; the per-site setting stays.
- HTTPS-Only is off (0119 dropped). It shows an error page when a site redirects `domain.tld` to `www.domain.tld` before HTTPS. Chromium's default upgrades to HTTPS silently and falls back to HTTP.
- Autofill uses the built-in password manager by default (0164 dropped). Vanadium defaults to Android's autofill service and skips the check that one is set (0219), so with none set nothing fills, and Firefox Sync passwords live in the built-in store. Settings → Autofill services still switches to another service; its built-in option reads "Autofill with tennessine", not Google (`tennessine/autofill-built-in-labels`).

Dropped:

- The always-on subresource filter (0201-0203): uBlock Origin does the blocking.
- GrapheneOS connectivity checks (0207-0211): outside GrapheneOS they send DNS and DoH probes to GrapheneOS's servers. Chromium's own Secure DNS setting stays.
- Titanium's drops: Trichrome targets, GrapheneOS language and PDF integration, the config app's component server, and a few UI patches.

Kept for now:

- 0154, a toggle to close tabs on exit. Later privacy-settings patches build on it.
- The GrapheneOS config-app framework and the WebView patches. They do nothing in this build (no config app, no WebView target), and later patches hook into them.

## Titanium

Kept: the extension support for phones (toolbar button, popups, pinning, MV2, Opera and Edge add-on installs, incognito, unpacked extensions from phone storage, the extensions page layout), DevTools on the phone, its fixes for Chromium bugs, accessibility fixes, un-expired flags, omnibox site search, the omnibox kept in phone behavior, and its feature overrides minus forced automatic picture-in-picture.

Dropped:

- The bundled Titanium Extension, downloaded unpinned at build time. It installs from its repo like any extension.
- Incognito as a separate window, from the desktop Android layout: private tabs stay in the tab switcher.
- (`kLoadAllTabsAtStartup` was dropped at first, then restored: extensions crash on restored tabs without it.)
- The software GL fallback for devices without GLES 3.
- Forced automatic picture-in-picture (`kAutoPictureInPictureAndroid`), which is off upstream: chrome://flags#auto-picture-in-picture-android turns it on.

## Dropped on 2026-10-09

Update cost for features outside the list above: the View source and Bookmark all tabs menu items. Delete browsing data on exit gave way to oganesson's no saved history on 2026-10-10: not keeping history at all says what's meant.

## Not taken

- Anti-fingerprinting patches (Cromite, Helium noise): they break sites, and the simplest reliable choice is none.
- Cromite's adblockers (eyeo, Bromite): uBlock Origin.
- Cromite's always-incognito mode.

## Our own

- `tennessine/jit-allowed-by-default`: see Vanadium above.
- The extensions puzzle button shows while no extension is pinned (`extensions-menu-button-when-nothing-pinned`): Titanium's `ext-pin` hides it once unpinned, and unpinning uBlock Origin too left the extensions out of reach.
- Firefox Sync, the same code as desktop oganesson (from dubna, with our Android side in `src/chrome/browser/firefox_sync/platform/`): passwords, bookmarks, history, open tabs, addresses and payment methods, and a custom server address. Settings → Firefox Sync, at the top like Fenix's account row (`firefox-sync-android`). The other devices' open tabs show in Recent tabs (app menu), one section per device, through Chromium's synced sessions (dubna's `firefox-sync-remote-tabs`), without Chromium's empty-state picture and its "sign in on all your devices" while they show (`recent-tabs-empty-state`); Chromium's own sign-in stays off, so the page shows no Google promo. Tabs go to and come from the account's other devices: "Send to your device" in the share sheet and a link's menu, and a received tab opens in the background with a notification (`firefox-sync-send-tabs`). Close requests from Firefox close the matching tabs. The browser checks for them when it comes to the front, and in the background through Chromium's background task scheduler at the interval set in Settings → Firefox Sync: every 15, 30 or 60 minutes, or only with each sync (`firefox-sync-receive-tabs`). The Rust library is built for arm64 (x86_64 for the emulator) by `tools/build-sync-lib.sh` and packaged with the APK (`firefox-sync-native-lib`).
- Page zoom follows Android's font size (`page-zoom-follows-font-size`): the zoom is Settings → Accessibility → Default zoom times Android's font scale, for the whole page, so a smaller Android font makes pages smaller as a smaller Android display size already does (Chromium's `AdjustForOSLevel`, shipped off). Tried first: text-only zoom, as Fennec's "Use system font size" (`git log` 2026-10-05); compared side by side on the maintainer's phone, whole-page zoom looked better. Pages capped at a max-width (purepc.pl's tiles, `max-width: 497px`) leave side gaps below 100%; a uBlock Origin `:style()` filter fixes that per site. Pages with a `text-scale` meta tag also scale their own text by the font scale, so it counts twice there. Changes apply on reload.
- `screen.width`, `screen.availWidth` and the rest of `window.screen` are in the page's CSS pixels, as Firefox reports them (`screen-size-follows-page-zoom`). Chromium reports device-independent pixels, which ignore page zoom, so below 100% a page that sizes its content from the screen (tibia.com's mobile tables, capped at `screen.availWidth - 30`) stopped short of the window. Changes apply on reload.
- The new tab page keeps the address bar where the user put it (`ntp-keeps-toolbar-position`, #1).
- Uploaded photos keep their location (`photo-location`). Without `ACCESS_MEDIA_LOCATION`, Android strips the GPS data from photos picked from its media store, such as the Images and Photos roots of the file chooser. The manifest lists it, and the file chooser asks for it when a page accepts images; denying it only keeps the stripping. Android's photo picker strips the location whatever the app holds.
- Swiping the toolbar between tabs no longer leaves the toolbar stuck on screen while the page scrolls (`toolbar-swipe-offset-tags`, a Chromium bug with viz-driven browser controls).
- A smaller APK (`smaller-apk`): the native libraries are stored compressed, deflated with Zopfli by `tools/repack_apk.py` before signing (Android reads only stored and deflate entries), and unpacked by Android at install; no XR module. About 190 MB instead of 340 MB per download, for about 110 MB more storage on the phone. F-Droid and Obtainium have no delta updates, so the download size is what each update costs.
- The layout follows the address bar (`layout-follows-address-bar`, #7). With the bar at the bottom, the app menu is reversed with its Back/Forward/Refresh row nearest the bar; typing in the address bar keeps it at the bottom above the keyboard, with suggestions on it, the best match nearest; the tab switcher's toolbar (new tab, tab count, grid toggle, search) sits below the tab list, and the tab list sits on it, the newest tab nearest, however few tabs there are. One setting rather than one per part: the mixed layouts put the most-used item far from the bar. Chromium moves the bar to the top while typing; its suggestion list already handled a bottom bar. With the bar at the top nothing changes.
- The toolbar's items follow an order set in Settings → Toolbar layout, the address bar included, and can be hidden, except the address bar and the menu (`toolbar-items`, #5).
- The toolbar hides on scroll only when Settings → Toolbar layout → Hide toolbar on scroll is on; off by default. When on, it hides as soon as a page commits, without Chromium's extra 3-second lock (`toolbar-hide-on-scroll`).
- In an installed web app, a link's long-press menu has Open in <default browser>, whichever browser that is, in place of Open in tennessine (`webapp-open-in-default-browser`), through the same launcher as the links below. Android has no way to open a link in another app in the background. The link is launched behind the web app (`ActivityOptions.makeTaskLaunchBehind`), which keeps it behind only when the browser's link handler is its final activity. Firefox and Chromium-based browsers hand the link to their main activity, which comes to the front: the web app then moves back in front of it, when Android allows that. With tennessine as the default, the link becomes a background tab without an app switch, in an open window or, with none open, at the next start.
- In dark mode, an installed web app's splash screen is dark when its manifest asks for a light one (`webapp-splash-dark-mode`): a background brighter than 205 of 255 goes through Blink's auto dark mode formula (CIELAB lightness inverted, near-black grays to `#121212`), so x.com's white becomes `#121212` and a colored one keeps its hue, darker. The icon and name switch to light text with it. Chromium's dark manifest colors don't help: Blink doesn't parse them, and home screen shortcuts never stored them. The splash can't know the page's own theme, so a site set to light flashes from dark to white.
- Tab containers, the same code as desktop oganesson (from dubna, with our Android side in `src/chrome/browser/containers/`): separate cookies and site data per container, links that stay in their tab's container, tabs that restore in their container, and Traffic Control's rules. A link's long-press menu has Open in container, a container tab's row in the tab switcher has a line in its color, and Settings → Containers edits the containers and the rules (`containers`, `containers-ui`; [containers.md](containers.md)).
- Links out of installed web apps, the same code as desktop oganesson (from dubna, with our Android side in `src/chrome/browser/app_links/`): a link in a web app that leaves its scope opens in the default browser, as one from any other app does, and so does a `target=_blank` link or `window.open` without window features. Redirects, pages a script loads, form posts, back and forward and popups stay in the app, so signing in keeps working. A dropped `pushState` from a tap fires `popstate`, and one URL opens per tap. Settings → Web apps lists the installed apps (WebAPKs and home screen shortcuts), and a site's page in Site settings links its apps: each has oganesson's "Pages that stay in the app" rules and the strict scope switch. The out-of-app bar follows the rules, and web apps don't prerender (`app-links`).
- Settings → App icon picks the launcher icon, one of several activity-aliases of the main activity. Every window shows the chosen icon in Recents through its task description, a window from "Open in new window" too, which starts without the alias (`app-icon`).
- "Open in new window" splits the screen only while Settings → Tabs → Open new windows side by side is on, as by default in Chromium, which leaves it to an experiment's parameter. Off, the new window opens full screen as a task of its own in Recents. In split screen, the new window always opens beside the old one (`new-window-side-by-side`).

## Planned

- Issues: what's left of the new tab page toolbar (#1).
- Helium bangs, when oganesson has its own bang list source.

## Builds

arm64 only, built locally (`tennessine.py build`), published to the F-Droid repository at https://packages.zakius.xyz/fdroid/repo (`tennessine.py publish`), and updated on the phone with F-Droid or Obtainium.
