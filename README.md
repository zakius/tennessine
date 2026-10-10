# tennessine

Chromium for Android with a configurable toolbar, Helium's built-in uBlock Origin, Chrome
extensions, page scaling that follows Android's font size, a list-mode tab switcher, the full URL in the address bar, switches that stop saving history, Open in default browser for links in web apps, and
Firefox Sync. Built on [Vanadium](https://github.com/GrapheneOS/Vanadium), with
[Titanium](https://github.com/jqssun/android-titanium-browser)'s extension support.

## Layout

| Path | What |
|---|---|
| `vanadium/` | Submodule at a Vanadium tag. Its tag sets the Chromium version. Its patches apply as they are, renamed to tennessine, minus `patches/vanadium-drops.txt`. |
| `titanium/` | Submodule at the Titanium tag that `patches/titanium/` matches. Titanium edits with `sed` in `patch.sh`; ours are those edits as patches, so a change fails loudly. |
| `oganesson/` | Submodule: [desktop oganesson](https://github.com/zakius/oganesson-focused). The uBlock Origin patches and version, and Firefox Sync (C++, strings, translations, Rust library) come from it; only `src/chrome/browser/firefox_sync/android/` is ours. |
| `patches/series` | Everything applied after Vanadium, in order. Paths from the repo root. |
| `src/` | New files copied over the tree. |

## Updating to a new Chromium

`tools/upstream.sh` lists what is new upstream; it prints nothing when all three submodules are current.

1. `git -C vanadium fetch --tags && git -C vanadium checkout <tag>` (Vanadium tags follow Chromium stable).
2. `git -C titanium fetch --tags && git -C titanium diff HEAD v<version> -- patch.sh`, port what's new into `patches/titanium/`, then `git -C titanium checkout v<version>`.
3. `./check.sh`: applies everything to a shallow checkout of the new version (`work/chromium/src`, about 30 s after the first fetch) and lists what fails.
4. For each failure, `STOP=1 ./check.sh`, then in `work/chromium/src`: `../../../tools/refresh.sh ../../../<patch>`. Read every hunk it reports with fuzz: fuzz can land a hunk inside the wrong `#if` or mid-expression. Fix those in the tree and take the fix with `tools/refresh.sh <patch> <file>`.
5. `./local-build.sh`, install, commit.

To take desktop fixes (uBlock Origin, Firefox Sync): `git -C oganesson pull`, then `./check.sh`.

## Building

`./local-build.sh` builds an arm64 APK in a rootless Ubuntu container: Chromium checkout, sync,
patches, compile, signing with a local test key. `./local-build.sh resume` recompiles after a
change to `src/` only.

`docs/patch-decisions.md` has why each patch is in or out.
