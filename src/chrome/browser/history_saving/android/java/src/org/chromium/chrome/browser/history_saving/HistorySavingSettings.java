// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.history_saving;

import android.content.Context;
import android.os.Bundle;

import androidx.preference.PreferenceScreen;

import org.chromium.base.supplier.MonotonicObservableSupplier;
import org.chromium.base.supplier.ObservableSuppliers;
import org.chromium.base.supplier.SettableMonotonicObservableSupplier;
import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.settings.ChromeBaseSettingsFragment;
import org.chromium.components.browser_ui.settings.ChromeSwitchPreference;
import org.chromium.components.prefs.PrefService;
import org.chromium.components.user_prefs.UserPrefs;

/**
 * Privacy → History: oganesson's switches that stop saving browsing and download history
 * (oganesson/patches/oganesson/core/no-saved-history.patch does the work).
 */
@NullMarked
public class HistorySavingSettings extends ChromeBaseSettingsFragment {
    // components/history/core/common/pref_names.cc, components/sessions/core/closed_tabs_in_memory.h
    // and chrome/browser/download/download_history_saving.h.
    private static final String SAVING_DISABLED = "history.saving_disabled";
    private static final String KEEP_CLOSED_TABS = "oganesson.history.keep_closed_tabs";
    private static final String DOWNLOADS_SAVING_DISABLED = "oganesson.downloads.saving_disabled";

    private final SettableMonotonicObservableSupplier<String> mPageTitle =
            ObservableSuppliers.createMonotonic();

    @Override
    public void onCreatePreferences(@Nullable Bundle savedInstanceState, @Nullable String rootKey) {
        mPageTitle.set(getString(R.string.history_saving_title));
        Context context = getPreferenceManager().getContext();
        PreferenceScreen screen = getPreferenceManager().createPreferenceScreen(context);
        setPreferenceScreen(screen);
        PrefService prefs = UserPrefs.get(getProfile());

        ChromeSwitchPreference history =
                addSwitch(
                        screen,
                        prefs,
                        SAVING_DISABLED,
                        "Don't save browsing history",
                        "Visited pages aren't saved, and Firefox Sync skips history. History"
                                + " saved before stays until you delete it.");
        // Only matters while history isn't saved.
        ChromeSwitchPreference keepClosedTabs =
                addSwitch(
                        screen,
                        prefs,
                        KEEP_CLOSED_TABS,
                        "Reopen closed tabs until the browser closes",
                        "Closed tabs are kept in memory only, never on disk, so Recent tabs can"
                                + " reopen them. Turn off to keep nothing.");
        keepClosedTabs.setVisible(prefs.getBoolean(SAVING_DISABLED));
        history.setOnPreferenceChangeListener(
                (pref, value) -> {
                    prefs.setBoolean(SAVING_DISABLED, (Boolean) value);
                    keepClosedTabs.setVisible((Boolean) value);
                    return true;
                });
        addSwitch(
                screen,
                prefs,
                DOWNLOADS_SAVING_DISABLED,
                "Don't save download history",
                "Downloads are listed until you close the browser. Downloads saved before stay"
                        + " until you delete them.");
    }

    private static ChromeSwitchPreference addSwitch(
            PreferenceScreen screen, PrefService prefs, String pref, String title, String summary) {
        ChromeSwitchPreference toggle = new ChromeSwitchPreference(screen.getContext());
        toggle.setTitle(title);
        toggle.setSummary(summary);
        toggle.setChecked(prefs.getBoolean(pref));
        toggle.setOnPreferenceChangeListener(
                (p, value) -> {
                    prefs.setBoolean(pref, (Boolean) value);
                    return true;
                });
        screen.addPreference(toggle);
        return toggle;
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
