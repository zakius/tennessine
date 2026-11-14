// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.firefox_sync;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;

import org.chromium.build.annotations.NullMarked;

/**
 * UnifiedPush's RAISE_TO_FOREGROUND service: Sunup binds to it for a few seconds with each push,
 * which raises the browser's priority so the fetch the push starts can run. It does nothing else.
 */
@NullMarked
public class SunupRaiseService extends Service {
    private final IBinder mBinder = new Binder();

    @Override
    public IBinder onBind(Intent intent) {
        return mBinder;
    }
}
