package org.meshcontrol.haptics;

// mce_platform's haptics on Android (src/engine/platform/include/mce/platform/haptics.h): the
// vibrator's predefined effects, stamped into every game's Gradle project by
// cmake/mobile/android_app.py. MceActivity.mceHaptic(preset) calls play() from the game's thread with
// a platform::haptics::Preset value. The manifest already asks for VIBRATE; a device without a
// vibrator plays nothing.

import android.content.Context;
import android.os.Build;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;

public final class MceHaptics {
    // platform::haptics::Preset's values.
    private static final int SELECTION = 0, LIGHT = 1, MEDIUM = 2, HEAVY = 3, SUCCESS = 4, WARNING = 5, ERROR = 6;

    private final Vibrator vibrator;

    public MceHaptics(Context context) {
        Vibrator v;
        if (Build.VERSION.SDK_INT >= 31) {
            VibratorManager m = (VibratorManager) context.getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
            v = m != null ? m.getDefaultVibrator() : null;
        } else {
            v = (Vibrator) context.getSystemService(Context.VIBRATOR_SERVICE);
        }
        vibrator = v != null && v.hasVibrator() ? v : null;
    }

    static VibrationEffect effect(int preset) {
        switch (preset) {
            case SELECTION:
            case LIGHT: return VibrationEffect.createPredefined(VibrationEffect.EFFECT_TICK);
            case MEDIUM: return VibrationEffect.createPredefined(VibrationEffect.EFFECT_CLICK);
            case HEAVY: return VibrationEffect.createPredefined(VibrationEffect.EFFECT_HEAVY_CLICK);
            case SUCCESS: return VibrationEffect.createPredefined(VibrationEffect.EFFECT_DOUBLE_CLICK);
            case WARNING: return VibrationEffect.createWaveform(new long[] {0, 30, 80, 30}, new int[] {0, 160, 0, 160}, -1);
            case ERROR: return VibrationEffect.createWaveform(new long[] {0, 40, 60, 40, 60, 60}, new int[] {0, 255, 0, 255, 0, 255}, -1);
            default: return null;
        }
    }

    public void play(int preset) {
        if (vibrator == null) return;
        VibrationEffect e = effect(preset);
        if (e != null) vibrator.vibrate(e);
    }
}
