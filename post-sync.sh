#!/bin/bash
# Steps that need gclient's dependencies, sourced by build.sh after the sync. Everything else is
# in patches/series.

# App icon: Titanium's tinted Vanadium icon until tennessine has its own.
mkdir -p chrome/android/java/res_tennessine_base/drawable
cp $SCRIPT_DIR/res/drawable/themed_app_icon.xml chrome/android/java/res_tennessine_base/drawable/themed_app_icon.xml
for icon in $(find chrome/android/java/res_tennessine_base -type f -name '*.png'); do convert $icon -fill navy -tint 36 $icon && $SCRIPT_DIR/res/icon.sh $icon; done

# DevTools fits a phone screen (third_party/devtools-frontend/src is a gclient dependency).
sed -i 's|<meta charset="utf-8">|&\n<meta name="viewport" content="width=device-width">|' third_party/devtools-frontend/src/front_end/entrypoint_template.html
sed -i '$a@media (max-width: 600px) { .settings-window-title, .tabbed-pane-header-tab-title { display: none; } .tabbed-pane-shadow.vertical-tab-layout .tabbed-pane-header-tabs { width: auto; } }' third_party/devtools-frontend/src/front_end/panels/settings/settingsScreen.css
