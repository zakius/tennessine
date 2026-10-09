// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.firefox_sync;

import org.json.JSONException;
import org.json.JSONObject;
import org.jni_zero.CalledByNative;
import org.jni_zero.JNINamespace;
import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;

import org.chromium.build.annotations.NullMarked;
import org.chromium.chrome.browser.profiles.Profile;

/**
 * Java side of FirefoxSyncService (firefox_sync_bridge.cc). The status is the service's
 * GetStatus() as JSON, the same object oganesson's settings page reads.
 */
@JNINamespace("firefox_sync")
@NullMarked
public class FirefoxSyncBridge {
    private final Profile mProfile;
    private final Runnable mOnStatusChanged;
    private long mNativeFirefoxSyncBridge;

    /** Calls {@code onStatusChanged} whenever the status changes, until {@link #destroy}. */
    public FirefoxSyncBridge(Profile profile, Runnable onStatusChanged) {
        mProfile = profile;
        mOnStatusChanged = onStatusChanged;
        mNativeFirefoxSyncBridge = FirefoxSyncBridgeJni.get().init(this, profile);
    }

    public void destroy() {
        if (mNativeFirefoxSyncBridge != 0) {
            FirefoxSyncBridgeJni.get().destroy(mNativeFirefoxSyncBridge);
            mNativeFirefoxSyncBridge = 0;
        }
    }

    public JSONObject getStatus() {
        try {
            return new JSONObject(FirefoxSyncBridgeJni.get().getStatus(mProfile));
        } catch (JSONException e) {
            return new JSONObject();
        }
    }

    public void signIn() {
        FirefoxSyncBridgeJni.get().signIn(mProfile);
    }

    public void signOut() {
        FirefoxSyncBridgeJni.get().signOut(mProfile);
    }

    public void syncNow() {
        FirefoxSyncBridgeJni.get().syncNow(mProfile);
    }

    /** The accounts server, or with {@code syncStorage} the sync (token) server. */
    public void setServer(String server, boolean syncStorage) {
        FirefoxSyncBridgeJni.get().setServer(mProfile, server, syncStorage);
    }

    public void setTypeEnabled(String name, boolean enabled) {
        FirefoxSyncBridgeJni.get().setTypeEnabled(mProfile, name, enabled);
    }

    @CalledByNative
    private void onStatusChanged() {
        mOnStatusChanged.run();
    }

    @NativeMethods
    interface Natives {
        long init(FirefoxSyncBridge caller, @JniType("Profile*") Profile profile);

        void destroy(long nativeFirefoxSyncBridge);

        @JniType("std::string")
        String getStatus(@JniType("Profile*") Profile profile);

        void signIn(@JniType("Profile*") Profile profile);

        void signOut(@JniType("Profile*") Profile profile);

        void syncNow(@JniType("Profile*") Profile profile);

        void setServer(
                @JniType("Profile*") Profile profile,
                @JniType("std::string") String server,
                boolean syncStorage);

        void setTypeEnabled(
                @JniType("Profile*") Profile profile,
                @JniType("std::string") String name,
                boolean enabled);
    }
}
