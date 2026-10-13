package org.meshcontrol.ads;

import android.app.Activity;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.preference.PreferenceManager;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Gravity;
import android.widget.FrameLayout;

import com.google.ads.mediation.admob.AdMobAdapter;
import com.google.android.gms.ads.AdError;
import com.google.android.gms.ads.AdListener;
import com.google.android.gms.ads.AdRequest;
import com.google.android.gms.ads.AdSize;
import com.google.android.gms.ads.AdView;
import com.google.android.gms.ads.FullScreenContentCallback;
import com.google.android.gms.ads.LoadAdError;
import com.google.android.gms.ads.MobileAds;
import com.google.android.gms.ads.interstitial.InterstitialAd;
import com.google.android.gms.ads.interstitial.InterstitialAdLoadCallback;
import com.google.android.gms.ads.rewarded.RewardedAd;
import com.google.android.gms.ads.rewarded.RewardedAdLoadCallback;
import com.google.android.ump.ConsentDebugSettings;
import com.google.android.ump.ConsentInformation;
import com.google.android.ump.ConsentRequestParameters;
import com.google.android.ump.UserMessagingPlatform;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;

// Google Mobile Ads and UMP consent for mce_platform's ads (platform/ads.h, M-MONETIZE step 4).
// Stamped into the Gradle project of a game whose store catalog has ad placements
// (cmake/mobile/android_app.py), driven through MceActivity's mceAds* methods over JNI. Every call
// hops to the UI thread; answers go back through nativeAnswer with platform::ads::AnswerKind's
// values and the consent outcome through nativeConsent (platform::ads::Consent's values).
//
// Consent first: UMP's form where the law needs one, then MobileAds.initialize; requests made
// before that wait. Without consent to personalized ads, requests carry npa=1.
public final class MceAds {
    static final int LOADED = 0;
    static final int NO_FILL = 1;
    static final int SHOWN = 2;
    static final int REWARDED = 3;
    static final int CLOSED = 4;
    static final int FAILED = 5;

    static final int CONSENT_PERSONALIZED = 1;
    static final int CONSENT_NON_PERSONALIZED = 2;

    static final int FORMAT_REWARDED = 0;
    static final int FORMAT_INTERSTITIAL = 1;

    static native void nativeAnswer(int kind, int request, String unit, int amount);
    static native void nativeConsent(int consent);

    private static final String TAG = "MceAds";

    private final Activity activity;
    private ConsentInformation consent;
    private boolean ready;            // consent gathered and the SDK initialized
    private boolean gathering;
    private boolean personalized;
    private final ArrayList<Runnable> waiting = new ArrayList<>();
    private final ArrayList<Runnable> refusals = new ArrayList<>();
    private final Map<String, InterstitialAd> interstitials = new HashMap<>();
    private final Map<String, RewardedAd> rewarded = new HashMap<>();
    private AdView banner;

    public MceAds(Activity activity) {
        this.activity = activity;
    }

    // ---- calls from MceActivity (SDL's thread) ---------------------------------------------------

    public void start(boolean testDevice) {
        activity.runOnUiThread(() -> {
            if (gathering || ready) return;
            gathering = true;
            consent = UserMessagingPlatform.getConsentInformation(activity);
            ConsentRequestParameters.Builder params = new ConsentRequestParameters.Builder();
            if (testDevice) {
                params.setConsentDebugSettings(new ConsentDebugSettings.Builder(activity)
                        .setDebugGeography(ConsentDebugSettings.DebugGeography.DEBUG_GEOGRAPHY_EEA)
                        .build());
            }
            consent.requestConsentInfoUpdate(activity, params.build(),
                    () -> UserMessagingPlatform.loadAndShowConsentFormIfRequired(activity, formError -> {
                        if (formError != null) Log.w(TAG, "consent form: " + formError.getMessage());
                        consentDone();
                    }),
                    requestError -> {
                        Log.w(TAG, "consent update: " + requestError.getMessage());
                        consentDone();  // an earlier session's answer may still allow requests
                    });
        });
    }

    public void stop() {
        activity.runOnUiThread(() -> {
            removeBanner();
            interstitials.clear();
            rewarded.clear();
            waiting.clear();
            refusals.clear();
        });
    }

    public boolean privacyRequired() {
        ConsentInformation c = consent;
        return c != null && c.getPrivacyOptionsRequirementStatus()
                == ConsentInformation.PrivacyOptionsRequirementStatus.REQUIRED;
    }

    public void showPrivacy() {
        activity.runOnUiThread(() -> UserMessagingPlatform.showPrivacyOptionsForm(activity, error -> {
            if (error != null) Log.w(TAG, "privacy options: " + error.getMessage());
            personalized = readPersonalized();
            nativeConsent(personalized ? CONSENT_PERSONALIZED : CONSENT_NON_PERSONALIZED);
        }));
    }

    public void load(int format, String unit, int request) {
        activity.runOnUiThread(() -> whenReady(() -> {
            if (format == FORMAT_INTERSTITIAL) {
                InterstitialAd.load(activity, unit, adRequest(), new InterstitialAdLoadCallback() {
                    @Override
                    public void onAdLoaded(InterstitialAd ad) {
                        interstitials.put(unit, ad);
                        nativeAnswer(LOADED, request, unit, 0);
                    }
                    @Override
                    public void onAdFailedToLoad(LoadAdError e) {
                        loadFailed(e, unit, request);
                    }
                });
            } else if (format == FORMAT_REWARDED) {
                RewardedAd.load(activity, unit, adRequest(), new RewardedAdLoadCallback() {
                    @Override
                    public void onAdLoaded(RewardedAd ad) {
                        rewarded.put(unit, ad);
                        nativeAnswer(LOADED, request, unit, 0);
                    }
                    @Override
                    public void onAdFailedToLoad(LoadAdError e) {
                        loadFailed(e, unit, request);
                    }
                });
            } else {
                nativeAnswer(FAILED, request, unit, 0);
            }
        }, () -> nativeAnswer(NO_FILL, request, unit, 0)));
    }

    public void show(int format, String unit, int request) {
        activity.runOnUiThread(() -> {
            FullScreenContentCallback callback = new FullScreenContentCallback() {
                @Override
                public void onAdShowedFullScreenContent() {
                    nativeAnswer(SHOWN, request, unit, 0);
                }
                @Override
                public void onAdDismissedFullScreenContent() {
                    nativeAnswer(CLOSED, request, unit, 0);
                }
                @Override
                public void onAdFailedToShowFullScreenContent(AdError e) {
                    Log.w(TAG, "show " + unit + ": " + e.getMessage());
                    nativeAnswer(FAILED, request, unit, 0);
                }
            };
            if (format == FORMAT_INTERSTITIAL && interstitials.containsKey(unit)) {
                InterstitialAd ad = interstitials.remove(unit);  // an ad shows once
                ad.setFullScreenContentCallback(callback);
                ad.show(activity);
            } else if (format == FORMAT_REWARDED && rewarded.containsKey(unit)) {
                RewardedAd ad = rewarded.remove(unit);
                ad.setFullScreenContentCallback(callback);
                ad.show(activity, item -> nativeAnswer(REWARDED, request, unit, item.getAmount()));
            } else {
                nativeAnswer(FAILED, request, unit, 0);  // not loaded
            }
        });
    }

    // Insets are device pixels: the banner sits inside the safe area, centered.
    public void banner(String unit, int request, boolean top, int left, int topInset, int right, int bottom) {
        activity.runOnUiThread(() -> whenReady(() -> {
            removeBanner();
            DisplayMetrics dm = activity.getResources().getDisplayMetrics();
            int widthDp = (int) ((dm.widthPixels - left - right) / dm.density);
            AdView view = new AdView(activity);
            view.setAdUnitId(unit);
            view.setAdSize(AdSize.getCurrentOrientationAnchoredAdaptiveBannerAdSize(activity, widthDp));
            view.setAdListener(new AdListener() {
                @Override
                public void onAdLoaded() {
                    nativeAnswer(LOADED, request, unit, 0);
                }
                @Override
                public void onAdFailedToLoad(LoadAdError e) {
                    loadFailed(e, unit, request);
                }
            });
            FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT,
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    (top ? Gravity.TOP : Gravity.BOTTOM) | Gravity.CENTER_HORIZONTAL);
            lp.setMargins(left, topInset, right, bottom);
            activity.addContentView(view, lp);
            banner = view;
            view.loadAd(adRequest());
        }, () -> nativeAnswer(NO_FILL, request, unit, 0)));
    }

    public void hideBanner() {
        activity.runOnUiThread(this::removeBanner);
    }

    // ---- helpers (UI thread) -----------------------------------------------------------------------

    private void consentDone() {
        gathering = false;
        personalized = readPersonalized();
        nativeConsent(personalized ? CONSENT_PERSONALIZED : CONSENT_NON_PERSONALIZED);
        if (!consent.canRequestAds()) {
            Log.w(TAG, "consent does not allow ad requests");
            failWaiting();
            return;
        }
        MobileAds.initialize(activity, status -> {
            ready = true;
            ArrayList<Runnable> run = new ArrayList<>(waiting);
            waiting.clear();
            refusals.clear();
            for (Runnable r : run) r.run();
        });
    }

    private void failWaiting() {
        ArrayList<Runnable> run = new ArrayList<>(refusals);
        waiting.clear();
        refusals.clear();
        for (Runnable r : run) r.run();
    }

    private void whenReady(Runnable call, Runnable refuse) {
        if (ready) {
            call.run();
        } else if (gathering || consent == null) {
            waiting.add(call);
            refusals.add(refuse);
        } else {
            refuse.run();  // consent finished without allowing requests
        }
    }

    // Personalized ads need IAB TCF purposes 1, 3 and 4 where UMP asked; elsewhere consent is not required.
    private boolean readPersonalized() {
        if (consent == null) return false;
        if (consent.getConsentStatus() == ConsentInformation.ConsentStatus.NOT_REQUIRED) return true;
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(activity);
        String purposes = prefs.getString("IABTCF_PurposeConsents", "");
        return purposes.length() >= 4 && purposes.charAt(0) == '1' && purposes.charAt(2) == '1'
                && purposes.charAt(3) == '1';
    }

    private AdRequest adRequest() {
        AdRequest.Builder b = new AdRequest.Builder();
        if (!personalized) {
            Bundle extras = new Bundle();
            extras.putString("npa", "1");
            b.addNetworkExtrasBundle(AdMobAdapter.class, extras);
        }
        return b.build();
    }

    private void loadFailed(LoadAdError e, String unit, int request) {
        if (e.getCode() == AdRequest.ERROR_CODE_NO_FILL) {
            nativeAnswer(NO_FILL, request, unit, 0);
        } else {
            Log.w(TAG, "load " + unit + ": " + e.getMessage());
            nativeAnswer(FAILED, request, unit, 0);
        }
    }

    private void removeBanner() {
        if (banner == null) return;
        if (banner.getParent() instanceof android.view.ViewGroup)
            ((android.view.ViewGroup) banner.getParent()).removeView(banner);
        banner.destroy();
        banner = null;
    }
}
