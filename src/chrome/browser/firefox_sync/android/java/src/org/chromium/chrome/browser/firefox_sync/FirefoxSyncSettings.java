// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.firefox_sync;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.text.InputType;
import android.text.format.DateUtils;
import android.widget.EditText;

import androidx.appcompat.app.AlertDialog;
import androidx.preference.Preference;
import androidx.preference.PreferenceCategory;
import androidx.preference.PreferenceScreen;

import org.json.JSONArray;
import org.json.JSONObject;

import org.chromium.base.supplier.MonotonicObservableSupplier;
import org.chromium.base.supplier.ObservableSuppliers;
import org.chromium.base.supplier.SettableMonotonicObservableSupplier;
import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.settings.ChromeBaseSettingsFragment;
import org.chromium.components.browser_ui.settings.ChromeSwitchPreference;

/**
 * Settings → Firefox Sync: the account, the sync server, how often to check for tabs other devices
 * sent, a switch per data type, and the tabs of other Firefox devices. Mirrors oganesson's settings
 * section; the screen is rebuilt from the service's status whenever it changes.
 */
@NullMarked
public class FirefoxSyncSettings extends ChromeBaseSettingsFragment {
    private static final String MOZILLA_SERVER = "https://accounts.firefox.com";

    private final SettableMonotonicObservableSupplier<String> mPageTitle =
            ObservableSuppliers.createMonotonic();
    private @Nullable FirefoxSyncBridge mBridge;
    // Sign in was tapped: leave for the sign-in tab once it opens, stay to show an error.
    private boolean mSignInPending;

    @Override
    public void onCreatePreferences(@Nullable Bundle savedInstanceState, @Nullable String rootKey) {
        mPageTitle.set(getString(R.string.firefox_sync_title));
        setPreferenceScreen(getPreferenceManager().createPreferenceScreen(getStyledContext()));
        mBridge = new FirefoxSyncBridge(getProfile(), this::render);
        render();
    }

    @Override
    public void onDestroy() {
        if (mBridge != null) {
            mBridge.destroy();
            mBridge = null;
        }
        super.onDestroy();
    }

    @Override
    public MonotonicObservableSupplier<String> getPageTitle() {
        return mPageTitle;
    }

    @Override
    public @AnimationType int getAnimationType() {
        return AnimationType.PROPERTY;
    }

    private void render() {
        FirefoxSyncBridge bridge = mBridge;
        if (bridge == null) return;
        JSONObject status = bridge.getStatus();
        Context context = getStyledContext();
        PreferenceScreen screen = getPreferenceScreen();
        screen.removeAll();

        boolean signedIn = status.optBoolean("signedIn");
        String error = status.optString("error");
        if (mSignInPending && status.optBoolean("signingIn")) {
            mSignInPending = false;
            // The sign-in page opened in a new tab in the browser, behind Settings.
            getActivity().finish();
            return;
        }
        if (!error.isEmpty()) mSignInPending = false;

        Preference account = new Preference(context);
        account.setTitle(signedIn ? status.optString("email") : "Not signed in");
        account.setSummary(error.isEmpty() ? syncSummary(status) : error);
        account.setSelectable(false);
        screen.addPreference(account);

        if (signedIn) {
            screen.addPreference(button(context, "Sync now", bridge::syncNow));
            screen.addPreference(
                    receiveInterval(context, bridge, status.optInt("receiveInterval")));
            screen.addPreference(
                    button(context, "Sign out", bridge::signOut));
        } else {
            String server = status.optString("server");
            String syncServer = status.optString("syncServer");
            // Not EditTextPreferences: their dialog crashes in Chromium's settings.
            Preference serverPref =
                    button(
                            context,
                            "Account server",
                            () -> editServer(context, bridge, "Account server", server, false));
            serverPref.setSummary(server.isEmpty() ? "Not set" : server);
            screen.addPreference(serverPref);
            // A self-hosted syncstorage-rs, as Fenix's "Custom Sync server".
            Preference syncServerPref =
                    button(
                            context,
                            "Sync server",
                            () -> editServer(context, bridge, "Sync server", syncServer, true));
            syncServerPref.setSummary(
                    syncServer.isEmpty() ? "The account server's" : syncServer);
            screen.addPreference(syncServerPref);
            screen.addPreference(
                    button(
                            context,
                            "Use Mozilla's servers",
                            () -> {
                                bridge.setServer("", true);
                                bridge.setServer(MOZILLA_SERVER, false);
                            }));
            Preference signIn =
                    button(
                            context,
                            "Sign in",
                            () -> {
                                mSignInPending = true;
                                bridge.signIn();
                            });
            signIn.setEnabled(status.optBoolean("canSignIn"));
            screen.addPreference(signIn);
        }

        PreferenceCategory types = new PreferenceCategory(context);
        types.setTitle("Sync");
        screen.addPreference(types);
        JSONArray typeList = status.optJSONArray("types");
        for (int i = 0; typeList != null && i < typeList.length(); i++) {
            JSONObject type = typeList.optJSONObject(i);
            if (type == null || !type.optBoolean("supported")) continue;
            String name = type.optString("name");
            ChromeSwitchPreference toggle = new ChromeSwitchPreference(context);
            toggle.setTitle(type.optString("label"));
            String typeError = type.optString("error");
            if (!typeError.isEmpty()) {
                toggle.setSummary(typeError);
            } else if (type.has("count")) {
                toggle.setSummary(type.optInt("count") + " synced");
            }
            toggle.setChecked(type.optBoolean("enabled"));
            toggle.setOnPreferenceChangeListener(
                    (pref, value) -> {
                        bridge.setTypeEnabled(name, (Boolean) value);
                        return true;
                    });
            types.addPreference(toggle);
        }

        JSONArray devices = status.optJSONArray("remoteTabs");
        for (int i = 0; devices != null && i < devices.length(); i++) {
            JSONObject device = devices.optJSONObject(i);
            if (device == null) continue;
            PreferenceCategory category = new PreferenceCategory(context);
            category.setTitle("Tabs from " + device.optString("name"));
            screen.addPreference(category);
            JSONArray tabs = device.optJSONArray("tabs");
            for (int j = 0; tabs != null && j < tabs.length(); j++) {
                JSONObject tab = tabs.optJSONObject(j);
                if (tab == null) continue;
                String url = tab.optString("url");
                Preference link = button(context, tab.optString("title", url), () -> open(url));
                link.setSummary(url);
                category.addPreference(link);
            }
        }
    }

    private String syncSummary(JSONObject status) {
        if (status.optBoolean("syncing")) return "Syncing…";
        if (status.optBoolean("needsSignIn")) return "Sign in again to keep syncing";
        double lastSync = status.optDouble("lastSync");
        if (lastSync > 0) {
            return "Last synced "
                    + DateUtils.getRelativeTimeSpanString(
                            (long) lastSync, System.currentTimeMillis(), DateUtils.MINUTE_IN_MILLIS);
        }
        return status.optBoolean("signedIn") ? "Not synced yet" : "";
    }

    /** The status change re-renders the summary. */
    private static void editServer(
            Context context,
            FirefoxSyncBridge bridge,
            String title,
            String server,
            boolean syncStorage) {
        EditText input = new EditText(context);
        input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        input.setText(server);
        new AlertDialog.Builder(context)
                .setTitle(title)
                .setView(input)
                .setPositiveButton(
                        android.R.string.ok,
                        (dialog, which) ->
                                bridge.setServer(input.getText().toString().trim(), syncStorage))
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    /** How often to check for the tabs other devices sent, as oganesson's dropdown has it. */
    private static Preference receiveInterval(
            Context context, FirefoxSyncBridge bridge, int current) {
        JSONObject setting = FirefoxSyncBridge.getReceiveIntervals();
        String title = setting.optString("title");
        JSONArray options = setting.optJSONArray("options");
        if (options == null) options = new JSONArray();
        int count = options.length();
        String[] labels = new String[count];
        int[] minutes = new int[count];
        int checked = -1;
        for (int i = 0; i < count; i++) {
            JSONObject option = options.optJSONObject(i);
            labels[i] = option.optString("label");
            minutes[i] = option.optInt("minutes");
            if (minutes[i] == current) checked = i;
        }
        int checkedItem = checked;
        Preference pref =
                button(
                        context,
                        title,
                        () ->
                                new AlertDialog.Builder(context)
                                        .setTitle(title)
                                        .setSingleChoiceItems(
                                                labels,
                                                checkedItem,
                                                (dialog, which) -> {
                                                    dialog.dismiss();
                                                    bridge.setReceiveInterval(minutes[which]);
                                                    ReceiveTabsTask.schedule(minutes[which], true);
                                                })
                                        .setNegativeButton(android.R.string.cancel, null)
                                        .show());
        if (checked >= 0) pref.setSummary(labels[checked]);
        return pref;
    }

    private Context getStyledContext() {
        return getPreferenceManager().getContext();
    }

    private static Preference button(Context context, String title, Runnable action) {
        Preference pref = new Preference(context);
        pref.setTitle(title);
        pref.setOnPreferenceClickListener(
                p -> {
                    action.run();
                    return true;
                });
        return pref;
    }

    /** Opens a tab from another device in this browser. */
    private void open(String url) {
        Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
        intent.setPackage(getContext().getPackageName());
        startActivity(intent);
    }
}
