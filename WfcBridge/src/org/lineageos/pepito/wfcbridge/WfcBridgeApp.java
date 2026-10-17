/*
 * WfcBridge — mirror the user's Wi-Fi-calling settings into system properties.
 *
 * A native radio daemon (the qmux_wfc_bridge VoWiFi feeder) cannot read the
 * Settings provider, and toggling Wi-Fi calling in Settings emits nothing to
 * the modem or any property on this stack. This persistent platform app mirrors
 *   Settings.Global.wfc_ims_enabled      -> persist.sys.pepito.wfc_enabled = 1 | 0
 *   siminfo wfc_ims_mode (Calling pref)  -> persist.sys.pepito.wfc_mode    = 0 | 1 | 2
 *   Wi-Fi network VALIDATED (ConnMgr)    -> sys.pepito.wfc_wifi_ok         = 1 | 0
 * init.qmux.rc copies each into a vendor.qmux.* prop for the feeder, which gates
 * feeding on the first and steers the modem's in-call IWLAN->LTE threshold on
 * the other two (memory wfc-wifi-preference-lane).
 *
 * Pepito-only (shipped behind TARGET_DEVICE_PEPITO). No Activity/Service: the
 * observers live in the Application, which android:persistent keeps resident.
 *
 * SAFETY: this is android:persistent, so an uncaught exception here would put the
 * app into a crash loop that can force a device reboot loop. Every path is
 * therefore wrapped so onCreate can NEVER throw — a mis-scoped sepolicy grant or
 * any other failure must degrade to "prop not mirrored", never to a bootloop.
 */
package org.lineageos.pepito.wfcbridge;

import android.app.Application;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.database.ContentObserver;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.os.Handler;
import android.os.Looper;
import android.os.PersistableBundle;
import android.os.SystemProperties;
import android.provider.Settings;
import android.provider.Telephony;
import android.telephony.CarrierConfigManager;
import android.telephony.SubscriptionManager;
import android.util.Log;

public class WfcBridgeApp extends Application {
    private static final String TAG = "WfcBridge";
    private static final String SETTING = "wfc_ims_enabled"; // Settings.Global key
    private static final String PROP = "persist.sys.pepito.wfc_enabled";
    // The user's Settings "Calling preference" (ImsMmTelManager.WIFI_MODE_*:
    // 0 wifi-only, 1 cellular-preferred, 2 wifi-preferred). ImsManager keeps it
    // per subscription in the siminfo table, not in Settings.Global, so it is
    // read through SubscriptionManager and watched via the siminfo URI. Derived
    // exactly like ImsManager.getWfcMode(false), so the mirror always matches
    // what the Settings row shows: when the carrier marks the mode editable and
    // the user has set it, the siminfo value; otherwise the carrier default
    // (carrier_default_wfc_ims_mode_int — 1 for Verizon, 2 in AOSP's defaults).
    // Roaming mode (wfc_ims_roaming_mode) is deliberately not mirrored: the
    // feeder's lever is for the home network.
    private static final String MODE_PROP = "persist.sys.pepito.wfc_mode";
    private static final int MODE_CELLULAR_PREFERRED = 1;
    private static final int SUB_PROPERTY_NOT_INITIALIZED = -1;
    // Live (not persisted) verdict on the Wi-Fi network from ConnectivityManager:
    // 1 while a Wi-Fi network with NET_CAPABILITY_VALIDATED is up, else 0. The
    // feeder only holds a call on Wi-Fi while this is 1, so an AP whose uplink
    // has died (association and RSSI still perfect) lets the modem rescue the
    // call to LTE instead of dropping it.
    private static final String WIFI_OK_PROP = "sys.pepito.wfc_wifi_ok";

    private ContentObserver mObserver;
    private ContentObserver mModeObserver;
    private BroadcastReceiver mCarrierConfigReceiver;
    private ConnectivityManager.NetworkCallback mWifiCallback;
    private Network mWifi;          // the Wi-Fi network the mirror currently reflects

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
        try {
            mModeObserver = new ContentObserver(new Handler(Looper.getMainLooper())) {
                @Override
                public void onChange(boolean selfChange) {
                    syncMode();
                }
            };
            // Any siminfo row change (the mode column, a SIM swap, the default
            // sub moving) re-derives the mirror; it is one cheap query.
            getContentResolver().registerContentObserver(
                    Telephony.SimInfo.CONTENT_URI, true, mModeObserver);
            // The carrier default / editable flag come from carrier config,
            // which loads after the SIM and can change on its own.
            mCarrierConfigReceiver = new BroadcastReceiver() {
                @Override
                public void onReceive(Context context, Intent intent) {
                    syncMode();
                }
            };
            registerReceiver(mCarrierConfigReceiver,
                    new IntentFilter(CarrierConfigManager.ACTION_CARRIER_CONFIG_CHANGED),
                    Context.RECEIVER_NOT_EXPORTED);
            syncMode();
        } catch (Throwable t) {
            Log.e(TAG, "mode init failed; Calling preference not mirrored", t);
        }
        try {
            // Defined from the start: an unset prop reads as "not ok" in the
            // feeder, which is the safe (stock-threshold) direction.
            setWifiOk(false, null);
            ConnectivityManager cm = getSystemService(ConnectivityManager.class);
            mWifiCallback = new ConnectivityManager.NetworkCallback() {
                @Override
                public void onCapabilitiesChanged(Network network, NetworkCapabilities caps) {
                    boolean ok = caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED);
                    if (ok || network.equals(mWifi)) {
                        setWifiOk(ok, network);
                    }
                }

                @Override
                public void onLost(Network network) {
                    if (network.equals(mWifi)) {
                        setWifiOk(false, null);
                    }
                }
            };
            cm.registerNetworkCallback(new NetworkRequest.Builder()
                    .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                    .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                    .build(), mWifiCallback, new Handler(Looper.getMainLooper()));
        } catch (Throwable t) {
            Log.e(TAG, "wifi init failed; Wi-Fi validated not mirrored", t);
        }
    }

    private void setWifiOk(boolean ok, Network network) {
        try {
            mWifi = ok ? network : null;
            String want = ok ? "1" : "0";
            if (!want.equals(SystemProperties.get(WIFI_OK_PROP, ""))) {
                SystemProperties.set(WIFI_OK_PROP, want);
                Log.i(TAG, "wifi validated=" + ok + " (" + network + ") -> " + WIFI_OK_PROP
                        + "=" + want);
            }
        } catch (Throwable t) {
            Log.e(TAG, "wifi sync failed", t);
        }
    }

    private void syncMode() {
        try {
            int subId = SubscriptionManager.getDefaultVoiceSubscriptionId();
            if (!SubscriptionManager.isValidSubscriptionId(subId)) {
                subId = SubscriptionManager.getDefaultSubscriptionId();
            }
            // No SIM / telephony not up yet: stock behaviour until a siminfo or
            // carrier-config change re-runs this.
            int mode = MODE_CELLULAR_PREFERRED;
            if (SubscriptionManager.isValidSubscriptionId(subId)) {
                mode = wfcModeFor(subId);
            }
            String want = Integer.toString(mode);
            if (!want.equals(SystemProperties.get(MODE_PROP, ""))) {
                SystemProperties.set(MODE_PROP, want);
                Log.i(TAG, "wfc mode(sub " + subId + ")=" + mode + " -> " + MODE_PROP
                        + "=" + want);
            }
        } catch (Throwable t) {
            Log.e(TAG, "mode sync failed", t);
        }
    }

    /** Same derivation as ImsManager.getWfcMode(false) for the home network. */
    private int wfcModeFor(int subId) {
        boolean editable = true;
        int carrierDefault = MODE_CELLULAR_PREFERRED;
        boolean haveConfig = false;
        CarrierConfigManager ccm = getSystemService(CarrierConfigManager.class);
        PersistableBundle b = ccm == null ? null : ccm.getConfigForSubId(subId,
                CarrierConfigManager.KEY_EDITABLE_WFC_MODE_BOOL,
                CarrierConfigManager.KEY_CARRIER_DEFAULT_WFC_IMS_MODE_INT);
        if (b != null && CarrierConfigManager.isConfigForIdentifiedCarrier(b)) {
            editable = b.getBoolean(CarrierConfigManager.KEY_EDITABLE_WFC_MODE_BOOL, true);
            carrierDefault = b.getInt(CarrierConfigManager.KEY_CARRIER_DEFAULT_WFC_IMS_MODE_INT,
                    MODE_CELLULAR_PREFERRED);
            haveConfig = true;
        }
        if (!editable) {
            return carrierDefault;
        }
        int v = SubscriptionManager.getIntegerSubscriptionProperty(subId,
                SubscriptionManager.WFC_IMS_MODE, SUB_PROPERTY_NOT_INITIALIZED, this);
        if (v != SUB_PROPERTY_NOT_INITIALIZED) {
            return v;
        }
        // Never set by the user: the Settings row shows the carrier default. If
        // carrier config has not loaded for this SIM yet, stay stock until it
        // does (ACTION_CARRIER_CONFIG_CHANGED re-runs us).
        return haveConfig ? carrierDefault : MODE_CELLULAR_PREFERRED;
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
