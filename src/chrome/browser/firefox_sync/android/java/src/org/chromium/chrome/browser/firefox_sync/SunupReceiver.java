// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.firefox_sync;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

import org.chromium.build.annotations.NullMarked;

/**
 * What Sunup sends (SunupPush). Exported, as the distributor's broadcasts come from another app;
 * one with a token other than the registration's is ignored.
 */
@NullMarked
public class SunupReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        String token = intent.getStringExtra("token");
        String action = intent.getAction();
        if (token == null || action == null || !SunupPush.isCurrent(token)) return;
        String id = intent.getStringExtra("id");
        if (id != null) SunupPush.acknowledge(token, id);
        switch (action) {
            case SunupPush.NEW_ENDPOINT:
                String endpoint = intent.getStringExtra("endpoint");
                if (endpoint != null) SunupPush.onNewEndpoint(endpoint);
                break;
            case SunupPush.REGISTRATION_FAILED:
                SunupPush.onRegistrationFailed(intent.getStringExtra("reason"));
                break;
            case SunupPush.UNREGISTERED:
                SunupPush.onUnregistered();
                break;
            case SunupPush.MESSAGE:
                SunupPush.onMessage();
                break;
        }
    }
}
