// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.app_links;

import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Bundle;
import android.text.TextUtils;

import androidx.preference.Preference;
import androidx.preference.PreferenceGroup;
import androidx.preference.PreferenceScreen;

import org.chromium.base.ContextUtils;
import org.chromium.base.IntentUtils;
import org.chromium.base.supplier.MonotonicObservableSupplier;
import org.chromium.base.supplier.ObservableSuppliers;
import org.chromium.base.supplier.SettableMonotonicObservableSupplier;
import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.browserservices.intents.WebappConstants;
import org.chromium.chrome.browser.settings.ChromeBaseSettingsFragment;
import org.chromium.chrome.browser.webapps.WebappDataStorage;
import org.chromium.chrome.browser.webapps.WebappRegistry;
import org.chromium.components.browser_ui.settings.TextMessagePreference;
import org.chromium.url.GURL;

import java.util.ArrayList;
import java.util.List;

/**
 * Settings → Web apps: the installed web apps (WebAPKs and home screen shortcuts), each opening
 * {@link WebAppLinksSettings}. A site's page in Site settings links its apps there too.
 */
@NullMarked
public class WebAppsSettings extends ChromeBaseSettingsFragment {
    private final SettableMonotonicObservableSupplier<String> mPageTitle =
            ObservableSuppliers.createMonotonic();

    @Override
    public void onCreatePreferences(@Nullable Bundle savedInstanceState, @Nullable String rootKey) {
        mPageTitle.set(getString(R.string.web_apps_title));
        PreferenceScreen screen = getPreferenceManager().createPreferenceScreen(getStyledContext());
        setPreferenceScreen(screen);
        List<WebappDataStorage> apps = installedApps(null);
        if (apps.isEmpty()) {
            TextMessagePreference none = new TextMessagePreference(getStyledContext(), null);
            none.setSummary(
                    "No installed web apps. Install one from a site's menu: Add to home screen or"
                            + " Install app.");
            screen.addPreference(none);
            return;
        }
        for (WebappDataStorage app : apps) addEntry(screen, getStyledContext(), app);
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

    /**
     * For a site's page in Site settings: an entry per web app installed from {@code origin}
     * (scheme://host[:port]), added to {@code group}.
     */
    public static void addSiteEntries(PreferenceGroup group, Context context, String origin) {
        for (WebappDataStorage app : installedApps(origin)) addEntry(group, context, app);
    }

    /** The installed apps, sorted by name; only those of {@code origin} unless it is null. */
    private static List<WebappDataStorage> installedApps(@Nullable String origin) {
        WebappRegistry.warmUpSharedPrefs();
        WebappRegistry registry = WebappRegistry.getInstance();
        List<WebappDataStorage> apps = new ArrayList<>();
        for (String id : registry.getRegisteredWebappIds()) {
            WebappDataStorage app = registry.getWebappDataStorage(id);
            if (app == null || TextUtils.isEmpty(app.getScope())) continue;
            if (origin != null
                    && !new GURL(app.getScope()).getOrigin().getSpec().equals(origin + "/")) {
                continue;
            }
            apps.add(app);
        }
        Context context = ContextUtils.getApplicationContext();
        apps.sort((a, b) -> name(context, a).compareToIgnoreCase(name(context, b)));
        return apps;
    }

    private static void addEntry(PreferenceGroup group, Context context, WebappDataStorage app) {
        Preference entry = new Preference(context);
        entry.setTitle(name(context, app));
        entry.setSummary(app.getScope());
        entry.setFragment(WebAppLinksSettings.class.getName());
        entry.getExtras().putString(WebAppLinksSettings.ARG_APP_ID, app.getId());
        group.addPreference(entry);
    }

    /** The app's name: a WebAPK's label, a shortcut's name, or its scope. */
    static String name(Context context, WebappDataStorage app) {
        String webApkPackage = app.getWebApkPackageName();
        if (webApkPackage != null) {
            PackageManager pm = context.getPackageManager();
            try {
                return pm.getApplicationLabel(pm.getApplicationInfo(webApkPackage, 0)).toString();
            } catch (PackageManager.NameNotFoundException e) {
                // Uninstalled, the registry not yet told.
            }
        }
        Intent intent = app.createWebappLaunchIntent();
        String name =
                intent != null ? IntentUtils.safeGetStringExtra(intent, WebappConstants.EXTRA_NAME)
                        : null;
        return !TextUtils.isEmpty(name) ? name : Uri.parse(app.getScope()).getHost() + "";
    }
}
