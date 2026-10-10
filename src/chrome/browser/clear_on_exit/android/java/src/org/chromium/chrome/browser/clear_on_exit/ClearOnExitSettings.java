// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.clear_on_exit;

import android.content.Context;
import android.os.Bundle;

import androidx.preference.Preference;
import androidx.preference.PreferenceScreen;

import org.chromium.base.shared_preferences.SharedPrefsUtils.SharedPrefsExt;
import org.chromium.base.supplier.MonotonicObservableSupplier;
import org.chromium.base.supplier.ObservableSuppliers;
import org.chromium.base.supplier.SettableMonotonicObservableSupplier;
import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.browsing_data.BrowsingDataType;
import org.chromium.chrome.browser.settings.ChromeBaseSettingsFragment;
import org.chromium.components.browser_ui.settings.ChromeSwitchPreference;

import java.util.Set;

/** Privacy → Delete browsing data on exit: a switch per data type, as in Fenix. */
@NullMarked
public class ClearOnExitSettings extends ChromeBaseSettingsFragment {
    private final SettableMonotonicObservableSupplier<String> mPageTitle =
            ObservableSuppliers.createMonotonic();

    @Override
    public void onCreatePreferences(@Nullable Bundle savedInstanceState, @Nullable String rootKey) {
        mPageTitle.set(getString(R.string.clear_on_exit_title));
        Context context = getPreferenceManager().getContext();
        PreferenceScreen screen = getPreferenceManager().createPreferenceScreen(context);
        setPreferenceScreen(screen);

        Preference about = new Preference(context);
        about.setSummary(
                "Deleted when you choose Exit in the menu or swipe the browser away from"
                        + " recent apps, not when Android closes it in the background.");
        about.setSelectable(false);
        screen.addPreference(about);

        ChromeSwitchPreference tabs = new ChromeSwitchPreference(context);
        tabs.setTitle("Open tabs");
        tabs.setChecked(SharedPrefsExt.CLOSE_TABS_ON_EXIT.get());
        tabs.setOnPreferenceChangeListener(
                (pref, value) -> {
                    SharedPrefsExt.CLOSE_TABS_ON_EXIT.put((Boolean) value);
                    return true;
                });
        screen.addPreference(tabs);

        Set<String> types = ClearOnExit.getTypes();
        addType(screen, types, BrowsingDataType.HISTORY, "Browsing history");
        addType(screen, types, BrowsingDataType.SITE_DATA, "Cookies and site data");
        addType(screen, types, BrowsingDataType.CACHE, "Cached images and files");
        addType(screen, types, BrowsingDataType.SITE_SETTINGS, "Site permissions");
    }

    private static void addType(PreferenceScreen screen, Set<String> types, int type, String title) {
        ChromeSwitchPreference pref = new ChromeSwitchPreference(screen.getContext());
        pref.setTitle(title);
        pref.setChecked(types.contains(String.valueOf(type)));
        pref.setOnPreferenceChangeListener(
                (p, value) -> {
                    ClearOnExit.setType(type, (Boolean) value);
                    return true;
                });
        screen.addPreference(pref);
    }

    @Override
    public MonotonicObservableSupplier<String> getPageTitle() {
        return mPageTitle;
    }

    @Override
    public @AnimationType int getAnimationType() {
        return AnimationType.PROPERTY;
    }
}
