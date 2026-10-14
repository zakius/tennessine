# tennessine

Chromium for Android with a configurable toolbar, Helium's built-in uBlock Origin, Chrome
extensions, page scaling that follows Android's font size, a list-mode tab switcher, the full URL in the address bar, switches that stop saving history, Open in default browser for links in web apps, and
Firefox Sync. Built on [Vanadium](https://github.com/GrapheneOS/Vanadium), with
[Titanium](https://github.com/jqssun/android-titanium-browser)'s extension support.

> **Highly volatile.** This is a personal project. Features change or disappear without notice, some may misbehave, updates can break things, and development may stop at any moment. Use it knowing that.

## What it's for

A browser that is complete for the way its maintainer uses it, not one that tries to do everything: Vivaldi shows where that leads, with so many features that some never work well. What an extension does well stays an extension's job. Content blocking is uBlock Origin, the state of the art, built in through Helium's work, and Chrome extensions run at all thanks to Titanium. The browser itself only builds what an extension can't do: a toolbar laid out as you like, a bottom-first layout for one-handed use, page zoom that follows Android's font size, a list-mode tab switcher, Firefox Sync, switches that stop saving history, and an Open in default browser item in web apps' link menu.

## The name

Tennessine (Ts, element 117) is the neighbour of oganesson (118), after which its desktop sibling, [oganesson](https://github.com/zakius/oganesson), is named, and that is why it was picked. Both were first made at the Joint Institute for Nuclear Research in Dubna (tennessine in 2010, with Oak Ridge National Laboratory and other American labs; it is named after Tennessee): a coincidence, and the name of the [code they share](https://github.com/zakius/dubna).

## Install

Add the F-Droid repository `https://packages.zakius.xyz/fdroid/repo` (fingerprint below) in F-Droid or Obtainium. Updates install in the background once the store has installed the app once and its battery use is unrestricted.

## Layout

| Path | What |
|---|---|
| `vanadium/` | Submodule at a Vanadium tag. Its tag sets the Chromium version. Its patches apply as they are, renamed to tennessine, minus `patches/vanadium-drops.txt`. |
| `titanium/` | Submodule at the Titanium tag that `patches/titanium/` matches. Titanium edits with `sed` in `patch.sh`; ours are those edits as patches, so a change fails loudly. |
| `helium/` | Submodule at the [Helium](https://github.com/imputnet/helium) tag that desktop oganesson follows. The uBlock Origin patches and version come from it, as they are. |
| `dubna/` | Submodule: [dubna](https://github.com/zakius/dubna), what tennessine shares with [desktop oganesson](https://github.com/zakius/oganesson): Firefox Sync (C++, strings, translations, Rust library), no saved history, and the update policy that keeps both on one Chromium milestone. Its Android side is ours: `src/chrome/browser/firefox_sync/platform/` (dubna's `firefox_sync_platform.h`) and `android/` (the settings screen). |
| `patches/series` | Everything applied after Vanadium, in order. Paths from the repo root. |
| `src/` | New files copied over the tree, after `dubna/src/`. |
| `tennessine.py` | Every command: `apply`, `check`, `refresh`, `upstream`, `build`, `publish`. Run it without arguments for help. |

## Updating to a new Chromium

`./tennessine.py upstream` lists what is new upstream; it prints nothing when every submodule is current. dubna's update policy (`dubna/README.md`) says when to take it: a new release in the milestone at once, a new milestone only once desktop's upstream has it too, a fix for an exploit in the wild at once.

1. `git -C vanadium fetch --tags && git -C vanadium checkout <tag>` (Vanadium tags follow Chromium stable).
2. `git -C titanium fetch --tags && git -C titanium diff HEAD v<version> -- patch.sh`, port what's new into `patches/titanium/`, then `git -C titanium checkout v<version>`.
3. `git -C helium checkout <commit>` at the Helium commit desktop oganesson's `helium` submodule moves to.
4. `./tennessine.py check`: applies everything to a shallow checkout of the new version (`work/chromium/src`, about 30 s after the first fetch) and lists what fails.
5. For each failure, `./tennessine.py check --stop`, then in `work/chromium/src`: `../../../tennessine.py refresh ../../../<patch>`. Read every hunk it reports with fuzz: fuzz can land a hunk inside the wrong `#if` or mid-expression. Fix those in the tree and take the fix with `../../../tennessine.py refresh <patch> <file>`.
6. `./tennessine.py build`, install, commit.

To take shared fixes (Firefox Sync, no saved history): `git -C dubna pull`, then `./tennessine.py check`.

## Building

`./tennessine.py build` builds an arm64 APK in a rootless Ubuntu container (podman,
`tools/container-build.sh`): Chromium checkout, sync, patches, compile, and signing with the APK
key in `~/.local/share/tennessine-signing`, which every update must be signed with. `./tennessine.py build resume` recompiles after a change to `src/` only. `build.sh`,
`common.sh` and `post-sync.sh` come from Titanium and run inside the container.

Each build's versionCode is 10^9 plus the minutes since 2026-01-01 (`set_version_code` in
`common.sh`), so a rebuild on the same Chromium version is still an update.

`./tennessine.py publish` publishes the APK in the F-Droid repository
`https://packages.zakius.xyz/fdroid/repo` (fingerprint
`A40055F0D1CF2D8B22F0DCD2CE8B874DB15352E5B6DC262C3B4FEA9FD46ADFBE`), for F-Droid or Obtainium. Its
index signing key is in `~/.local/share/tennessine-fdroid`.

Both keys exist only on this machine: back up `~/.local/share/tennessine-signing` and
`~/.local/share/tennessine-fdroid`. Losing the APK key means installed copies can't update.

`docs/patch-decisions.md` has why each patch is in or out.
