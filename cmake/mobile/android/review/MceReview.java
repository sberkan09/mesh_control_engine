package org.meshcontrol.review;

// mce_platform's review prompt on Android (src/engine/platform/include/mce/platform/review.h):
// Play In-App Review, stamped into every game's Gradle project by cmake/mobile/android_app.py.
//
// MceActivity.mceReviewRequest() calls request() from the game's thread. Play decides whether the
// sheet shows (it keeps its own quota) and never says whether it did or what the player answered,
// so nothing goes back to native code.

import android.app.Activity;
import android.util.Log;

import com.google.android.play.core.review.ReviewInfo;
import com.google.android.play.core.review.ReviewManager;
import com.google.android.play.core.review.ReviewManagerFactory;

public final class MceReview {
    private static final String TAG = "MceReview";

    private final Activity activity;
    private final ReviewManager manager;

    public MceReview(Activity activity) {
        this.activity = activity;
        this.manager = ReviewManagerFactory.create(activity.getApplicationContext());
    }

    public void request() {
        activity.runOnUiThread(() -> manager.requestReviewFlow().addOnCompleteListener(task -> {
            if (!task.isSuccessful()) {
                Log.w(TAG, "requestReviewFlow failed", task.getException());
                return;
            }
            ReviewInfo info = task.getResult();
            manager.launchReviewFlow(activity, info)
                .addOnCompleteListener(done -> Log.i(TAG, "review flow finished"));
        }));
    }
}
