// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.firefox_sync;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.provider.Browser;

import androidx.appcompat.app.AlertDialog;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import org.jni_zero.CalledByNative;
import org.jni_zero.JNINamespace;
import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;

import org.chromium.base.ContextUtils;
import org.chromium.base.IntentUtils;
import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.browserservices.intents.WebappConstants;
import org.chromium.chrome.browser.notifications.NotificationUmaTracker;
import org.chromium.chrome.browser.notifications.NotificationWrapperBuilderFactory;
import org.chromium.chrome.browser.notifications.channels.ChromeChannelDefinitions;
import org.chromium.chrome.browser.profiles.Profile;
import org.chromium.components.browser_ui.notifications.BaseNotificationManagerProxyFactory;
import org.chromium.components.browser_ui.notifications.NotificationMetadata;
import org.chromium.components.browser_ui.notifications.PendingIntentProvider;
import org.chromium.ui.widget.Toast;

import java.util.ArrayList;
import java.util.List;

/**
 * Tabs to and from the account's other devices (android_platform.cc): "Send to your device" in
 * the share sheet and a link's menu, and the notification for a tab another device sent.
 */
@JNINamespace("firefox_sync")
@NullMarked
public class SendTab {
    private static final String NOTIFICATION_TAG = "firefox_sync_tab:";
    // The launcher activity is in chrome_java, which this target can't depend on.
    private static final String LAUNCHER_ACTIVITY =
            "org.chromium.chrome.browser.document.ChromeLauncherActivity";

    /** Whether {@code url} is a web page and the account has a device that takes tabs. */
    public static boolean canSend(Profile profile, @Nullable String url) {
        return url != null
                && (url.startsWith("http://") || url.startsWith("https://"))
                && !targets(profile).isEmpty();
    }

    /** Asks which device to send {@code url} to, and sends it. */
    public static void showDevicePicker(
            Context context, Profile profile, String url, @Nullable String title) {
        List<JSONObject> devices = targets(profile);
        String[] names = new String[devices.size()];
        for (int i = 0; i < names.length; i++) names[i] = devices.get(i).optString("name");
        new AlertDialog.Builder(context)
                .setTitle(R.string.send_tab_to_self_enhanced_title)
                .setItems(
                        names,
                        (dialog, which) -> {
                            SendTabJni.get()
                                    .sendTab(
                                            profile,
                                            devices.get(which).optString("id"),
                                            url,
                                            title == null || title.isEmpty() ? url : title);
                            Toast.makeText(
                                            context,
                                            context.getString(
                                                    R.string.firefox_sync_tab_sent, names[which]),
                                            Toast.LENGTH_SHORT)
                                    .show();
                        })
                .show();
    }

    private static List<JSONObject> targets(Profile profile) {
        List<JSONObject> targets = new ArrayList<>();
        try {
            JSONArray devices = new JSONArray(SendTabJni.get().getDevices(profile));
            for (int i = 0; i < devices.length(); i++) {
                JSONObject device = devices.getJSONObject(i);
                if (device.optBoolean("sendTab")) targets.add(device);
            }
        } catch (JSONException e) {
            // No devices to offer.
        }
        return targets;
    }

    /**
     * "Tab from <device>: <title>" for a tab another device sent, which the native side opened
     * in the background. A tap switches to the tab with that URL, or opens it again if it was
     * closed or no window was open.
     */
    @CalledByNative
    private static void showReceivedTab(
            @JniType("std::string") String from,
            @JniType("std::string") String url,
            @JniType("std::string") String title) {
        Context context = ContextUtils.getApplicationContext();
        Intent intent =
                new Intent(Intent.ACTION_VIEW, Uri.parse(url))
                        .setClassName(context, LAUNCHER_ACTIVITY)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                        .putExtra(Browser.EXTRA_APPLICATION_ID, context.getPackageName())
                        .putExtra(WebappConstants.REUSE_URL_MATCHING_TAB_ELSE_NEW_TAB, true);
        IntentUtils.addTrustedIntentExtras(intent);
        // The URL in the tag: the same tab sent again replaces its notification.
        NotificationMetadata metadata =
                new NotificationMetadata(
                        NotificationUmaTracker.SystemNotificationType.SEND_TAB_TO_SELF,
                        NOTIFICATION_TAG + url,
                        0);
        BaseNotificationManagerProxyFactory.create()
                .notify(
                        NotificationWrapperBuilderFactory.createNotificationWrapperBuilder(
                                        ChromeChannelDefinitions.ChannelId.SHARING, metadata)
                                .setSmallIcon(R.drawable.ic_chrome)
                                .setContentTitle(
                                        context.getString(R.string.firefox_sync_tab_from, from, title))
                                .setContentText(url)
                                .setContentIntent(
                                        PendingIntentProvider.getActivity(context, 0, intent, 0))
                                .setAutoCancel(true)
                                .buildNotificationWrapper());
    }

    @NativeMethods
    interface Natives {
        /** FirefoxSyncService::devices() as JSON. */
        @JniType("std::string")
        String getDevices(@JniType("Profile*") Profile profile);

        void sendTab(
                @JniType("Profile*") Profile profile,
                @JniType("std::string") String deviceId,
                @JniType("std::string") String url,
                @JniType("std::string") String title);
    }
}
