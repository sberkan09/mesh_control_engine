package org.meshcontrol.firebase;

import android.app.Activity;
import android.os.Bundle;

import com.google.firebase.FirebaseApp;
import com.google.firebase.analytics.FirebaseAnalytics;
import com.google.firebase.crashlytics.FirebaseCrashlytics;
import com.google.firebase.remoteconfig.FirebaseRemoteConfig;
import com.google.firebase.remoteconfig.FirebaseRemoteConfigValue;

import java.util.EnumMap;
import java.util.Map;

// Firebase Crashlytics, Analytics and Remote Config for mce_platform's live services vendor
// (platform/firebase.h, M-LIVEOPS steps 1 and 3). Stamped into the Gradle project of a game shipped with
// MCE_FIREBASE_ANDROID_CONFIG (cmake/mobile/android_app.py), driven through MceActivity's
// mceFirebase* methods over JNI.
//
// FirebaseApp starts itself before the activity (its content provider reads google-services.json),
// and Crashlytics' NDK handler is installed then, before the game library's own crash handler, which
// hands every native crash on to it (src/engine/core/src/crash.cpp). The manifest starts both SDKs
// with collection off; setCollection turns it on once the player consents
// (platform::firebase::Collection's values).
public final class MceFirebase {
    // Remote Config's answer (firebase.cpp): every value as text, then done.
    static native void nativeConfigValue(String key, String value);
    static native void nativeConfigDone(boolean ok);

    private final FirebaseAnalytics analytics;

    public MceFirebase(Activity activity) {
        if (FirebaseApp.getApps(activity).isEmpty()) FirebaseApp.initializeApp(activity);
        analytics = FirebaseAnalytics.getInstance(activity);
    }

    public void setCollection(int collection) {
        FirebaseCrashlytics crashlytics = FirebaseCrashlytics.getInstance();
        Map<FirebaseAnalytics.ConsentType, FirebaseAnalytics.ConsentStatus> consent =
            new EnumMap<>(FirebaseAnalytics.ConsentType.class);
        switch (collection) {
            case 1:  // Granted
                consent.put(FirebaseAnalytics.ConsentType.ANALYTICS_STORAGE, FirebaseAnalytics.ConsentStatus.GRANTED);
                analytics.setConsent(consent);
                analytics.setAnalyticsCollectionEnabled(true);
                crashlytics.setCrashlyticsCollectionEnabled(true);
                crashlytics.sendUnsentReports();
                break;
            case 2:  // Denied
                consent.put(FirebaseAnalytics.ConsentType.ANALYTICS_STORAGE, FirebaseAnalytics.ConsentStatus.DENIED);
                analytics.setConsent(consent);
                analytics.setAnalyticsCollectionEnabled(false);
                crashlytics.setCrashlyticsCollectionEnabled(false);
                crashlytics.deleteUnsentReports();
                break;
            default:  // Undecided: keep everything on the device
                analytics.setAnalyticsCollectionEnabled(false);
                crashlytics.setCrashlyticsCollectionEnabled(false);
                break;
        }
    }

    // kinds: platform::firebase::Param::Kind (0 int, 1 float, 2 string).
    public void logEvent(String name, String[] keys, int[] kinds, long[] ints, double[] floats, String[] strings) {
        Bundle params = new Bundle();
        for (int i = 0; i < keys.length; ++i) {
            switch (kinds[i]) {
                case 0: params.putLong(keys[i], ints[i]); break;
                case 1: params.putDouble(keys[i], floats[i]); break;
                default: params.putString(keys[i], strings[i]); break;
            }
        }
        analytics.logEvent(name, params);
    }

    // Fetches and activates, then hands every value the service sent to native code. The engine
    // decides when fetched values take effect (liveops::RemoteConfigService), so activating here
    // only moves them into the SDK's own active set.
    public void fetchRemoteConfig() {
        FirebaseRemoteConfig config = FirebaseRemoteConfig.getInstance();
        config.fetchAndActivate().addOnCompleteListener(task -> {
            if (!task.isSuccessful()) {
                nativeConfigDone(false);
                return;
            }
            for (Map.Entry<String, FirebaseRemoteConfigValue> e : config.getAll().entrySet()) {
                if (e.getValue().getSource() == FirebaseRemoteConfig.VALUE_SOURCE_REMOTE)
                    nativeConfigValue(e.getKey(), e.getValue().asString());
            }
            nativeConfigDone(true);
        });
    }
}
