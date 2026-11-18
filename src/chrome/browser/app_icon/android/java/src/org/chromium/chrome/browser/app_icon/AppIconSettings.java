// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.app_icon;

import android.content.Context;
import android.graphics.drawable.Drawable;
import android.os.Bundle;

import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.content.res.AppCompatResources;
import androidx.preference.Preference;
import androidx.preference.PreferenceScreen;

import org.chromium.base.supplier.MonotonicObservableSupplier;
import org.chromium.base.supplier.ObservableSuppliers;
import org.chromium.base.supplier.SettableMonotonicObservableSupplier;
import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.lifetime.ApplicationLifetime;
import org.chromium.chrome.browser.settings.ChromeBaseSettingsFragment;
import org.chromium.components.browser_ui.styles.SemanticColorUtils;

/** Settings → App icon: which of the icons in {@link AppIcons} the launcher shows. */
@NullMarked
public class AppIconSettings extends ChromeBaseSettingsFragment {
    // In the order of AppIcons.NAMES.
    private static final String[] LABELS = {
        "Indigo", "Teal", "Crimson", "Black", "White", "No background, white", "No background, black",
    };

    private final SettableMonotonicObservableSupplier<String> mPageTitle =
            ObservableSuppliers.createMonotonic();

    @Override
    public void onCreatePreferences(@Nullable Bundle savedInstanceState, @Nullable String rootKey) {
        mPageTitle.set(getString(R.string.app_icon_title));
        Context context = getPreferenceManager().getContext();
        PreferenceScreen screen = getPreferenceManager().createPreferenceScreen(context);
        setPreferenceScreen(screen);
        String current = AppIcons.current(context);

        for (int i = 0; i < AppIcons.NAMES.size(); i++) {
            String name = AppIcons.NAMES.get(i);
            String label = LABELS[i];
            Preference option = new Preference(context);
            option.setTitle(label);
            Drawable icon = AppCompatResources.getDrawable(context, AppIcons.ICONS[i]);
            // A glyph alone could vanish against this screen: shown in the icon colour.
            if (name.startsWith("Clear") && icon != null) {
                icon = icon.mutate();
                icon.setTint(SemanticColorUtils.getDefaultIconColor(context));
            }
            option.setIcon(icon);
            if (name.equals(current)) {
                option.setSummary("In use");
            } else {
                option.setOnPreferenceClickListener(
                        p -> {
                            confirm(name, label);
                            return true;
                        });
            }
            screen.addPreference(option);
        }

        Preference note = new Preference(context);
        note.setSummary(
                "Black, White and No background are 1-bit. No background is the icon alone:"
                        + " launchers that put every icon on a plate, like Pixel's, show it on a"
                        + " white one. With themed icons on, Android draws the icon in your"
                        + " wallpaper's colours instead.");
        note.setSelectable(false);
        screen.addPreference(note);
    }

    private void confirm(String name, String label) {
        new AlertDialog.Builder(getPreferenceManager().getContext())
                .setTitle("Use the " + label + " icon?")
                .setMessage(
                        "tennessine restarts. Your launcher may take the icon off the home screen;"
                                + " add it back from the app list.")
                .setNegativeButton(android.R.string.cancel, null)
                .setPositiveButton(
                        "Restart",
                        (dialog, which) -> {
                            AppIcons.select(getPreferenceManager().getContext(), name);
                            ApplicationLifetime.terminate(/* restart= */ true);
                        })
                .show();
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
