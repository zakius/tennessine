// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.toolbar_items;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Bundle;

import androidx.appcompat.app.AlertDialog;
import androidx.preference.Preference;
import androidx.preference.PreferenceScreen;

import org.chromium.base.ContextUtils;
import org.chromium.base.supplier.MonotonicObservableSupplier;
import org.chromium.base.supplier.ObservableSuppliers;
import org.chromium.base.supplier.SettableMonotonicObservableSupplier;
import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.lifetime.ApplicationLifetime;
import org.chromium.chrome.browser.settings.ChromeBaseSettingsFragment;
import org.chromium.components.browser_ui.settings.ChromeSwitchPreference;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Settings → Toolbar layout: the order of the toolbar items, left to right, and which are shown.
 * Stored as comma-separated tokens in the app preference read by ToolbarPhone; a leading "-" hides
 * an item. Items before the address bar sit on its left, the rest on its right.
 */
@NullMarked
public class ToolbarItemsSettings extends ChromeBaseSettingsFragment {
    static final String PREF = "tennessine.toolbar.items";
    /** What ToolbarPhone applied when it inflated; a different PREF needs a restart. */
    static final String APPLIED_PREF = "tennessine.toolbar.items.applied";
    /** Read by TabStateBrowserControlsVisibilityDelegate (tennessine/toolbar-hide-on-scroll). */
    static final String HIDE_ON_SCROLL_PREF = "tennessine.toolbar.hide_on_scroll";
    static final List<String> DEFAULT =
            Arrays.asList("home", "location", "shortcut", "extensions", "tabs", "menu");

    private final SettableMonotonicObservableSupplier<String> mPageTitle =
            ObservableSuppliers.createMonotonic();

    @Override
    public void onCreatePreferences(@Nullable Bundle savedInstanceState, @Nullable String rootKey) {
        mPageTitle.set(getString(R.string.toolbar_items_title));
        setPreferenceScreen(
                getPreferenceManager().createPreferenceScreen(getPreferenceManager().getContext()));
        render();
    }

    @Override
    public MonotonicObservableSupplier<String> getPageTitle() {
        return mPageTitle;
    }

    @Override
    public @AnimationType int getAnimationType() {
        return AnimationType.PROPERTY;
    }

    private static SharedPreferences prefs() {
        return ContextUtils.getAppSharedPreferences();
    }

    /** The stored order, with items missing from it (new ones) added at the end. */
    static List<String> load() {
        List<String> items = new ArrayList<>();
        for (String token : prefs().getString(PREF, "").split(",")) {
            String name = token.trim().replaceFirst("^-", "");
            if (DEFAULT.contains(name) && !items.contains(name) && !items.contains("-" + name)) {
                items.add(token.trim());
            }
        }
        for (String name : DEFAULT) {
            if (!items.contains(name) && !items.contains("-" + name)) items.add(name);
        }
        return items;
    }

    private void save(List<String> items) {
        prefs().edit().putString(PREF, String.join(",", items)).apply();
        render();
    }

    private static String label(String name) {
        switch (name) {
            case "home":
                return "Home button";
            case "location":
                return "Address bar";
            case "shortcut":
                return "Toolbar shortcut";
            case "extensions":
                return "Extensions";
            case "tabs":
                return "Tab switcher";
            default:
                return "Menu";
        }
    }

    private void render() {
        Context context = getPreferenceManager().getContext();
        PreferenceScreen screen = getPreferenceScreen();
        screen.removeAll();
        List<String> items = load();

        // First, so a pending change isn't missed below the list.
        if (!prefs().getString(PREF, "").equals(prefs().getString(APPLIED_PREF, ""))) {
            Preference restart = new Preference(context);
            restart.setTitle("Restart to apply");
            restart.setSummary("The toolbar takes the new layout when the browser starts");
            restart.setOnPreferenceClickListener(
                    p -> {
                        ApplicationLifetime.terminate(/* restart= */ true);
                        return true;
                    });
            screen.addPreference(restart);
        }

        ChromeSwitchPreference hideOnScroll = new ChromeSwitchPreference(context);
        hideOnScroll.setTitle("Hide toolbar on scroll");
        hideOnScroll.setSummary("Takes effect on the next page load");
        hideOnScroll.setChecked(prefs().getBoolean(HIDE_ON_SCROLL_PREF, false));
        hideOnScroll.setOnPreferenceChangeListener(
                (pref, value) -> {
                    prefs().edit().putBoolean(HIDE_ON_SCROLL_PREF, (Boolean) value).apply();
                    return true;
                });
        screen.addPreference(hideOnScroll);

        Preference about = new Preference(context);
        about.setSummary(
                "Left to right. Items before the address bar sit on its left. Tap an item to move"
                        + " or hide it. Changes apply when the browser restarts.");
        about.setSelectable(false);
        screen.addPreference(about);

        for (int i = 0; i < items.size(); i++) {
            String token = items.get(i);
            boolean hidden = token.startsWith("-");
            String name = hidden ? token.substring(1) : token;
            Preference row = new Preference(context);
            row.setTitle(label(name));
            row.setSummary(hidden ? "Hidden" : null);
            int index = i;
            row.setOnPreferenceClickListener(
                    p -> {
                        showActions(items, index);
                        return true;
                    });
            screen.addPreference(row);
        }

        Preference reset = new Preference(context);
        reset.setTitle("Reset to default");
        reset.setOnPreferenceClickListener(
                p -> {
                    prefs().edit().remove(PREF).apply();
                    render();
                    return true;
                });
        screen.addPreference(reset);

    }

    private void showActions(List<String> items, int index) {
        String token = items.get(index);
        boolean hidden = token.startsWith("-");
        String name = hidden ? token.substring(1) : token;
        List<String> labels = new ArrayList<>();
        List<Runnable> actions = new ArrayList<>();
        if (index > 0) {
            labels.add("Move left");
            actions.add(() -> swap(items, index, index - 1));
        }
        if (index < items.size() - 1) {
            labels.add("Move right");
            actions.add(() -> swap(items, index, index + 1));
        }
        if (!name.equals("location") && !name.equals("menu")) {
            labels.add(hidden ? "Show" : "Hide");
            actions.add(
                    () -> {
                        items.set(index, hidden ? name : "-" + name);
                        save(items);
                    });
        }
        new AlertDialog.Builder(getPreferenceManager().getContext())
                .setTitle(label(name))
                .setItems(labels.toArray(new String[0]), (dialog, which) -> actions.get(which).run())
                .show();
    }

    private void swap(List<String> items, int a, int b) {
        String item = items.get(a);
        items.set(a, items.get(b));
        items.set(b, item);
        save(items);
    }
}
