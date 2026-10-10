// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.clear_on_exit;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.content.SharedPreferences;

import org.chromium.base.ContextUtils;
import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.browsing_data.BrowsingDataBridge;
import org.chromium.chrome.browser.browsing_data.TimePeriod;
import org.chromium.chrome.browser.profiles.Profile;

import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Delete browsing data on exit, as in Fenix, but also when the app is swiped away from recents:
 * the chosen data types are cleared when the user quits from the menu, and at the next start
 * when the previous process ended at the user's request. A process the system killed (for
 * memory, for instance) keeps everything. Open tabs are Vanadium's close-tabs-on-exit pref,
 * gated by the same check (TabPreferencesUtils).
 */
@NullMarked
public final class ClearOnExit {
    /** BrowsingDataType values to clear, as strings. */
    static final String PREF_TYPES = "tennessine.clear_on_exit.types";
    /** Set by the Exit menu item, so the next start knows the user quit. */
    private static final String PREF_QUIT_REQUESTED = "tennessine.clear_on_exit.quit_requested";

    private static @Nullable Boolean sLastExitWasByUser;
    private static boolean sClearedAtStart;

    private ClearOnExit() {}

    static SharedPreferences prefs() {
        return ContextUtils.getAppSharedPreferences();
    }

    static Set<String> getTypes() {
        return new HashSet<>(prefs().getStringSet(PREF_TYPES, new HashSet<>()));
    }

    static void setType(int type, boolean enabled) {
        Set<String> types = getTypes();
        if (enabled) {
            types.add(String.valueOf(type));
        } else {
            types.remove(String.valueOf(type));
        }
        prefs().edit().putStringSet(PREF_TYPES, types).apply();
    }

    /**
     * Whether the previous process ended because the user quit: the Exit menu item, swiping the
     * task away from recents, or a force stop. Decided once per process.
     */
    public static boolean lastExitWasByUser() {
        if (sLastExitWasByUser == null) {
            sLastExitWasByUser = readLastExit();
            prefs().edit().remove(PREF_QUIT_REQUESTED).apply();
        }
        return sLastExitWasByUser;
    }

    private static boolean readLastExit() {
        if (prefs().getBoolean(PREF_QUIT_REQUESTED, false)) return true;
        Context context = ContextUtils.getApplicationContext();
        ActivityManager am = context.getSystemService(ActivityManager.class);
        if (am == null) return false;
        List<ApplicationExitInfo> exits =
                am.getHistoricalProcessExitReasons(context.getPackageName(), 0, 1);
        if (exits.isEmpty()) return false;
        ApplicationExitInfo last = exits.get(0);
        // Swiping the task away from recents and a force stop are REASON_USER_REQUESTED
        // (the swipe with SUBREASON_REMOVE_TASK); some releases report the swipe as
        // REASON_OTHER, "remove task". Low-memory and other system kills are left alone.
        String description = last.getDescription();
        return last.getReason() == ApplicationExitInfo.REASON_USER_REQUESTED
                || (last.getReason() == ApplicationExitInfo.REASON_OTHER
                        && description != null
                        && description.contains("remove task"));
    }

    /** Clears the chosen data once per process if the user quit last time. */
    public static void clearAtStartIfNeeded(Profile profile) {
        if (sClearedAtStart) return;
        sClearedAtStart = true;
        if (lastExitWasByUser()) clear(profile, null);
    }

    /** The Exit menu item: clear the chosen data, then quit; the next start clears again. */
    public static void quit(Profile profile, Runnable terminate) {
        prefs().edit().putBoolean(PREF_QUIT_REQUESTED, true).commit();
        clear(profile, terminate);
    }

    private static void clear(Profile profile, @Nullable Runnable done) {
        Set<String> types = getTypes();
        int[] dataTypes = new int[types.size()];
        int i = 0;
        for (String type : types) dataTypes[i++] = Integer.parseInt(type);
        if (dataTypes.length == 0) {
            if (done != null) done.run();
            return;
        }
        BrowsingDataBridge.getForProfile(profile)
                .clearBrowsingData(
                        () -> {
                            if (done != null) done.run();
                        },
                        dataTypes,
                        TimePeriod.ALL_TIME);
    }

    /** Whether Vanadium's close-tabs-on-exit pref should act at this start. */
    public static boolean shouldCloseTabs(boolean closeTabsPref) {
        return closeTabsPref && lastExitWasByUser();
    }
}
