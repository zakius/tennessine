// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.containers;

import android.content.Context;
import android.content.DialogInterface;
import android.text.SpannableString;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import androidx.appcompat.app.AlertDialog;

import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.app.tabwindow.TabWindowManagerSingleton;
import org.chromium.chrome.browser.content.WebContentsFactory;
import org.chromium.chrome.browser.profiles.Profile;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.chrome.browser.tab.TabLaunchType;
import org.chromium.content_public.browser.WebContents;

import java.util.ArrayList;
import java.util.List;

/**
 * Tabs in containers: the container a new tab starts in, the link menu's "Open in container" and
 * the tab switcher's color line. A tab stays in the container its WebContents is created in
 * (dubna's containers_tabs.h), so these only pick that container.
 */
@NullMarked
public final class Containers {
    private Containers() {}

    /** The link menu item's id: one no resource has (generated ids stay below 0x01000000). */
    public static final int MENU_ID = View.generateViewId();

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

    private static String containerFor(Tab tab, Profile profile) {
        switch (tab.getLaunchType()) {
            case TabLaunchType.FROM_LINK:
            case TabLaunchType.FROM_LONGPRESS_FOREGROUND:
            case TabLaunchType.FROM_LONGPRESS_BACKGROUND:
            case TabLaunchType.FROM_LONGPRESS_FOREGROUND_IN_GROUP:
            case TabLaunchType.FROM_LONGPRESS_BACKGROUND_IN_GROUP:
            case TabLaunchType.FROM_LINK_CREATING_NEW_WINDOW:
            case TabLaunchType.FROM_HISTORY_NAVIGATION_FOREGROUND:
            case TabLaunchType.FROM_HISTORY_NAVIGATION_BACKGROUND:
                Tab parent = TabWindowManagerSingleton.getInstance().getTabById(tab.getParentId());
                WebContents source = parent != null ? parent.getWebContents() : null;
                return source != null ? ContainersBridge.containerOf(source) : "";
            case TabLaunchType.FROM_CHROME_UI:
            case TabLaunchType.FROM_TAB_SWITCHER_UI:
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
        Profile profile = tab.getProfile();
        List<String> ids = new ArrayList<>();
        List<CharSequence> labels = new ArrayList<>();
        for (ContainersBridge.Container container : ContainersBridge.getContainers(profile)) {
            ids.add(container.id);
            labels.add(withDot(container.name, container.argb));
        }
        ids.add(ContainersBridge.TEMPORARY);
        labels.add(context.getString(R.string.containers_new_temporary));
        if (!ContainersBridge.containerOf(source).isEmpty()) {
            ids.add("");
            labels.add(context.getString(R.string.containers_none));
        }
        new AlertDialog.Builder(context)
                .setTitle(R.string.contextmenu_open_in_container)
                .setItems(labels.toArray(new CharSequence[0]), opener(tab, profile, ids, url))
                .show();
    }

    /** The chooser's action: opens {@code url} in container {@code ids[which]}. */
    private static DialogInterface.OnClickListener opener(
            Tab tab, Profile profile, List<String> ids, String url) {
        return (dialog, which) -> {
            // The tab may have closed while the chooser was open.
            @Nullable WebContents source = tab.isDestroyed() ? null : tab.getWebContents();
            if (source == null || source.isDestroyed()) return;
            String id = ids.get(which);
            if (id.equals(ContainersBridge.TEMPORARY)) {
                id = ContainersBridge.createTemporary(profile);
            }
            ContainersBridge.openInNewTab(source, url, id);
        };
    }

    /** {@code name} after a dot in color {@code argb}. */
    static CharSequence withDot(String name, int argb) {
        SpannableString text = new SpannableString("●  " + name);
        text.setSpan(new ForegroundColorSpan(argb), 0, 1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        return text;
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
}
