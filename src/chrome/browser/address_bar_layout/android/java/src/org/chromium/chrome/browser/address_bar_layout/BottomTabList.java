// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.address_bar_layout;

import android.view.View;
import android.view.ViewGroup;

import androidx.recyclerview.widget.GridLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;

/**
 * The tab switcher's tab list with the address bar at the bottom: as tall as its tabs, at the
 * bottom of the pane. RecyclerView takes its smaller height at once when a tab closes, which
 * moves every row down a row before the close animation moves the rows below the tab back up.
 * Instead, while the animation runs, the list keeps its height and gets the closed row's height as
 * top padding: its layout then puts the rows above the closed tab a row lower and leaves the rows
 * below where they are, and RecyclerView's own animation moves the rows above down. Afterwards the
 * height and the padding go back in one layout, which moves nothing on the screen. One tab per row
 * only (the list mode); a grid row closes as before.
 */
@NullMarked
public final class BottomTabList {
    private BottomTabList() {}

    /** Smooths the closing of tabs in `list`, whose adapter is set. */
    public static void smoothRemovals(RecyclerView list) {
        RecyclerView.Adapter<?> adapter = list.getAdapter();
        if (adapter == null) return;
        adapter.registerAdapterDataObserver(
                new RecyclerView.AdapterDataObserver() {
                    @Override
                    public void onItemRangeRemoved(int positionStart, int itemCount) {
                        // Before the layout that would shrink the list.
                        if (list.getHeight() == 0 || list.getMinimumHeight() > 0) return;
                        int removed = rowsHeight(list, positionStart, itemCount);
                        RecyclerView.ItemAnimator animator = list.getItemAnimator();
                        if (removed <= 0 || animator == null) return;
                        boolean clips = list.getClipToPadding();
                        int top = list.getPaddingTop();
                        list.setMinimumHeight(list.getHeight());
                        list.setClipToPadding(false);
                        setTopPadding(list, top + removed);
                        whenAnimated(list, animator, () -> {
                            list.setMinimumHeight(0);
                            setTopPadding(list, top);
                            list.setClipToPadding(clips);
                        });
                    }
                });
    }

    /** The height of the rows of `count` items from `start`, as laid out; 0 when not all are. */
    private static int rowsHeight(RecyclerView list, int start, int count) {
        RecyclerView.LayoutManager manager = list.getLayoutManager();
        if (manager == null
                || (manager instanceof GridLayoutManager grid && grid.getSpanCount() != 1)) {
            return 0;
        }
        int height = 0;
        for (int position = start; position < start + count; position++) {
            RecyclerView.ViewHolder holder = list.findViewHolderForAdapterPosition(position);
            @Nullable View view = holder != null ? holder.itemView : null;
            if (view == null) return 0;
            ViewGroup.MarginLayoutParams params = (ViewGroup.MarginLayoutParams) view.getLayoutParams();
            height += manager.getDecoratedMeasuredHeight(view) + params.topMargin + params.bottomMargin;
        }
        return height;
    }

    private static void setTopPadding(RecyclerView list, int top) {
        list.setPadding(list.getPaddingLeft(), top, list.getPaddingRight(), list.getPaddingBottom());
    }

    /**
     * Runs `done` once the animations of the next layout end. RecyclerView starts them on the
     * animation frame it asks for during that layout, so this asks for one after it.
     */
    private static void whenAnimated(
            RecyclerView list, RecyclerView.ItemAnimator animator, Runnable done) {
        list.addOnLayoutChangeListener(
                new View.OnLayoutChangeListener() {
                    @Override
                    public void onLayoutChange(
                            View view, int l, int t, int r, int b, int ol, int ot, int or, int ob) {
                        list.removeOnLayoutChangeListener(this);
                        list.postOnAnimation(() -> animator.isRunning(done::run));
                    }
                });
    }
}
