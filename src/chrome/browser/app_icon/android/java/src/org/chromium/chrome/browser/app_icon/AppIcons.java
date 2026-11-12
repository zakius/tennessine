// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.app_icon;

import android.content.ComponentName;
import android.content.Context;
import android.content.pm.PackageManager;

import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;

import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * The launcher icons in Settings → App icon. Each is an activity-alias of ChromeTabbedActivity in
 * AndroidManifest.xml (tennessine/app-icon.patch) holding the launcher category, and exactly one
 * is enabled. The manifest enables the first; a choice is stored only as the aliases' states.
 */
@NullMarked
public final class AppIcons {
    private static final String PREFIX = "org.chromium.chrome.browser.app_icon.";

    /** The aliases' names after PREFIX, in the manifest's order; the first is the default. */
    static final List<String> NAMES =
            Arrays.asList("Indigo", "Teal", "Crimson", "Black", "White");

    private AppIcons() {}

    /** Whether {@code className} is one of the aliases. */
    public static boolean isAlias(@Nullable String className) {
        return className != null && className.startsWith(PREFIX);
    }

    /** {@code names} and every alias, for ChromeTabbedActivity's set of its component names. */
    public static Set<String> withAliases(String... names) {
        Set<String> all = new HashSet<>(Arrays.asList(names));
        for (String name : NAMES) all.add(PREFIX + name);
        return Collections.unmodifiableSet(all);
    }

    /** The enabled alias. */
    static String current(Context context) {
        PackageManager pm = context.getPackageManager();
        for (String name : NAMES) {
            if (pm.getComponentEnabledSetting(component(context, name))
                    == PackageManager.COMPONENT_ENABLED_STATE_ENABLED) {
                return name;
            }
        }
        return NAMES.get(0);
    }

    /** Enables {@code chosen}, then disables the rest, so the launcher always has an entry. */
    static void select(Context context, String chosen) {
        PackageManager pm = context.getPackageManager();
        pm.setComponentEnabledSetting(
                component(context, chosen),
                PackageManager.COMPONENT_ENABLED_STATE_ENABLED,
                PackageManager.DONT_KILL_APP);
        for (String name : NAMES) {
            if (name.equals(chosen)) continue;
            pm.setComponentEnabledSetting(
                    component(context, name),
                    PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
                    PackageManager.DONT_KILL_APP);
        }
    }

    private static ComponentName component(Context context, String name) {
        return new ComponentName(context.getPackageName(), PREFIX + name);
    }
}
