// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.crash_log;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.net.Uri;
import android.provider.MediaStore;

import org.chromium.base.ContextUtils;
import org.chromium.build.annotations.NullMarked;

import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.io.StringWriter;
import java.util.Date;

/**
 * Keeps the stack trace of the last uncaught Java exception, so it can be shared from Settings →
 * Last crash without adb, and in Downloads/tennessine-crash.txt for crashes at startup, when
 * Settings can't be reached. A native crash is found at the next start through Android's exit
 * records: its description goes to Settings → Last crash, its tombstone (protobuf) to
 * Downloads/tennessine-native-crash.pb.
 */
@NullMarked
public final class CrashLog {
    static final String PREF = "tennessine.crash_log.last";
    private static final String NATIVE_SEEN_PREF = "tennessine.crash_log.native_seen";

    private CrashLog() {}

    /** Wraps the default handler; call early in the browser process. */
    public static void install() {
        new Thread(CrashLog::recordNativeCrash, "CrashLog").start();
        Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler(
                (thread, throwable) -> {
                    try {
                        StringWriter trace = new StringWriter();
                        throwable.printStackTrace(new PrintWriter(trace));
                        String log = new Date() + " on " + thread.getName() + "\n" + trace;
                        ContextUtils.getAppSharedPreferences().edit().putString(PREF, log).commit();
                        saveToDownloads(log);
                    } catch (Throwable ignored) {
                        // Never get in the way of the crash itself.
                    }
                    if (previous != null) previous.uncaughtException(thread, throwable);
                });
    }

    /** The newest native crash of this app not recorded yet, if any. */
    private static void recordNativeCrash() {
        try {
            ActivityManager am =
                    ContextUtils.getApplicationContext().getSystemService(ActivityManager.class);
            if (am == null) return;
            long seen = ContextUtils.getAppSharedPreferences().getLong(NATIVE_SEEN_PREF, 0);
            for (ApplicationExitInfo exit :
                    am.getHistoricalProcessExitReasons(
                            ContextUtils.getApplicationContext().getPackageName(), 0, 5)) {
                if (exit.getTimestamp() <= seen) return;
                if (exit.getReason() != ApplicationExitInfo.REASON_CRASH_NATIVE) continue;
                ContextUtils.getAppSharedPreferences()
                        .edit()
                        .putLong(NATIVE_SEEN_PREF, exit.getTimestamp())
                        .putString(
                                PREF,
                                new Date(exit.getTimestamp())
                                        + " native crash in "
                                        + exit.getProcessName()
                                        + ": "
                                        + exit.getDescription()
                                        + "\nTombstone: Downloads/tennessine-native-crash.pb")
                        .commit();
                try (InputStream trace = exit.getTraceInputStream()) {
                    if (trace != null) {
                        saveToDownloads(
                                "tennessine-native-crash.pb",
                                "application/octet-stream",
                                trace.readAllBytes());
                    }
                }
                return;
            }
        } catch (Throwable ignored) {
            // Best effort.
        }
    }

    private static void saveToDownloads(String log) throws Exception {
        saveToDownloads(
                "tennessine-crash.txt", "text/plain", log.getBytes(StandardCharsets.UTF_8));
    }

    private static void saveToDownloads(String name, String type, byte[] data) throws Exception {
        ContentResolver resolver = ContextUtils.getApplicationContext().getContentResolver();
        ContentValues values = new ContentValues();
        values.put(MediaStore.Downloads.DISPLAY_NAME, name);
        values.put(MediaStore.Downloads.MIME_TYPE, type);
        Uri uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
        if (uri == null) return;
        try (OutputStream out = resolver.openOutputStream(uri)) {
            if (out != null) out.write(data);
        }
    }
}
