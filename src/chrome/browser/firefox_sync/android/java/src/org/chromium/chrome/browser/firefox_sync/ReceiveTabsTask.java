// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.firefox_sync;

import android.content.Context;
import android.text.format.DateUtils;

import org.jni_zero.CalledByNative;

import org.chromium.base.ContextUtils;
import org.chromium.base.task.PostTask;
import org.chromium.base.task.TaskTraits;
import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.browser.profiles.ProfileManager;
import org.chromium.components.background_task_scheduler.BackgroundTaskSchedulerFactory;
import org.chromium.components.background_task_scheduler.NativeBackgroundTask;
import org.chromium.components.background_task_scheduler.TaskIds;
import org.chromium.components.background_task_scheduler.TaskInfo;
import org.chromium.components.background_task_scheduler.TaskParameters;

/**
 * Fetches the tabs and close requests other devices sent (FirefoxSyncService::PollCommands) at the
 * receive interval while signed in, with the browser in the background or not running. A running
 * browser also fetches them on the service's own timer and when it comes to the front.
 */
@NullMarked
public class ReceiveTabsTask extends NativeBackgroundTask {
    // PollCommands reports no completion, so the task holds the process this long: the fetch is
    // a request or two. Commands a killed process didn't handle wait on the accounts server for
    // the next fetch, since the account that marks them handled is saved before they apply.
    private static final long HOLD_MS = 30 * DateUtils.SECOND_IN_MILLIS;

    private @Nullable FirefoxSyncBridge mBridge;

    /**
     * Runs the task every {@code minutes}, or never for 0. With {@code replace}, a scheduled task
     * takes the new interval; without, it stays as it is.
     */
    @CalledByNative
    public static void schedule(int minutes, boolean replace) {
        Context context = ContextUtils.getApplicationContext();
        if (minutes <= 0) {
            BackgroundTaskSchedulerFactory.getScheduler()
                    .cancel(context, TaskIds.FIREFOX_SYNC_RECEIVE_TABS_JOB_ID);
            return;
        }
        long interval = minutes * DateUtils.MINUTE_IN_MILLIS;
        TaskInfo task =
                TaskInfo.createTask(
                                TaskIds.FIREFOX_SYNC_RECEIVE_TABS_JOB_ID,
                                TaskInfo.PeriodicInfo.create()
                                        .setIntervalMs(interval)
                                        .setFlexMs(interval / 3)
                                        .build())
                        .setRequiredNetworkType(TaskInfo.NetworkType.ANY)
                        .setIsPersisted(true)
                        .setUpdateCurrent(replace)
                        .build();
        BackgroundTaskSchedulerFactory.getScheduler().schedule(context, task);
    }

    @Override
    protected int onStartTaskBeforeNativeLoaded(
            Context context, TaskParameters taskParameters, TaskFinishedCallback callback) {
        return StartBeforeNativeResult.LOAD_NATIVE;
    }

    @Override
    protected void onStartTaskWithNative(
            Context context, TaskParameters taskParameters, TaskFinishedCallback callback) {
        FirefoxSyncBridge bridge =
                new FirefoxSyncBridge(ProfileManager.getLastUsedRegularProfile(), this::poll);
        mBridge = bridge;
        bridge.pollCommands();
        PostTask.postDelayedTask(
                TaskTraits.UI_DEFAULT,
                () -> {
                    mBridge = null;
                    bridge.destroy();
                    callback.taskFinished(false);
                },
                HOLD_MS);
    }

    // On each status change too: in a browser that just started, PollCommands does nothing until
    // the account is decrypted, and a sync under way fetches the commands itself. It does nothing
    // within a minute of the last fetch.
    private void poll() {
        if (mBridge != null) mBridge.pollCommands();
    }

    @Override
    protected boolean onStopTaskBeforeNativeLoaded(Context context, TaskParameters taskParameters) {
        // A periodic task runs again at its next interval.
        return false;
    }

    @Override
    protected boolean onStopTaskWithNative(Context context, TaskParameters taskParameters) {
        return false;
    }
}
