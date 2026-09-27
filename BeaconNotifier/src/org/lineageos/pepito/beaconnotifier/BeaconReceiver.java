/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package org.lineageos.pepito.beaconnotifier;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.drawable.Icon;
import android.icu.text.BreakIterator;
import android.util.Log;

/**
 * Posts one notification per beacon handed over by XiaomiParts' BeaconNotifyController
 * (see PLAN-ble-beacon-notify.md). All the radio work stays there; this app exists
 * only so the channels belong to an ordinary UID and stay user-editable.
 *
 * Intent extras: txid, seq, status (0 OK, 1 WARNING, 2 CRITICAL, 3 plain), text.
 * A leading symbol/emoji in the text becomes the icon ({@link #renderGlyph}).
 */
public class BeaconReceiver extends BroadcastReceiver {

    private static final String TAG = "BeaconNotifier";

    public static final String ACTION_BEACON = "org.lineageos.pepito.beaconnotifier.BEACON";
    public static final String EXTRA_TXID = "txid";
    public static final String EXTRA_SEQ = "seq";
    public static final String EXTRA_STATUS = "status";
    public static final String EXTRA_TEXT = "text";

    // Indexed by status. Importance is only the initial default; the user owns the
    // rest (sound, vibration, DND) in Settings, which is the point of this app.
    private static final String[] CHANNEL_IDS =
            { "beacon_ok", "beacon_warning", "beacon_critical", "beacon" };
    private static final int[] CHANNEL_NAMES = { R.string.channel_ok, R.string.channel_warning,
            R.string.channel_critical, R.string.channel_plain };
    private static final int[] CHANNEL_IMPORTANCE = {
            NotificationManager.IMPORTANCE_LOW, NotificationManager.IMPORTANCE_DEFAULT,
            NotificationManager.IMPORTANCE_HIGH, NotificationManager.IMPORTANCE_DEFAULT };
    private static final String[] STATUS_LABELS = { "OK", "WARNING", "CRITICAL", null };
    private static final int[] STATUS_COLORS = { 0xFF2E7D32, 0xFFF9A825, 0xFFC62828, 0 };
    private static final int[] STATUS_ICONS = {
            android.R.drawable.presence_online, android.R.drawable.stat_sys_warning,
            android.R.drawable.stat_notify_error, R.drawable.ic_beacon };
    private static final int GLYPH_DP = 48;
    private static final int GLYPH_PLAIN_COLOR = 0xFF757575;

    @Override
    public void onReceive(final Context context, final Intent intent) {
        if (!ACTION_BEACON.equals(intent.getAction())) {
            return;
        }
        // Only XiaomiParts (the system UID) may post through us. We can't use a
        // signature permission: this app is deliberately not platform-signed.
        if (getSentFromUid() != android.os.Process.SYSTEM_UID) {
            Log.w(TAG, "dropping beacon from uid " + getSentFromUid());
            return;
        }
        final int txId = intent.getIntExtra(EXTRA_TXID, 0);
        final int seq = intent.getIntExtra(EXTRA_SEQ, 0);
        final int status = intent.getIntExtra(EXTRA_STATUS, 3) & 3;
        final String text = intent.getStringExtra(EXTRA_TEXT) != null
                ? intent.getStringExtra(EXTRA_TEXT) : "";
        Log.i(TAG, "beacon tx=" + txId + " seq=" + seq + " status=" + status
                + " text=\"" + text + "\"");
        post(context, txId, seq, status, text);
    }

    private void post(final Context context, final int txId, final int seq, final int status,
            final String text) {
        final NotificationManager nm = context.getSystemService(NotificationManager.class);
        if (nm == null) {
            return;
        }
        // createNotificationChannel is idempotent and never downgrades user edits.
        for (int i = 0; i < CHANNEL_IDS.length; i++) {
            nm.createNotificationChannel(new NotificationChannel(CHANNEL_IDS[i],
                    context.getString(CHANNEL_NAMES[i]), CHANNEL_IMPORTANCE[i]));
        }
        // A leading symbol is the icon, not part of the message.
        final String glyph = leadingGlyph(text);
        final String shown = glyph != null ? text.substring(glyph.length()).trim() : text;
        final String body = shown.isEmpty() ? "Event " + seq : shown;
        final String label = STATUS_LABELS[status];
        final String title = (label != null ? label + " · " : "") + "Beacon " + txId;
        final Notification.Builder b = new Notification.Builder(context, CHANNEL_IDS[status]);
        if (glyph != null) {
            final Bitmap bitmap = renderGlyph(context, glyph,
                    STATUS_COLORS[status] != 0 ? STATUS_COLORS[status] : GLYPH_PLAIN_COLOR);
            b.setSmallIcon(Icon.createWithBitmap(bitmap));
            b.setLargeIcon(bitmap);
        } else {
            b.setSmallIcon(STATUS_ICONS[status]);
        }
        final Notification n = b
                .setColor(STATUS_COLORS[status])
                .setContentTitle(title)
                .setContentText(body)
                .setStyle(new Notification.BigTextStyle().bigText(body)) // 51 bytes wraps
                .setShowWhen(true)
                .setAutoCancel(true)
                .build();
        // One slot per event, so events stack as a history rather than replacing each
        // other. (A seq reused after the de-dupe window overwrites its old self.)
        nm.notify(TAG, (txId << 8) | seq, n);
    }

    /**
     * The first grapheme cluster of {@code text} if it is a symbol (general category So:
     * emoji, dingbats, arrows-with-meaning, Noto Sans Symbols fare), else null. Cluster
     * rather than code point so VS16, skin tones and ZWJ sequences come along whole.
     */
    private static String leadingGlyph(final String text) {
        if (text.isEmpty() || Character.getType(text.codePointAt(0)) != Character.OTHER_SYMBOL) {
            return null;
        }
        final BreakIterator it = BreakIterator.getCharacterInstance();
        it.setText(text);
        final int end = it.next();
        return end == BreakIterator.DONE ? null : text.substring(0, end);
    }

    /**
     * Draws the glyph with the system fonts. As a small icon SystemUI keeps only the
     * alpha channel, so a colour emoji shows as its silhouette in the status bar; as the
     * large icon the same bitmap keeps its colours. {@code color} only matters for
     * monochrome symbols, which would otherwise be white on a light shade.
     */
    private static Bitmap renderGlyph(final Context context, final String glyph,
            final int color) {
        final int size = Math.round(GLYPH_DP * context.getResources().getDisplayMetrics().density);
        final Bitmap bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
        final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(color);
        paint.setTextAlign(Paint.Align.CENTER);
        paint.setTextSize(size * 0.8f);
        final Paint.FontMetrics fm = paint.getFontMetrics();
        new Canvas(bitmap).drawText(glyph, size / 2f, (size - fm.ascent - fm.descent) / 2f, paint);
        return bitmap;
    }
}
