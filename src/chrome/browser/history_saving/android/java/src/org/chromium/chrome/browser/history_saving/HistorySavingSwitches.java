// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.history_saving;

import androidx.preference.PreferenceGroup;

import org.chromium.build.annotations.NullMarked;
import org.chromium.chrome.browser.profiles.Profile;
import org.chromium.components.browser_ui.settings.ChromeSwitchPreference;
import org.chromium.components.prefs.PrefService;
import org.chromium.components.user_prefs.UserPrefs;

/**
 * Privacy: oganesson's switches that stop saving browsing and download history
 * (oganesson/patches/oganesson/core/no-saved-history.patch does the work).
 */
@NullMarked
public final class HistorySavingSwitches {
    // components/history/core/common/pref_names.cc, components/sessions/core/closed_tabs_in_memory.h
    // and chrome/browser/download/download_history_saving.h.
    private static final String SAVING_DISABLED = "history.saving_disabled";
    private static final String KEEP_CLOSED_TABS = "oganesson.history.keep_closed_tabs";
    private static final String DOWNLOADS_SAVING_DISABLED = "oganesson.downloads.saving_disabled";

    private HistorySavingSwitches() {}

    /**
     * Adds the switches to {@code group} with {@code order}. Preferences of equal order sort by
     * title, so the titles all start with "Don't save" to stay together.
     */
    public static void addTo(PreferenceGroup group, Profile profile, int order) {
        PrefService prefs = UserPrefs.get(profile);
        ChromeSwitchPreference history =
                addSwitch(
                        group,
                        order,
                        "Don't save browsing history",
                        "Visited pages aren't saved, and Firefox Sync skips history. History"
                                + " saved before stays until you delete it.",
                        prefs.getBoolean(SAVING_DISABLED));
        // Only matters while history isn't saved. Shown inverted: on means keep nothing.
        ChromeSwitchPreference closedTabs =
                addSwitch(
                        group,
                        order,
                        "Don't save closed tabs",
                        "Recent tabs can't reopen closed tabs. When off, closed tabs are kept"
                                + " in memory only, until the browser closes.",
                        !prefs.getBoolean(KEEP_CLOSED_TABS));
        closedTabs.setVisible(history.isChecked());
        history.setOnPreferenceChangeListener(
                (pref, value) -> {
                    prefs.setBoolean(SAVING_DISABLED, (Boolean) value);
                    closedTabs.setVisible((Boolean) value);
                    return true;
                });
        closedTabs.setOnPreferenceChangeListener(
                (pref, value) -> {
                    prefs.setBoolean(KEEP_CLOSED_TABS, !(Boolean) value);
                    return true;
                });
        ChromeSwitchPreference downloads =
                addSwitch(
                        group,
                        order,
                        "Don't save download history",
                        "Downloads are listed until you close the browser. Downloads saved"
                                + " before stay until you delete them.",
                        prefs.getBoolean(DOWNLOADS_SAVING_DISABLED));
        downloads.setOnPreferenceChangeListener(
                (pref, value) -> {
                    prefs.setBoolean(DOWNLOADS_SAVING_DISABLED, (Boolean) value);
                    return true;
                });
    }

    private static ChromeSwitchPreference addSwitch(
            PreferenceGroup group, int order, String title, String summary, boolean checked) {
        ChromeSwitchPreference toggle = new ChromeSwitchPreference(group.getContext());
        toggle.setOrder(order);
        toggle.setTitle(title);
        toggle.setSummary(summary);
        toggle.setChecked(checked);
        group.addPreference(toggle);
        return toggle;
    }
}
