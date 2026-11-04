// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.app_links;

import android.app.Activity;
import android.app.ActivityManager.AppTask;
import android.app.ActivityOptions;
import android.content.Intent;
import android.content.pm.ResolveInfo;
import android.net.Uri;
import android.os.SystemClock;

import org.chromium.base.ActivityState;
import org.chromium.base.ApplicationStatus;
import org.chromium.base.IntentUtils;
import org.chromium.build.annotations.NullMarked;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.IntentHandler;
import org.chromium.chrome.browser.util.AndroidTaskUtils;
import org.chromium.chrome.browser.util.DefaultBrowserInfo;
import org.chromium.ui.widget.Toast;

/** Opens a link from an installed web app in the default browser, whichever it is. */
@NullMarked
public final class DefaultBrowser {
    private DefaultBrowser() {}

    /**
     * Opens {@code url} from the web app of {@code activity} in the default browser. With
     * {@code background}, behind the web app, as far as Android allows: Android has no way to open
     * a link in another app in the background.
     */
    public static void open(Activity activity, String url, boolean background) {
        Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
        intent.addCategory(Intent.CATEGORY_BROWSABLE);
        // By package: a link in the web app's scope would otherwise go to its own WebAPK.
        // No default browser set: this browser.
        ResolveInfo browser = DefaultBrowserInfo.getDefaultWebBrowserInfo();
        String browserPackage =
                browser != null && browser.match != 0
                        ? browser.activityInfo.packageName
                        : activity.getPackageName();
        intent.setPackage(browserPackage);
        if (browserPackage.equals(activity.getPackageName())) {
            if (background) {
                // A tab added in the background, no app switch.
                ChromeTabbedActivity.openInBackgroundTab(url);
                Toast.makeText(activity, R.string.open_in_new_tab_toast, Toast.LENGTH_SHORT)
                        .show();
                return;
            }
            // As the web app's "Open in" menu item does: a tab, not the web app again.
            intent.putExtra(IntentHandler.EXTRA_FROM_OPEN_IN_BROWSER, true);
        }
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        if (!background) {
            IntentUtils.safeStartActivity(activity, intent);
            return;
        }
        // Launched behind the web app, the browser's task waits in recents. Browsers whose link
        // handler is a trampoline (Firefox, Chromium) still bring their main activity forward:
        // once it covers the web app, the web app moves back to the front. Android may refuse
        // that, and the browser stays in front.
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_DOCUMENT);
        long launchTime = SystemClock.elapsedRealtime();
        ApplicationStatus.registerStateListenerForActivity(
                new ApplicationStatus.ActivityStateListener() {
                    @Override
                    public void onActivityStateChange(Activity stopped, int newState) {
                        if (newState != ActivityState.STOPPED
                                && newState != ActivityState.DESTROYED) {
                            return;
                        }
                        ApplicationStatus.unregisterActivityStateListener(this);
                        // ponytail: a stop within 5 s is taken as the browser's doing, not the
                        // user leaving the web app.
                        if (newState != ActivityState.STOPPED
                                || SystemClock.elapsedRealtime() - launchTime > 5000) {
                            return;
                        }
                        AppTask task =
                                AndroidTaskUtils.getAppTaskFromId(stopped, stopped.getTaskId());
                        if (task != null) task.moveToFront();
                    }
                },
                activity);
        IntentUtils.safeStartActivity(
                activity, intent, ActivityOptions.makeTaskLaunchBehind().toBundle());
    }
}
