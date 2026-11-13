#!/bin/bash
# Steps that need gclient's dependencies, sourced by tools/container-build.sh after the sync.
# Everything else is in patches/series.

# App icon: tennessine's (tools/app_icon.py), drawn over Vanadium's PNGs at their sizes. The other
# icons in Settings > App icon are in src/chrome/browser/app_icon.
mkdir -p chrome/android/java/res_tennessine_base/drawable
cp $SCRIPT_DIR/res/drawable/themed_app_icon.xml chrome/android/java/res_tennessine_base/drawable/themed_app_icon.xml
for icon in $(find chrome/android/java/res_tennessine_base -type f -name '*.png'); do $SCRIPT_DIR/res/icon.sh $icon; done

# DevTools fits a phone screen (third_party/devtools-frontend/src is a gclient dependency).
sed -i 's|<meta charset="utf-8">|&\n<meta name="viewport" content="width=device-width">|' third_party/devtools-frontend/src/front_end/entrypoint_template.html
sed -i '$a@media (max-width: 600px) { .settings-window-title, .tabbed-pane-header-tab-title { display: none; } .tabbed-pane-shadow.vertical-tab-layout .tabbed-pane-header-tabs { width: auto; } }' third_party/devtools-frontend/src/front_end/panels/settings/settingsScreen.css

# v8.h no longer pulls in v8-version.h, whose patch level changes with every Chromium release:
# through v8.h each release recompiled every file that uses V8 (thousands), though only V8's
# version.cc and the users of v8-version-string.h read the numbers, and they include it themselves
# (v8 is a gclient dependency).
sed -i '/^#include "v8-version.h"/d' v8/include/v8.h
