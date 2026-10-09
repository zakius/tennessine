// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.crash_log;

import android.content.Context;
import android.content.Intent;
import android.os.Bundle;

import androidx.preference.Preference;
import androidx.preference.PreferenceScreen;

import org.chromium.base.ContextUtils;
import org.chromium.base.supplier.MonotonicObservableSupplier;
import org.chromium.base.supplier.ObservableSuppliers;
import org.chromium.base.supplier.SettableMonotonicObservableSupplier;
import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.settings.ChromeBaseSettingsFragment;

/** Settings → Last crash: the saved stack trace, with Share and Clear. */
@NullMarked
public class CrashLogSettings extends ChromeBaseSettingsFragment {
    private final SettableMonotonicObservableSupplier<String> mPageTitle =
            ObservableSuppliers.createMonotonic();

    @Override
    public void onCreatePreferences(@Nullable Bundle savedInstanceState, @Nullable String rootKey) {
        mPageTitle.set(getString(R.string.crash_log_title));
        render();
    }

    private void render() {
        Context context = getPreferenceManager().getContext();
        PreferenceScreen screen = getPreferenceManager().createPreferenceScreen(context);
        setPreferenceScreen(screen);
        String log = ContextUtils.getAppSharedPreferences().getString(CrashLog.PREF, "");

        Preference text = new Preference(context);
        text.setTitle(log.isEmpty() ? "No crash saved" : "Stack trace");
        text.setSummary(log.isEmpty() ? null : log);
        text.setSelectable(false);
        screen.addPreference(text);
        if (log.isEmpty()) return;

        Preference share = new Preference(context);
        share.setTitle("Share");
        share.setOnPreferenceClickListener(
                p -> {
                    Intent send = new Intent(Intent.ACTION_SEND);
                    send.setType("text/plain");
                    send.putExtra(Intent.EXTRA_TEXT, log);
                    startActivity(Intent.createChooser(send, null));
                    return true;
                });
        screen.addPreference(share);

        Preference clear = new Preference(context);
        clear.setTitle("Clear");
        clear.setOnPreferenceClickListener(
                p -> {
                    ContextUtils.getAppSharedPreferences().edit().remove(CrashLog.PREF).apply();
                    render();
                    return true;
                });
        screen.addPreference(clear);
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
