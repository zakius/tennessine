// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.firefox_sync;

import android.app.BroadcastOptions;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Build;
import android.text.format.DateUtils;

import org.jni_zero.CalledByNative;
import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;

import org.chromium.base.ContextUtils;
import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.profiles.Profile;
import org.chromium.chrome.browser.profiles.ProfileManager;
import org.chromium.components.background_task_scheduler.BackgroundTaskSchedulerFactory;
import org.chromium.components.background_task_scheduler.TaskIds;
import org.chromium.components.background_task_scheduler.TaskInfo;

import java.util.UUID;

/**
 * Tabs other devices send, at once through Sunup, a UnifiedPush distributor (the AND_3 protocol,
 * https://unifiedpush.org/developers/spec/android/). Mozilla accounts take push endpoints only on
 * Mozilla's push servers, and Sunup's are. FirefoxSyncService registers the endpoint with the
 * account; a push only starts a fetch (FirefoxSyncService::OnPushReceived). Opt-in, offered only
 * while Sunup is installed. tennessine keeps no connection of its own, and the receive interval
 * still applies.
 */
@NullMarked
public class SunupPush {
    static final String SUNUP = "org.unifiedpush.distributor.sunup";
    static final String NEW_ENDPOINT = "org.unifiedpush.android.connector.NEW_ENDPOINT";
    static final String REGISTRATION_FAILED =
            "org.unifiedpush.android.connector.REGISTRATION_FAILED";
    static final String UNREGISTERED = "org.unifiedpush.android.connector.UNREGISTERED";
    static final String MESSAGE = "org.unifiedpush.android.connector.MESSAGE";
    private static final String REGISTER = "org.unifiedpush.android.distributor.REGISTER";
    private static final String UNREGISTER = "org.unifiedpush.android.distributor.UNREGISTER";
    private static final String MESSAGE_ACK = "org.unifiedpush.android.distributor.MESSAGE_ACK";

    // The switch, the token of the registration, its endpoint, and why it doesn't push, if so.
    static final String PREF_PREFIX = "tennessine.sunup_push";
    private static final String ENABLED_PREF = PREF_PREFIX;
    private static final String TOKEN_PREF = PREF_PREFIX + "_token";
    private static final String ENDPOINT_PREF = PREF_PREFIX + "_endpoint";
    private static final String PROBLEM_PREF = PREF_PREFIX + "_problem";

    /** Whether Sunup is installed: the switch shows only then. */
    public static boolean isSunupInstalled() {
        Context context = ContextUtils.getApplicationContext();
        Intent register = new Intent(REGISTER).setPackage(SUNUP);
        return !context.getPackageManager().queryBroadcastReceivers(register, 0).isEmpty();
    }

    public static boolean isEnabled() {
        return prefs().getBoolean(ENABLED_PREF, false);
    }

    /** What the switch's summary says: why tabs don't arrive at once, or null if they do. */
    public static @Nullable String getProblem() {
        return prefs().getString(PROBLEM_PREF, null);
    }

    public static void setEnabled(boolean enabled) {
        String token = prefs().getString(TOKEN_PREF, null);
        if (!enabled && token != null) {
            send(UNREGISTER, token);
        }
        prefs().edit()
                .putBoolean(ENABLED_PREF, enabled)
                .remove(TOKEN_PREF)
                .remove(ENDPOINT_PREF)
                .remove(PROBLEM_PREF)
                .apply();
        setServiceEndpoint("");
        if (enabled) register();
    }

    /**
     * At each start of the browser: registers again, as UnifiedPush asks, so a lost registration
     * comes back; Sunup answers a known token with the same endpoint. Returns the endpoint to
     * register with the account, if there is one.
     */
    @CalledByNative
    private static @JniType("std::string") String start() {
        if (!isEnabled() || !isSunupInstalled()) return "";
        register();
        return mozillaEndpoint(prefs().getString(ENDPOINT_PREF, ""));
    }

    private static void register() {
        String token = prefs().getString(TOKEN_PREF, null);
        if (token == null) {
            token = UUID.randomUUID().toString();
            prefs().edit().putString(TOKEN_PREF, token).apply();
        }
        send(REGISTER, token);
    }

    static void onNewEndpoint(String endpoint) {
        // Sunup answers each start's registration with the endpoint start() already handed over.
        // Another subscribe_push would race the start's first fetch, whose saved account it could
        // overwrite with an older one, so that the fetch after gets the same tabs again.
        if (endpoint.equals(prefs().getString(ENDPOINT_PREF, null))) return;
        String problem =
                mozillaEndpoint(endpoint).isEmpty()
                        ? "Sunup's push server isn't Mozilla's, and Mozilla accounts take no"
                                + " other. Tabs arrive when the browser checks for them."
                        : null;
        prefs().edit()
                .putString(ENDPOINT_PREF, endpoint)
                .putString(PROBLEM_PREF, problem)
                .apply();
        setServiceEndpoint(mozillaEndpoint(endpoint));
    }

    /** A new token for the next registration, as UnifiedPush asks. */
    static void onRegistrationFailed(@Nullable String reason) {
        if (prefs().contains(ENDPOINT_PREF)) return;
        prefs().edit()
                .remove(TOKEN_PREF)
                .putString(
                        PROBLEM_PREF,
                        "Sunup couldn't register"
                                + (reason == null ? "" : " (" + reason + ")")
                                + ". It tries again when the browser starts.")
                .apply();
    }

    /** Sunup dropped the registration, as when the user removed it there: the switch goes off. */
    static void onUnregistered() {
        prefs().edit()
                .putBoolean(ENABLED_PREF, false)
                .remove(TOKEN_PREF)
                .remove(ENDPOINT_PREF)
                .remove(PROBLEM_PREF)
                .apply();
        setServiceEndpoint("");
    }

    /**
     * A push from the accounts server: fetches the tabs at once in a running browser, else in a
     * background task that starts it.
     */
    static void onMessage() {
        if (ProfileManager.isInitialized()) {
            SunupPushJni.get().onPushReceived(ProfileManager.getLastUsedRegularProfile());
            return;
        }
        TaskInfo task =
                TaskInfo.createTask(
                                TaskIds.FIREFOX_SYNC_PUSH_JOB_ID,
                                TaskInfo.OneOffInfo.create()
                                        .setWindowEndTimeMs(DateUtils.MINUTE_IN_MILLIS)
                                        .build())
                        .setRequiredNetworkType(TaskInfo.NetworkType.ANY)
                        .setUpdateCurrent(true)
                        .build();
        BackgroundTaskSchedulerFactory.getScheduler()
                .schedule(ContextUtils.getApplicationContext(), task);
    }

    /** For ReceiveTabsTask's run after a push, with the browser started. */
    static void onPushReceived(Profile profile) {
        SunupPushJni.get().onPushReceived(profile);
    }

    static void acknowledge(String token, String id) {
        Intent ack =
                new Intent(MESSAGE_ACK)
                        .setPackage(SUNUP)
                        .putExtra("token", token)
                        .putExtra("id", id);
        ContextUtils.getApplicationContext().sendBroadcast(ack);
    }

    static boolean isCurrent(@Nullable String token) {
        return token != null && token.equals(prefs().getString(TOKEN_PREF, null));
    }

    /** `endpoint` if Mozilla accounts take it (fxa-auth-server's push.allowedServerRegex). */
    private static String mozillaEndpoint(String endpoint) {
        Uri uri = Uri.parse(endpoint);
        String host = uri.getHost();
        boolean mozilla =
                "https".equals(uri.getScheme())
                        && host != null
                        && host.endsWith(".services.mozilla.com");
        return mozilla ? endpoint : "";
    }

    // Sunup learns who asks from the shared identity (Android 14 and later) or, before, from the
    // creator of `pi`, an immutable PendingIntent.
    private static void send(String action, String token) {
        Context context = ContextUtils.getApplicationContext();
        Intent dummy = new Intent().setPackage("org.unifiedpush.dummy_app");
        Intent intent =
                new Intent(action)
                        .setPackage(SUNUP)
                        .putExtra("token", token)
                        .putExtra(
                                "pi",
                                PendingIntent.getBroadcast(
                                        context, 0, dummy, PendingIntent.FLAG_IMMUTABLE));
        if (action.equals(REGISTER)) intent.putExtra("message", "Tabs from your other devices");
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            BroadcastOptions options = BroadcastOptions.makeBasic();
            options.setShareIdentityEnabled(true);
            context.sendBroadcast(intent, null, options.toBundle());
        } else {
            context.sendBroadcast(intent);
        }
    }

    private static void setServiceEndpoint(String endpoint) {
        if (ProfileManager.isInitialized()) {
            SunupPushJni.get()
                    .setPushEndpoint(ProfileManager.getLastUsedRegularProfile(), endpoint);
        }
    }

    private static SharedPreferences prefs() {
        return ContextUtils.getAppSharedPreferences();
    }

    @NativeMethods
    interface Natives {
        void setPushEndpoint(
                @JniType("Profile*") Profile profile, @JniType("std::string") String endpoint);

        void onPushReceived(@JniType("Profile*") Profile profile);
    }
}
