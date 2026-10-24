// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.firefox_sync;

import android.content.Context;
import android.text.format.DateUtils;

import org.json.JSONObject;
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
    // The task ends when the fetch is handled; this caps it if the account never decrypts or a
    // sync under way takes long. Android stops a task after 10 minutes.
    private static final long TIMEOUT_MS = 5 * DateUtils.MINUTE_IN_MILLIS;

    private @Nullable FirefoxSyncBridge mBridge;
    private @Nullable TaskFinishedCallback mCallback;
    private boolean mPolling;

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
        mCallback = callback;
        mBridge = new FirefoxSyncBridge(ProfileManager.getLastUsedRegularProfile(), this::poll);
        PostTask.postDelayedTask(TaskTraits.UI_DEFAULT, this::finish, TIMEOUT_MS);
        poll();
    }

    // On each status change too: in a browser that just started, PollCommands has nothing to
    // fetch with until the saved account is decrypted, which signedIn waits for.
    private void poll() {
        if (mBridge == null || mPolling) return;
        JSONObject status = mBridge.getStatus();
        if (status.optString("email").isEmpty() || status.optBoolean("needsSignIn")) {
            finish();
        } else if (status.optBoolean("signedIn")) {
            mPolling = true;
            mBridge.pollCommands(this::finish);
        }
    }

    private void finish() {
        if (mBridge == null || mCallback == null) return;
        mBridge.destroy();
        mBridge = null;
        mCallback.taskFinished(false);
    }

    @Override
    protected boolean onStopTaskBeforeNativeLoaded(Context context, TaskParameters taskParameters) {
        // A periodic task runs again at its next interval.
        return false;
    }

    @Override
    protected boolean onStopTaskWithNative(Context context, TaskParameters taskParameters) {
        if (mBridge != null) {
            mBridge.destroy();
            mBridge = null;
        }
        return false;
    }
}
