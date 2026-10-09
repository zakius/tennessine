# tennessine

Chromium for Android with a configurable toolbar, Helium's built-in uBlock Origin, Chrome
extensions, page scaling that follows Android's font size, a list-mode tab switcher, and
Firefox Sync. Built on [Vanadium](https://github.com/GrapheneOS/Vanadium), with
[Titanium](https://github.com/jqssun/android-titanium-browser)'s extension support.

## Layout

| Path | What |
|---|---|
| `vanadium/` | Submodule at a Vanadium tag. Its tag sets the Chromium version. Its patches apply as they are, renamed to tennessine, minus `patches/vanadium-drops.txt`. |
| `titanium/` | Submodule at the Titanium tag that `patches/titanium/` matches. Titanium edits with `sed` in `patch.sh`; ours are those edits as patches, so a change fails loudly. |
| `oganesson/` | Submodule: [desktop oganesson](https://github.com/zakius/oganesson-focused). The uBlock Origin patches and version come from it. |
| `patches/series` | Everything applied after Vanadium, in order. Paths from the repo root. |
| `src/` | New files copied over the tree. |
| `rust/` | The Firefox Sync library. |

## Updating to a new Chromium

1. `git -C vanadium fetch --tags && git -C vanadium checkout <tag>` (Vanadium tags follow Chromium stable).
2. `git -C titanium fetch --tags && git -C titanium diff HEAD v<version> -- patch.sh`, port what's new into `patches/titanium/`, then `git -C titanium checkout v<version>`.
3. `./check.sh`: applies everything to a sparse checkout of the new version and lists what fails.
4. Fix the failing patches against `work/check/src` (`git diff` there gives the new hunks).
5. `./local-build.sh`, install, commit.

To take desktop fixes (uBlock Origin): `git -C oganesson pull`.

## Building

`./local-build.sh` builds an arm64 APK in a rootless Ubuntu container: Chromium checkout, sync,
patches, compile, signing with a local test key. `./local-build.sh resume` recompiles after a
change to `src/` only.

`docs/patch-decisions.md` has why each patch is in or out.
