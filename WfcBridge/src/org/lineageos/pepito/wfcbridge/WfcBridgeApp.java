/*
 * WfcBridge — mirror the user's Wi-Fi-calling toggle into a system property.
 *
 * A native radio daemon (the qmux_wfc_bridge VoWiFi feeder) cannot read the
 * Settings provider, and toggling Wi-Fi calling in Settings emits nothing to
 * the modem or any property on this stack. This persistent platform app watches
 * Settings.Global.wfc_ims_enabled and mirrors it into
 *   persist.sys.pepito.wfc_enabled = 1 | 0
 * init.qmux.rc starts the feeder only when that prop is 1 and stops it when 0,
 * so the feeder does not run (and burns no power) while WFC is off.
 *
 * Pepito-only (shipped behind TARGET_DEVICE_PEPITO). No Activity/Service: the
 * observer lives in the Application, which android:persistent keeps resident.
 *
 * SAFETY: this is android:persistent, so an uncaught exception here would put the
 * app into a crash loop that can force a device reboot loop. Every path is
 * therefore wrapped so onCreate can NEVER throw — a mis-scoped sepolicy grant or
 * any other failure must degrade to "prop not mirrored", never to a bootloop.
 */
package org.lineageos.pepito.wfcbridge;

import android.app.Application;
import android.database.ContentObserver;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemProperties;
import android.provider.Settings;
import android.util.Log;

public class WfcBridgeApp extends Application {
    private static final String TAG = "WfcBridge";
    private static final String SETTING = "wfc_ims_enabled"; // Settings.Global key
    private static final String PROP = "persist.sys.pepito.wfc_enabled";

    private ContentObserver mObserver;

    @Override
    public void onCreate() {
        super.onCreate();
        try {
            mObserver = new ContentObserver(new Handler(Looper.getMainLooper())) {
                @Override
                public void onChange(boolean selfChange) {
                    sync();
                }
            };
            getContentResolver().registerContentObserver(
                    Settings.Global.getUriFor(SETTING), false, mObserver);
            sync();
        } catch (Throwable t) {
            // Never let a persistent-app failure become a reboot loop.
            Log.e(TAG, "init failed; WFC gate not mirrored", t);
        }
    }

    private void sync() {
        try {
            // Default to enabled when the setting is unset so we never silently
            // disable WFC; the explicit =0 path is what stops the feeder.
            boolean on = Settings.Global.getInt(getContentResolver(), SETTING, 1) != 0;
            String want = on ? "1" : "0";
            SystemProperties.set(PROP, want);
            Log.i(TAG, SETTING + "=" + on + " -> " + PROP + "=" + want);
        } catch (Throwable t) {
            Log.e(TAG, "sync failed", t);
        }
    }
}
