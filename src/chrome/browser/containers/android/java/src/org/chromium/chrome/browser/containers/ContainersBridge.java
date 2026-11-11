// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.containers;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;

import org.jni_zero.CalledByNative;
import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import org.chromium.base.ContextUtils;
import org.chromium.base.IntentUtils;
import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.app.tabwindow.TabWindowManagerSingleton;
import org.chromium.chrome.browser.document.ChromeLauncherActivity;
import org.chromium.chrome.browser.profiles.Profile;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tab.WebContentsState;
import org.chromium.chrome.browser.tabmodel.TabList;
import org.chromium.chrome.browser.tabmodel.TabModel;
import org.chromium.chrome.browser.tabmodel.TabModelSelector;
import org.chromium.chrome.browser.tabmodel.TabModelSelectorObserver;
import org.chromium.chrome.browser.tabwindow.TabWindowManager;
import org.chromium.content_public.browser.WebContents;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * dubna's containers (chrome/browser/containers) on Android: what containers_android.cc does
 * for Java, and what it asks of Java. The containers and Traffic Control's rules are prefs that
 * cross as JSON, as dubna's container.h and traffic_control.h define them.
 */
@NullMarked
public final class ContainersBridge {
    private ContainersBridge() {}

    /** A container of the user's list. {@code argb} is its color, for drawing. */
    public static final class Container {
        public final String id;
        public String name;
        public String icon;
        public String color;
        public final int argb;

        Container(String id, String name, String icon, String color, int argb) {
            this.id = id;
            this.name = name;
            this.icon = icon;
            this.color = color;
            this.argb = argb;
        }
    }

    /** A Traffic Control rule: URL patterns, one per line, and the container they open in. */
    public static final class Rule {
        public String urlFilter;
        public String container;
        public boolean enabled;

        Rule(String urlFilter, String container, boolean enabled) {
            this.urlFilter = urlFilter;
            this.container = container;
            this.enabled = enabled;
        }
    }

    /** A rule's container that is a new temporary container each time (traffic_control.h). */
    public static final String TEMPORARY = "temporary";

    public static List<Container> getContainers(Profile profile) {
        List<Container> containers = new ArrayList<>();
        try {
            JSONArray list = new JSONArray(ContainersBridgeJni.get().getContainers(profile));
            for (int i = 0; i < list.length(); i++) {
                JSONObject item = list.getJSONObject(i);
                containers.add(
                        new Container(
                                item.getString("id"),
                                item.optString("name"),
                                item.optString("icon"),
                                item.optString("color"),
                                item.getInt("argb")));
            }
        } catch (JSONException e) {
            // The native side writes the JSON: an error leaves the list empty.
        }
        return containers;
    }

    /** Sets the user's list; a container left out is deleted, and its tabs close. */
    public static void setContainers(Profile profile, List<Container> containers) {
        JSONArray list = new JSONArray();
        try {
            for (Container container : containers) {
                list.put(
                        new JSONObject()
                                .put("id", container.id)
                                .put("name", container.name)
                                .put("icon", container.icon)
                                .put("color", container.color));
            }
        } catch (JSONException e) {
            return;
        }
        ContainersBridgeJni.get().setContainers(profile, list.toString());
    }

    public static List<Rule> getRules(Profile profile) {
        List<Rule> rules = new ArrayList<>();
        try {
            JSONArray list = new JSONArray(ContainersBridgeJni.get().getRules(profile));
            for (int i = 0; i < list.length(); i++) {
                JSONObject item = list.getJSONObject(i);
                rules.add(
                        new Rule(
                                item.optString("url_filter"),
                                item.optString("container"),
                                item.optBoolean("enabled", true)));
            }
        } catch (JSONException e) {
            // As in getContainers().
        }
        return rules;
    }

    /** Sets Traffic Control's rules; returns why one isn't valid, keeping the old ones, or "". */
    public static String setRules(Profile profile, List<Rule> rules) {
        JSONArray list = new JSONArray();
        try {
            for (Rule rule : rules) {
                list.put(
                        new JSONObject()
                                .put("url_filter", rule.urlFilter)
                                .put("container", rule.container)
                                .put("enabled", rule.enabled));
            }
        } catch (JSONException e) {
            return "Not a valid rule";
        }
        return ContainersBridgeJni.get().setRules(profile, list.toString());
    }

    /** Where new tabs open: a container's id, {@link #TEMPORARY}, or "" outside containers. */
    public static String getNewTabSetting(Profile profile) {
        return ContainersBridgeJni.get().getNewTabSetting(profile);
    }

    public static void setNewTabSetting(Profile profile, String value) {
        ContainersBridgeJni.get().setNewTabSetting(profile, value);
    }

    /** The color {@code color}, one of Firefox's color names, is drawn in. */
    public static int colorValue(String color) {
        return ContainersBridgeJni.get().colorValue(color);
    }

    /** The container of {@code webContents}, or "". */
    public static String containerOf(WebContents webContents) {
        return ContainersBridgeJni.get().containerOf(webContents);
    }

    /** The color of the container of {@code webContents}, or 0 outside containers. */
    public static int colorOf(WebContents webContents) {
        return ContainersBridgeJni.get().colorOf(webContents);
    }

    /** The container a new tab opens in now; a new temporary one when the setting says so. */
    static String newTabContainer(Profile profile) {
        return ContainersBridgeJni.get().newTabContainer(profile);
    }

    static String createTemporary(Profile profile) {
        return ContainersBridgeJni.get().createTemporary(profile);
    }

    static WebContents createWebContents(
            Profile profile, String id, boolean initiallyHidden, boolean initializeRenderer) {
        return ContainersBridgeJni.get()
                .createWebContents(profile, id, initiallyHidden, initializeRenderer);
    }

    /** Opens {@code url} in a new tab beside the one of {@code source}, in container {@code id}. */
    static void openInNewTab(WebContents source, String url, String id) {
        ContainersBridgeJni.get().openInNewTab(source, url, id, /* foreground= */ true);
    }

    /**
     * Calls back native {@code request} once every tab is loaded, with the containers of all of
     * them: the open windows' tabs, the tabs of windows that aren't open, which Chromium loads
     * without a window after startup, the archived tabs and the tabs waiting for Undo. Calls back
     * at once, with {@code restored} false, when no window is open, and so it does when a window
     * closes before its tabs are restored or a tab's pages are unknown.
     */
    @CalledByNative
    private static void whenTabsRestored(long request) {
        List<TabModelSelector> selectors = openSelectors();
        if (selectors.isEmpty()) {
            ContainersBridgeJni.get().onTabsRestored(request, false, new String[0]);
            return;
        }
        whenRestored(request, selectors, 0);
    }

    private static List<TabModelSelector> openSelectors() {
        List<TabModelSelector> selectors = new ArrayList<>();
        for (TabModelSelector selector :
                TabWindowManagerSingleton.getInstance().getAllTabModelSelectors()) {
            if (selector != null) selectors.add(selector);
        }
        return selectors;
    }

    /** Waits for {@code selectors} from {@code index} on, one after the other. */
    private static void whenRestored(long request, List<TabModelSelector> selectors, int index) {
        if (index == selectors.size()) {
            whenAllRestored(request);
            return;
        }
        TabModelSelector selector = selectors.get(index);
        if (selector.isTabStateInitialized()) {
            whenRestored(request, selectors, index + 1);
            return;
        }
        selector.addObserver(
                new TabModelSelectorObserver() {
                    @Override
                    public void onTabStateInitialized() {
                        selector.removeObserver(this);
                        whenRestored(request, selectors, index + 1);
                    }

                    @Override
                    public void onDestroyed() {
                        selector.removeObserver(this);
                        ContainersBridgeJni.get().onTabsRestored(request, false, new String[0]);
                    }
                });
    }

    /** Waits for the archived tabs and the tabs of the windows that aren't open. */
    private static void whenAllRestored(long request) {
        TabWindowManager manager = TabWindowManagerSingleton.getInstance();
        if (manager.isAllTabStateInitialized()) {
            String[] ids = tabContainers();
            ContainersBridgeJni.get()
                    .onTabsRestored(request, ids != null, ids != null ? ids : new String[0]);
            return;
        }
        manager.addObserver(
                new TabWindowManager.Observer() {
                    @Override
                    public void onAllTabModelStateInitialized() {
                        manager.removeObserver(this);
                        whenAllRestored(request);
                    }
                });
    }

    /**
     * The containers of every tab, those waiting for Undo included, or null when a tab that isn't
     * loaded has no saved pages to tell.
     */
    private static String @Nullable [] tabContainers() {
        List<TabModelSelector> selectors = openSelectors();
        TabModelSelector archived =
                TabWindowManagerSingleton.getInstance().getArchivedTabModelSelector();
        if (archived != null) selectors.add(archived);
        Set<String> ids = new HashSet<>();
        for (TabModelSelector selector : selectors) {
            TabModel model = selector.getModel(/* incognito= */ false);
            TabList tabs = model.getComprehensiveModel();
            for (int i = 0; i < tabs.getCount(); i++) {
                Tab tab = tabs.getTabAt(i);
                if (tab == null || tab.isDestroyed()) continue;
                WebContents contents = tab.getWebContents();
                if (contents != null) {
                    ids.add(containerOf(contents));
                    continue;
                }
                WebContentsState state = tab.getWebContentsState();
                if (state == null) return null;
                Collections.addAll(
                        ids,
                        ContainersBridgeJni.get()
                                .containersOfState(state.buffer(), state.version()));
            }
        }
        return ids.toArray(new String[0]);
    }

    /** Opens {@code url} in a new tab of the browser, out of a custom tab or a web app. */
    @CalledByNative
    private static void openInBrowser(@JniType("std::string") String url) {
        Context context = ContextUtils.getApplicationContext();
        Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
        intent.setClass(context, ChromeLauncherActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        IntentUtils.safeStartActivity(context, intent);
    }

    @NativeMethods
    interface Natives {
        WebContents createWebContents(
                @JniType("Profile*") Profile profile,
                @JniType("std::string") String id,
                boolean initiallyHidden,
                boolean initializeRenderer);

        @JniType("std::string")
        String containerOf(@JniType("content::WebContents*") WebContents webContents);

        int colorOf(@JniType("content::WebContents*") WebContents webContents);

        int colorValue(@JniType("std::string") String color);

        void openInNewTab(
                @JniType("content::WebContents*") WebContents source,
                @JniType("std::string") String url,
                @JniType("std::string") String id,
                boolean foreground);

        @JniType("std::string")
        String getContainers(@JniType("Profile*") Profile profile);

        boolean setContainers(
                @JniType("Profile*") Profile profile, @JniType("std::string") String json);

        @JniType("std::string")
        String getRules(@JniType("Profile*") Profile profile);

        @JniType("std::string")
        String setRules(@JniType("Profile*") Profile profile, @JniType("std::string") String json);

        @JniType("std::string")
        String getNewTabSetting(@JniType("Profile*") Profile profile);

        void setNewTabSetting(
                @JniType("Profile*") Profile profile, @JniType("std::string") String value);

        @JniType("std::string")
        String newTabContainer(@JniType("Profile*") Profile profile);

        @JniType("std::string")
        String createTemporary(@JniType("Profile*") Profile profile);

        void onTabsRestored(
                long request,
                boolean restored,
                @JniType("std::vector<std::string>") String[] tabContainers);

        @JniType("std::vector<std::string>")
        String[] containersOfState(ByteBuffer state, int version);
    }
}
