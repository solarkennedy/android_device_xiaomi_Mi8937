package org.lineageos.pepito.volumetile;

import android.app.StatusBarManager;
import android.media.AudioManager;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

public class VolumeTile extends TileService {
    @Override
    public void onStartListening() {
        super.onStartListening();
        Tile tile = getQsTile();
        if (tile != null) {
            tile.setState(Tile.STATE_INACTIVE);
            tile.updateTile();
        }
    }

    @Override
    public void onClick() {
        AudioManager am = (AudioManager) getSystemService(AUDIO_SERVICE);
        if (am != null) {
            am.adjustVolume(AudioManager.ADJUST_SAME, AudioManager.FLAG_SHOW_UI);
        }
        // Collapse only the shade. ACTION_CLOSE_SYSTEM_DIALOGS would (a) throw
        // SecurityException for targetSdk S+ without BROADCAST_CLOSE_SYSTEM_DIALOGS
        // and (b) dismiss the volume dialog we just requested.
        StatusBarManager sbm = getSystemService(StatusBarManager.class);
        if (sbm != null) {
            sbm.collapsePanels();
        }
    }
}
