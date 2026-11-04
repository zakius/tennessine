// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.app_links;

import android.app.Activity;

import org.jni_zero.CalledByNative;
import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;

import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.browserservices.intents.BrowserServicesIntentDataProvider;
import org.chromium.chrome.browser.browserservices.intents.WebappExtras;
import org.chromium.chrome.browser.customtabs.BaseCustomTabActivity;
import org.chromium.chrome.browser.profiles.Profile;
import org.chromium.chrome.browser.profiles.ProfileManager;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tab.TabUtils;
import org.chromium.components.embedder_support.util.UrlUtilities;
import org.chromium.content_public.browser.WebContents;

/**
 * dubna's links out of installed web apps (chrome/browser/app_links/app_links_platform.h) on
 * Android: which web app a page is in, its scope, and the default browser; and the user's scope
 * rules (app_scope_rules.h) for the settings screens and the web app's out-of-app bar. An app's id
 * is its WebappDataStorage id, as WebappExtras.id has it.
 */
@NullMarked
public final class AppLinksBridge {
    private AppLinksBridge() {}

    /** The web app shown by the activity of {@code webContents}, or null outside one. */
    private static @Nullable WebappExtras webappExtras(WebContents webContents) {
        Tab tab = TabUtils.fromWebContents(webContents);
        Activity activity = TabUtils.getActivity(tab);
        if (!(activity instanceof BaseCustomTabActivity customTab)) return null;
        BrowserServicesIntentDataProvider provider = customTab.getIntentDataProvider();
        return provider.isWebappOrWebApkActivity() ? provider.getWebappExtras() : null;
    }

    /**
     * Whether {@code url} is in the web app's scope as Chromium decides it, before the user's
     * rules: a WebAPK's manifest scope, or a home screen shortcut's site (WebApkVerifier,
     * AddToHomescreenVerifier).
     */
    public static boolean isUrlInAppScope(WebappExtras extras, boolean isWebApk, String url) {
        return isWebApk
                ? UrlUtilities.isUrlWithinScope(url, extras.scopeUrl)
                : UrlUtilities.sameDomainOrHost(extras.url, url, true);
    }

    /**
     * Whether {@code url} is in the scope of the app {@code appId} with the user's rules, given
     * {@code inScope}, Chromium's answer for it. For the out-of-app bar (WebappVerifier).
     */
    public static boolean isUrlInScope(String appId, String url, boolean inScope) {
        // Before the native library: Chromium's answer, as the rules live in native prefs.
        if (!ProfileManager.isInitialized()) return inScope;
        int override =
                AppLinksBridgeJni.get()
                        .scopeOverride(ProfileManager.getLastUsedRegularProfile(), appId, url);
        return override < 0 ? inScope : override == 1;
    }

    @CalledByNative
    private static @JniType("std::optional<std::string>") @Nullable String getAppId(
            WebContents webContents) {
        WebappExtras extras = webappExtras(webContents);
        return extras != null ? extras.id : null;
    }

    @CalledByNative
    private static boolean isUrlInScope(
            WebContents webContents, @JniType("std::string") String url) {
        Tab tab = TabUtils.fromWebContents(webContents);
        WebappExtras extras = webappExtras(webContents);
        if (extras == null) return false;
        BaseCustomTabActivity activity = (BaseCustomTabActivity) TabUtils.getActivity(tab);
        return isUrlInAppScope(
                extras, activity.getIntentDataProvider().isWebApkActivity(), url);
    }

    @CalledByNative
    private static void openInDefaultBrowser(
            WebContents webContents, @JniType("std::string") String url, boolean background) {
        Activity activity = TabUtils.getActivity(TabUtils.fromWebContents(webContents));
        if (activity != null) DefaultBrowser.open(activity, url, background);
    }

    /** The app's scope rules, one per line. */
    public static String getRules(Profile profile, String appId) {
        return AppLinksBridgeJni.get().getRules(profile, appId);
    }

    /** Sets the app's rules; returns why one is invalid, keeping the old ones, or "". */
    public static String setRules(Profile profile, String appId, String rules) {
        return AppLinksBridgeJni.get().setRules(profile, appId, rules);
    }

    public static boolean isStrict(Profile profile, String appId) {
        return AppLinksBridgeJni.get().isStrict(profile, appId);
    }

    public static void setStrict(Profile profile, String appId, boolean strict) {
        AppLinksBridgeJni.get().setStrict(profile, appId, strict);
    }

    @NativeMethods
    interface Natives {
        @JniType("std::string")
        String getRules(@JniType("Profile*") Profile profile, @JniType("std::string") String appId);

        @JniType("std::string")
        String setRules(
                @JniType("Profile*") Profile profile,
                @JniType("std::string") String appId,
                @JniType("std::string") String rules);

        boolean isStrict(@JniType("Profile*") Profile profile, @JniType("std::string") String appId);

        void setStrict(
                @JniType("Profile*") Profile profile,
                @JniType("std::string") String appId,
                boolean strict);

        // -1 where Chromium's scope decides, 0 out of the scope, 1 in it.
        int scopeOverride(
                @JniType("Profile*") Profile profile,
                @JniType("std::string") String appId,
                @JniType("std::string") String url);
    }
}
