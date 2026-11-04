// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.app_links;

import android.content.Context;
import android.os.Bundle;
import android.text.InputType;
import android.widget.EditText;

import androidx.appcompat.app.AlertDialog;
import androidx.preference.Preference;
import androidx.preference.PreferenceScreen;

import org.chromium.base.supplier.MonotonicObservableSupplier;
import org.chromium.base.supplier.ObservableSuppliers;
import org.chromium.base.supplier.SettableMonotonicObservableSupplier;
import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.settings.ChromeBaseSettingsFragment;
import org.chromium.chrome.browser.webapps.WebappDataStorage;
import org.chromium.chrome.browser.webapps.WebappRegistry;
import org.chromium.components.browser_ui.settings.ChromeSwitchPreference;
import org.chromium.components.browser_ui.settings.TextMessagePreference;

/**
 * One installed web app's links: its scope rules, as on oganesson's app settings page ("Pages
 * that stay in the app"), and the strict scope switch. dubna's app_scope_rules.h keeps them.
 */
@NullMarked
public class WebAppLinksSettings extends ChromeBaseSettingsFragment {
    static final String ARG_APP_ID = "app_id";

    private final SettableMonotonicObservableSupplier<String> mPageTitle =
            ObservableSuppliers.createMonotonic();
    private String mAppId = "";

    @Override
    public void onCreatePreferences(@Nullable Bundle savedInstanceState, @Nullable String rootKey) {
        Bundle args = getArguments();
        mAppId = args != null ? args.getString(ARG_APP_ID, "") : "";
        Context context = getStyledContext();
        WebappRegistry.warmUpSharedPrefs();
        WebappDataStorage app = WebappRegistry.getInstance().getWebappDataStorage(mAppId);
        mPageTitle.set(app != null ? WebAppsSettings.name(context, app) : mAppId);

        PreferenceScreen screen = getPreferenceManager().createPreferenceScreen(context);
        setPreferenceScreen(screen);

        TextMessagePreference about = new TextMessagePreference(context, null);
        about.setSummary(
                "A link in the app that leads out of its scope"
                        + (app != null ? " (" + app.getScope() + ")" : "")
                        + " opens in your default browser. Redirects, sign-in pages a script"
                        + " loads, form posts and popups stay in the app.");
        screen.addPreference(about);

        Preference rules = new Preference(context);
        rules.setKey("rules");
        rules.setTitle("Pages that stay in the app");
        rules.setOnPreferenceClickListener(
                pref -> {
                    editRules(pref);
                    return true;
                });
        screen.addPreference(rules);
        showRules(rules);

        ChromeSwitchPreference strict = new ChromeSwitchPreference(context);
        strict.setTitle("Send every page outside the app to the default browser");
        strict.setSummary(
                "Also pages a script, a redirect or a popup leads to. Signing in within the app"
                        + " stops working where it goes through another site: sign in in the"
                        + " default browser, then open the app.");
        strict.setChecked(AppLinksBridge.isStrict(getProfile(), mAppId));
        strict.setOnPreferenceChangeListener(
                (pref, value) -> {
                    AppLinksBridge.setStrict(getProfile(), mAppId, (Boolean) value);
                    return true;
                });
        screen.addPreference(strict);
    }

    @Override
    public MonotonicObservableSupplier<String> getPageTitle() {
        return mPageTitle;
    }

    @Override
    public @AnimationType int getAnimationType() {
        return AnimationType.PROPERTY;
    }

    private Context getStyledContext() {
        return getPreferenceManager().getContext();
    }

    private void showRules(Preference pref) {
        String rules = AppLinksBridge.getRules(getProfile(), mAppId);
        pref.setSummary(rules.isEmpty() ? "None: the app's own scope" : rules);
    }

    /** A dialog with the rules, one per line; Save keeps it open on an invalid rule. */
    private void editRules(Preference pref) {
        Context context = getStyledContext();
        EditText field = new EditText(context);
        field.setInputType(
                InputType.TYPE_CLASS_TEXT
                        | InputType.TYPE_TEXT_FLAG_MULTI_LINE
                        | InputType.TYPE_TEXT_VARIATION_URI);
        field.setMinLines(3);
        field.setText(AppLinksBridge.getRules(getProfile(), mAppId));
        field.setHint("https://*.example.com/*\n!https://example.com/share/*");
        AlertDialog dialog =
                new AlertDialog.Builder(context)
                        .setTitle("Pages that stay in the app")
                        .setMessage(
                                "One URL pattern per line, such as https://*.example.com/*. They"
                                        + " add to the app's scope; on a site a pattern names"
                                        + " without a wildcard, they replace it. A pattern"
                                        + " starting with ! takes its pages out of the app, by any"
                                        + " route.")
                        .setView(field)
                        .setPositiveButton("Save", null)
                        .setNegativeButton(android.R.string.cancel, null)
                        .create();
        dialog.setOnShowListener(
                d ->
                        dialog.getButton(AlertDialog.BUTTON_POSITIVE)
                                .setOnClickListener(
                                        v -> {
                                            String error =
                                                    AppLinksBridge.setRules(
                                                            getProfile(),
                                                            mAppId,
                                                            field.getText().toString());
                                            if (!error.isEmpty()) {
                                                field.setError(error);
                                                return;
                                            }
                                            showRules(pref);
                                            dialog.dismiss();
                                        }));
        dialog.show();
    }
}
