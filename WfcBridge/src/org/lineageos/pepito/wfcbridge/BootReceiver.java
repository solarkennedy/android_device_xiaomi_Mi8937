/*
 * Exists only to force WfcBridgeApp's process to start at boot (delivering a
 * broadcast creates the app process, which runs WfcBridgeApp.onCreate and
 * registers the ContentObserver + does the initial mirror). android:persistent
 * then keeps the process resident. onReceive itself has nothing to do.
 */
package org.lineageos.pepito.wfcbridge;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

public class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        // No-op: the side effect (process start -> WfcBridgeApp.onCreate) is the point.
    }
}
