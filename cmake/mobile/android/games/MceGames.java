package org.meshcontrol.games;

import android.app.Activity;
import android.content.Intent;

import com.google.android.gms.games.AchievementsClient;
import com.google.android.gms.games.GamesSignInClient;
import com.google.android.gms.games.LeaderboardsClient;
import com.google.android.gms.games.PlayGames;
import com.google.android.gms.games.PlayGamesSdk;
import com.google.android.gms.games.SnapshotsClient;
import com.google.android.gms.games.snapshot.Snapshot;
import com.google.android.gms.games.snapshot.SnapshotMetadataChange;
import com.google.android.gms.tasks.Task;

import java.io.IOException;

// Play Games Services v2 for mce_platform's game services (platform/game_services.h, M-LIVEOPS
// step 2). Stamped into the Gradle project of a game with a .mcservices when MCE_PLAY_GAMES_APP_ID
// names its Play Games project (cmake/mobile/android_app.py), driven through MceActivity's mceGames*
// methods over JNI. Every call is answered exactly once through nativeAnswer with the tag it carried.
public final class MceGames {
    // platform::game_services::AnswerKind
    static final int SIGNED_IN = 0;
    static final int ACHIEVEMENT = 1;
    static final int SCORE = 2;
    static final int BLOB_READ = 3;
    static final int BLOB_WRITTEN = 4;

    static native void nativeAnswer(int kind, boolean ok, boolean missing, int tag, byte[] bytes);

    private static final int SHOW_REQUEST = 0x4D47;

    private final Activity activity;

    public MceGames(Activity activity) {
        this.activity = activity;
        PlayGamesSdk.initialize(activity);
    }

    public void signIn(boolean interactive) {
        GamesSignInClient client = PlayGames.getGamesSignInClient(activity);
        Task<com.google.android.gms.games.AuthenticationResult> task = interactive ? client.signIn() : client.isAuthenticated();
        task.addOnCompleteListener(t -> nativeAnswer(SIGNED_IN,
                t.isSuccessful() && t.getResult() != null && t.getResult().isAuthenticated(), false, 0, null));
    }

    // steps of total: one-shot achievements (total 1) are unlocked, the others set to their steps.
    public void achievement(String id, int steps, int total, int tag) {
        AchievementsClient client = PlayGames.getAchievementsClient(activity);
        Task<?> task = total <= 1 ? client.unlockImmediate(id) : client.setStepsImmediate(id, steps);
        task.addOnCompleteListener(t -> nativeAnswer(ACHIEVEMENT, t.isSuccessful(), false, tag, null));
    }

    public void score(String id, long score, int tag) {
        LeaderboardsClient client = PlayGames.getLeaderboardsClient(activity);
        client.submitScoreImmediate(id, score)
                .addOnCompleteListener(t -> nativeAnswer(SCORE, t.isSuccessful(), false, tag, null));
    }

    // A snapshot that has never been written reads empty: that is "missing".
    public void read(String name, int tag) {
        SnapshotsClient client = PlayGames.getSnapshotsClient(activity);
        client.open(name, true, SnapshotsClient.RESOLUTION_POLICY_MOST_RECENTLY_MODIFIED).addOnCompleteListener(t -> {
            if (!t.isSuccessful() || t.getResult() == null || t.getResult().isConflict()) {
                nativeAnswer(BLOB_READ, false, false, tag, null);
                return;
            }
            Snapshot snapshot = t.getResult().getData();
            byte[] bytes;
            try {
                bytes = snapshot.getSnapshotContents().readFully();
            } catch (IOException e) {
                client.discardAndClose(snapshot);
                nativeAnswer(BLOB_READ, false, false, tag, null);
                return;
            }
            client.discardAndClose(snapshot);
            boolean missing = bytes == null || bytes.length == 0;
            nativeAnswer(BLOB_READ, true, missing, tag, missing ? null : bytes);
        });
    }

    public void write(String name, byte[] bytes, int tag) {
        SnapshotsClient client = PlayGames.getSnapshotsClient(activity);
        client.open(name, true, SnapshotsClient.RESOLUTION_POLICY_MOST_RECENTLY_MODIFIED).addOnCompleteListener(t -> {
            if (!t.isSuccessful() || t.getResult() == null || t.getResult().isConflict()) {
                nativeAnswer(BLOB_WRITTEN, false, false, tag, null);
                return;
            }
            Snapshot snapshot = t.getResult().getData();
            if (!snapshot.getSnapshotContents().writeBytes(bytes)) {
                client.discardAndClose(snapshot);
                nativeAnswer(BLOB_WRITTEN, false, false, tag, null);
                return;
            }
            client.commitAndClose(snapshot, SnapshotMetadataChange.EMPTY_CHANGE)
                    .addOnCompleteListener(c -> nativeAnswer(BLOB_WRITTEN, c.isSuccessful(), false, tag, null));
        });
    }

    public void showAchievements() {
        PlayGames.getAchievementsClient(activity).getAchievementsIntent().addOnSuccessListener(this::show);
    }

    public void showLeaderboard(String id) {
        PlayGames.getLeaderboardsClient(activity).getLeaderboardIntent(id).addOnSuccessListener(this::show);
    }

    private void show(Intent intent) {
        activity.runOnUiThread(() -> activity.startActivityForResult(intent, SHOW_REQUEST));
    }
}
