// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.containers;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.LayerDrawable;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import androidx.annotation.StringRes;
import androidx.appcompat.app.AlertDialog;

import org.chromium.base.Callback;
import org.chromium.base.IntentUtils;
import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.IntentHandler;
import org.chromium.chrome.browser.app.tabwindow.TabWindowManagerSingleton;
import org.chromium.chrome.browser.content.WebContentsFactory;
import org.chromium.chrome.browser.profiles.Profile;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tab.TabLaunchType;
import org.chromium.chrome.browser.tabmodel.TabClosureParams;
import org.chromium.chrome.browser.tabmodel.TabModel;
import org.chromium.components.browser_ui.widget.ListItemBuilder;
import org.chromium.content_public.browser.WebContents;
import org.chromium.ui.base.WindowAndroid;
import org.chromium.ui.modelutil.MVCListAdapter.ListItem;

import java.util.ArrayList;
import java.util.List;

/**
 * Tabs in containers: the container a new tab starts in, the link menu's "Open in container", the
 * tab switcher button menu's "New tab in container", a tab menu's "Reopen in container", the
 * tab switcher's color line and the toolbar's icon. A tab stays in the container its WebContents is
 * created in (dubna's containers_tabs.h), so these only pick that container.
 */
@NullMarked
public final class Containers {
    private Containers() {}

    /** The link menu item's id: one no resource has (generated ids stay below 0x01000000). */
    public static final int MENU_ID = View.generateViewId();

    /** The tab switcher button menu's New tab in container. */
    public static final int NEW_TAB_MENU_ID = View.generateViewId();

    /** The tab switcher's tab menu's Reopen in container. */
    public static final int REOPEN_MENU_ID = View.generateViewId();

    /** The container New tab in container opens its tab in, while it opens it. */
    private static @Nullable String sNewTabContainer;

    private static final Object LINE_TAG = new Object();

    /**
     * The WebContents for {@code tab}, which TabImpl creates without one: in its opener's container
     * for a link, in the container Settings → Containers says for a new tab from the browser's UI,
     * else outside containers. WebContentsFactory.createWebContents() otherwise.
     */
    public static WebContents createWebContents(
            Tab tab, Profile profile, boolean initiallyHidden, boolean initializeRenderer) {
        String id = profile.isOffTheRecord() ? "" : containerFor(tab, profile);
        if (id.isEmpty()) {
            return WebContentsFactory.createWebContents(
                    profile, initiallyHidden, initializeRenderer);
        }
        return ContainersBridge.createWebContents(
                profile, id, initiallyHidden, initializeRenderer);
    }

    /**
     * Whether a new tab of launch type {@code type} from {@code parent}, or from the tab in another
     * window that {@code intent} names, goes into a container. Then it can't take the spare tab
     * custom tabs warm up, which isn't in one.
     */
    public static boolean goesInContainer(
            @Nullable Tab parent, @TabLaunchType int type, @Nullable Intent intent, Profile profile) {
        if (profile.isOffTheRecord()) return false;
        WebContents source = sourceInOtherWindow(intent, parent);
        String id =
                source != null
                        ? ContainersBridge.containerOf(source)
                        : containerFor(type, parent, profile);
        return !id.isEmpty();
    }

    /**
     * The WebContents for a new tab that a link in another window opens in this one, as Open in
     * new window and Open in other window do: {@code intent} names the link's tab, which this
     * window's tab model doesn't have, so the new tab has no {@code parent}. In the link's
     * container, or outside containers. Null for any other new tab.
     */
    public static @Nullable WebContents createForOtherWindow(
            @Nullable Intent intent, @Nullable Tab parent, Profile profile, boolean initiallyHidden) {
        WebContents source = sourceInOtherWindow(intent, parent);
        if (source == null || profile.isOffTheRecord()) return null;
        String id = ContainersBridge.containerOf(source);
        if (id.isEmpty()) {
            return WebContentsFactory.createWebContents(
                    profile, initiallyHidden, /* initializeRenderer= */ false);
        }
        return ContainersBridge.createWebContents(
                profile, id, initiallyHidden, /* initializeRenderer= */ false);
    }

    /** The page of the parent tab a trusted {@code intent} names, when it isn't {@code parent}. */
    private static @Nullable WebContents sourceInOtherWindow(
            @Nullable Intent intent, @Nullable Tab parent) {
        if (parent != null || !IntentHandler.wasIntentSenderChrome(intent)) return null;
        int id =
                IntentUtils.safeGetIntExtra(
                        intent, IntentHandler.EXTRA_PARENT_TAB_ID, Tab.INVALID_TAB_ID);
        if (id == Tab.INVALID_TAB_ID) return null;
        Tab source = TabWindowManagerSingleton.getInstance().getTabById(id);
        return source != null ? source.getWebContents() : null;
    }

    private static String containerFor(Tab tab, Profile profile) {
        Tab parent = TabWindowManagerSingleton.getInstance().getTabById(tab.getParentId());
        return containerFor(tab.getLaunchType(), parent, profile);
    }

    private static String containerFor(
            @TabLaunchType int type, @Nullable Tab parent, Profile profile) {
        switch (type) {
            case TabLaunchType.FROM_LINK:
            case TabLaunchType.FROM_LONGPRESS_FOREGROUND:
            case TabLaunchType.FROM_LONGPRESS_BACKGROUND:
            case TabLaunchType.FROM_LONGPRESS_FOREGROUND_IN_GROUP:
            case TabLaunchType.FROM_LONGPRESS_BACKGROUND_IN_GROUP:
            case TabLaunchType.FROM_LINK_CREATING_NEW_WINDOW:
            case TabLaunchType.FROM_HISTORY_NAVIGATION_FOREGROUND:
            case TabLaunchType.FROM_HISTORY_NAVIGATION_BACKGROUND:
                WebContents source = parent != null ? parent.getWebContents() : null;
                return source != null ? ContainersBridge.containerOf(source) : "";
            case TabLaunchType.FROM_CHROME_UI:
            case TabLaunchType.FROM_TAB_SWITCHER_UI:
                if (sNewTabContainer != null) return sNewTabContainer;
                return ContainersBridge.newTabContainer(profile);
            default:
                return "";
        }
    }

    /** Whether {@code tab}'s page is in a container. */
    public static boolean isInContainer(@Nullable Tab tab) {
        @Nullable WebContents contents = tab != null ? tab.getWebContents() : null;
        return contents != null && !ContainersBridge.containerOf(contents).isEmpty();
    }

    /** The link menu item's title. */
    public static CharSequence getMenuTitle(Context context) {
        return context.getString(R.string.contextmenu_open_in_container);
    }

    /**
     * The link menu item's chooser: the user's containers, a new temporary container, and no
     * container when {@code tab} is in one. The link opens in a new tab beside {@code tab}.
     */
    public static void showChooser(Context context, @Nullable Tab tab, String url) {
        if (tab == null) return;
        WebContents source = tab.getWebContents();
        if (source == null) return;
        boolean inContainer = !ContainersBridge.containerOf(source).isEmpty();
        choose(
                context,
                tab.getProfile(),
                R.string.contextmenu_open_in_container,
                /* skipped= */ null,
                inContainer,
                id -> {
                    // The tab may have closed while the chooser was open.
                    @Nullable WebContents current = tab.isDestroyed() ? null : tab.getWebContents();
                    if (current == null || current.isDestroyed()) return;
                    ContainersBridge.openInNewTab(current, url, id);
                });
    }

    /**
     * The tab switcher button menu's New tab in container, after its New tab; null in incognito,
     * which has no containers.
     */
    public static @Nullable ListItem newTabMenuItem(Profile profile) {
        if (profile.isOffTheRecord()) return null;
        return new ListItemBuilder()
                .withTitleRes(R.string.containers_new_tab_in)
                .withMenuId(NEW_TAB_MENU_ID)
                .withStartIconRes(R.drawable.new_tab_icon)
                .build();
    }

    /**
     * New tab in container's chooser: the user's containers, a new temporary container and no
     * container. {@code openNewTab} then opens the new tab, as the menu's New tab does.
     */
    public static void chooseNewTab(Context context, Profile profile, Runnable openNewTab) {
        choose(
                context,
                profile.getOriginalProfile(),
                R.string.containers_new_tab_in,
                /* skipped= */ null,
                /* withNone= */ true,
                id -> {
                    sNewTabContainer = id;
                    try {
                        openNewTab.run();
                    } finally {
                        sNewTabContainer = null;
                    }
                });
    }

    /** A tab menu's Reopen in container; null for an incognito tab. */
    public static @Nullable ListItem reopenMenuItem(Tab tab) {
        if (tab.isIncognitoBranded()) return null;
        return new ListItemBuilder()
                .withTitleRes(R.string.containers_reopen)
                .withMenuId(REOPEN_MENU_ID)
                .withStartIconRes(R.drawable.ic_refresh_24dp)
                .build();
    }

    /**
     * Handles a tab menu's item {@code menuId} when it is Reopen in container, and returns whether
     * it was. Its chooser has the containers but {@code tab}'s own; the page opens in the one
     * picked beside {@code tab}, which closes.
     */
    public static boolean onTabMenuItem(int menuId, Tab tab, TabModel model) {
        if (menuId != REOPEN_MENU_ID) return false;
        WebContents source = tab.getWebContents();
        @Nullable WindowAndroid window = tab.getWindowAndroid();
        @Nullable Activity activity = window != null ? window.getActivity().get() : null;
        if (source == null || activity == null) return true;
        String current = ContainersBridge.containerOf(source);
        choose(
                activity,
                tab.getProfile(),
                R.string.containers_reopen,
                current,
                !current.isEmpty(),
                id -> {
                    @Nullable WebContents contents = tab.isDestroyed() ? null : tab.getWebContents();
                    if (contents == null || contents.isDestroyed()) return;
                    ContainersBridge.openInNewTab(contents, tab.getUrl().getSpec(), id);
                    model.getTabRemover()
                            .closeTabs(
                                    TabClosureParams.closeTab(tab).allowUndo(false).build(),
                                    /* allowDialog= */ false);
                });
        return true;
    }

    /**
     * Asks which container to use: the user's containers but {@code skipped}, a new temporary
     * container, and no container when {@code withNone}. {@code onChosen} gets the id picked, a
     * new temporary container's for that choice, or "" for no container.
     */
    private static void choose(
            Context context,
            Profile profile,
            @StringRes int title,
            @Nullable String skipped,
            boolean withNone,
            Callback<String> onChosen) {
        List<String> ids = new ArrayList<>();
        List<CharSequence> labels = new ArrayList<>();
        for (ContainersBridge.Container container : ContainersBridge.getContainers(profile)) {
            if (container.id.equals(skipped)) continue;
            ids.add(container.id);
            labels.add(
                    ContainerIcons.withIcon(
                            context, container.name, container.icon, container.argb));
        }
        ids.add(ContainersBridge.TEMPORARY);
        labels.add(context.getString(R.string.containers_new_temporary));
        if (withNone) {
            ids.add("");
            labels.add(context.getString(R.string.containers_none));
        }
        new AlertDialog.Builder(context)
                .setTitle(title)
                .setItems(
                        labels.toArray(new CharSequence[0]),
                        (dialog, which) -> {
                            String id = ids.get(which);
                            if (id.equals(ContainersBridge.TEMPORARY)) {
                                id = ContainersBridge.createTemporary(profile);
                            }
                            onChosen.onResult(id);
                        })
                .show();
    }

    /**
     * Draws a line in the color of tab {@code tabId}'s container along the bottom of its tab
     * switcher row {@code row} (TabGridView), as Firefox draws one under the tab; none outside
     * containers.
     */
    public static void markTabRow(ViewGroup row, int tabId) {
        Tab tab = TabWindowManagerSingleton.getInstance().getTabById(tabId);
        WebContents webContents = tab != null ? tab.getWebContents() : null;
        int argb = webContents != null ? ContainersBridge.colorOf(webContents) : 0;
        View line = row.findViewWithTag(LINE_TAG);
        if (argb == 0) {
            if (line != null) line.setVisibility(View.GONE);
            return;
        }
        if (line == null) {
            line = new View(row.getContext());
            line.setTag(LINE_TAG);
            float density = row.getResources().getDisplayMetrics().density;
            int margin =
                    row.getResources()
                            .getDimensionPixelSize(
                                    org.chromium.chrome.tab_ui.R.dimen.tab_grid_card_margin);
            FrameLayout.LayoutParams params =
                    new FrameLayout.LayoutParams(
                            ViewGroup.LayoutParams.MATCH_PARENT,
                            Math.round(3 * density),
                            Gravity.BOTTOM);
            // Inside the card, clear of its rounded corners.
            int inset = margin + Math.round(12 * density);
            params.setMargins(inset, 0, inset, margin);
            row.addView(line, params);
        }
        line.setBackgroundColor(argb);
        line.setVisibility(View.VISIBLE);
    }

    /**
     * Puts {@code tab}'s container's icon, in its color, at the corner of the tab switcher button's
     * icon in {@code toolbar}, and its name in the button's state for screen readers; nothing for a
     * tab outside containers.
     */
    public static void markToolbar(View toolbar, @Nullable Tab tab) {
        View button = toolbar.findViewById(R.id.tab_switcher_button);
        if (button == null) return;
        @Nullable WebContents contents = tab != null ? tab.getWebContents() : null;
        String id = contents != null ? ContainersBridge.containerOf(contents) : "";
        ContainersBridge.@Nullable Container container = null;
        if (tab != null && !id.isEmpty()) {
            for (ContainersBridge.Container c : ContainersBridge.getContainers(tab.getProfile())) {
                if (c.id.equals(id)) container = c;
            }
        }
        if (container == null) {
            button.setForeground(null);
            button.setStateDescription(null);
            return;
        }
        float density = button.getResources().getDisplayMetrics().density;
        Drawable icon =
                ContainerIcons.drawable(
                        container.icon, container.argb, Math.round(12 * density));
        LayerDrawable layer = new LayerDrawable(new Drawable[] {icon});
        // Centered, then moved up and to the end by half the 24dp icon.
        int offset = Math.round(24 * density);
        layer.setLayerGravity(0, Gravity.CENTER);
        layer.setLayerInsetRelative(0, offset, 0, 0, offset);
        button.setForeground(layer);
        button.setStateDescription(
                button.getContext().getString(R.string.containers_current, container.name));
    }
}
