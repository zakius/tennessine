# tennessine

tennessine is Chromium for Android, built on [Vanadium](https://github.com/GrapheneOS/Vanadium) with [Titanium](https://github.com/jqssun/android-titanium-browser)'s extension support. It adds a configurable toolbar, uBlock Origin (installed on first run), Chrome extensions, Firefox-compatible Sync, and switches that stop saving history. Its desktop sibling is [oganesson](https://github.com/zakius/oganesson), and the code the two share is in [dubna](https://github.com/zakius/dubna).

> **Highly volatile.** This is a personal project. Features change or disappear without notice, some may misbehave, updates can break things, and development may stop at any moment. Use it knowing that.

## What it's for

tennessine is meant to be complete for the way the maintainer uses it. It does not try to do everything. Vivaldi tries to do too much, and some of its features never work well.

Where an extension does the job well, the job stays with the extension. Content blocking is uBlock Origin, the state of the art, in the build Helium maintains, which the browser downloads and installs on first run. Other Chrome extensions run thanks to Titanium. The browser builds natively only what an extension can't do:

- A toolbar laid out as you like, and a bottom-first layout for one-handed use.
- Page zoom that follows Android's font size.
- A list-mode tab switcher and the full URL in the address bar, as [Cromite](https://github.com/uazo/cromite) has them.
- Firefox-compatible Sync. It works best with a self-hosted sync server.
- Switches that stop saving history.
- An Open in default browser item in the link menu of installed web apps.

[docs/patch-decisions.md](docs/patch-decisions.md) explains why each patch is in or out.

## The name

Tennessine (Ts, element 117) was picked only because it is the neighbour of oganesson (118), the element its desktop sibling [oganesson](https://github.com/zakius/oganesson) is named after. Tennessine is named after Tennessee. It was first made in 2010 at the Joint Institute for Nuclear Research in Dubna, with Oak Ridge National Laboratory and other American labs. Oganesson was first made there too, in 2002, with Lawrence Livermore National Laboratory. That is a coincidence, and it gave the shared repo, [dubna](https://github.com/zakius/dubna), its name.

## Install

Add this F-Droid repository in F-Droid or Obtainium:

```
https://packages.zakius.xyz/fdroid/repo
```

Fingerprint:

```
A40055F0D1CF2D8B22F0DCD2CE8B874DB15352E5B6DC262C3B4FEA9FD46ADFBE
```

After the store installs the app for the first time, it installs updates in the background, provided the store's battery use is unrestricted.

## Layout

| Path | What |
|---|---|
| `vanadium/` | Submodule at a Vanadium tag, which sets the Chromium version. Its patches apply as they are, renamed to tennessine, minus `patches/vanadium-drops.txt`. |
| `titanium/` | Submodule at the Titanium tag that `patches/titanium/` matches. Titanium makes its changes with `sed` in `patch.sh`. Ours are those changes as patches, so a change that no longer applies fails loudly. |
| `dubna/` | Submodule: [dubna](https://github.com/zakius/dubna), what tennessine shares with oganesson. That is Firefox-compatible Sync (C++, strings, translations, Rust library), no saved history, and the update policy that keeps both browsers on one Chromium milestone. The Android side of it is here: `src/chrome/browser/firefox_sync/platform/` (dubna's `firefox_sync_platform.h`) and `android/` (the settings screen). |
| `patches/series` | Everything applied after Vanadium, in order. Paths are relative to the repo root. |
| `src/` | New files copied over the tree, after `dubna/src/`. |
| `tennessine.py` | Every command: `apply`, `check`, `refresh`, `upstream`, `build`, `publish`. Run it without arguments for help. |

## Build

`./tennessine.py build` builds an arm64 APK in a rootless Ubuntu container (podman, `tools/container-build.sh`). It checks out and syncs Chromium, applies the patches, compiles, and signs the APK with the key in `~/.local/share/tennessine-signing`. Every update must be signed with that key.

```sh
./tennessine.py build           # full build
./tennessine.py build resume    # recompile after a change to src/ only
```

`build.sh`, `common.sh` and `post-sync.sh` come from Titanium and run inside the container.

Each build's versionCode is 10^9 plus the minutes since 2026-01-01 (`set_version_code` in `common.sh`). A rebuild on the same Chromium version is therefore still an update.

## Updating

`./tennessine.py upstream` lists what is new upstream. It prints nothing when every submodule is current. dubna's [update policy](https://github.com/zakius/dubna#update-policy) says when to update: a new release in the milestone at once, a new milestone only when oganesson's upstream has it too, and a fix for an exploit in the wild at once.

1. Move Vanadium to the new tag. Vanadium tags follow Chromium stable.
   ```sh
   git -C vanadium fetch --tags && git -C vanadium checkout <tag>
   ```
2. Compare Titanium's `patch.sh` with the new version and port what is new into `patches/titanium/`. Then check out the new version.
   ```sh
   git -C titanium fetch --tags && git -C titanium diff HEAD v<version> -- patch.sh
   git -C titanium checkout v<version>
   ```
3. Run `./tennessine.py check`. It applies everything to a shallow checkout of the new version in `work/chromium/src` and lists what fails. After the first fetch, this takes about 30 seconds.
4. For each failure, run `./tennessine.py check --stop`, then refresh the patch from `work/chromium/src`:
   ```sh
   ../../../tennessine.py refresh ../../../<patch>
   ```
   Read every hunk it reports with fuzz, because fuzz can put a hunk inside the wrong `#if` or in the middle of an expression. Fix those hunks in the tree and take the fix into the patch with `../../../tennessine.py refresh <patch> <file>`.
5. Run `./tennessine.py build`, install the APK, and commit.

To take shared fixes (Firefox-compatible Sync, no saved history), run `git -C dubna pull`, then `./tennessine.py check`.

## Releasing

`./tennessine.py publish` publishes the APK in the F-Droid repository listed under [Install](#install). The repository's index signing key is in `~/.local/share/tennessine-fdroid`.

> **Back up both keys.** The APK key (`~/.local/share/tennessine-signing`) and the index key (`~/.local/share/tennessine-fdroid`) exist only on the build machine. If the APK key is lost, installed copies can't update.

## Docs

[Patch decisions](docs/patch-decisions.md): why each patch is in or out.

## License

tennessine's own code is GPL-2.0-or-later. Vanadium's patches are GPL-2.0-only, and GrapheneOS's license notes rule out combining them with GPL-3.0 code, so the APK is GPL-2.0 (`LICENSE`) and contains no GPL-3.0 code: dubna is used under its GPL-2.0 option, Titanium's changes are GPL-2.0, and Chromium and ungoogled-chromium are BSD-licensed. Helium's and Cromite's patches are GPL-3.0, so none are built in; the list-mode tab switcher and the full URL are our own implementations.

uBlock Origin (GPL-3.0) is a separate program: the browser downloads Helium's build from [imputnet/uBlock](https://github.com/imputnet/uBlock) on first run, checks it against a pinned SHA-256, and installs it as an extension (`src/chrome/browser/ublock_installer/`). `./tennessine.py upstream` reports a newer release; the pin is in that folder. Its CNAME uncloaking uses our own DNS API (`patches/tennessine/dns-api.patch`), written for tennessine in place of Helium's.
