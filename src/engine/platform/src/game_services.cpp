#include "mce/platform/game_services.h"

#include <SDL3/SDL.h>

#include "mce/fixed_string.h"
#include "mce/log.h"

#if defined(SDL_PLATFORM_ANDROID)
#include "android_jni.h"
#endif

namespace mce::platform::game_services {
namespace {

struct State {
    AnswerFn fn = nullptr;
    void* user = nullptr;
    bool started = false;
};
State g_games;

[[maybe_unused]] void answer(AnswerKind kind, bool ok, u16 tag, Span<const byte> bytes = {}, bool missing = false) {
    if (!g_games.fn) return;
    Answer a;
    a.kind = kind;
    a.ok = ok;
    a.tag = tag;
    a.bytes = bytes;
    a.missing = missing;
    g_games.fn(g_games.user, a);
}

}  // namespace

const char* backend_name(Backend b) {
    switch (b) {
        case Backend::None:
            return "none";
        case Backend::GameCenter:
            return "game_center";
        case Backend::PlayGames:
            return "play_games";
    }
    return "none";
}

// ---- Android: Play Games Services v2 through the stamped MceActivity --------------------------------
#if defined(SDL_PLATFORM_ANDROID)
namespace {

using detail::Jni;

bool play_games_available() {
    static const bool has = [] {
        Jni j;
        return j.method("mceGamesStart", "()V") != nullptr;
    }();
    return has;
}

void call_tagged(const char* method, StringView id, u16 tag, AnswerKind refuse) {
    Jni j;
    jstring s = j.str(id);
    j.env->CallVoidMethod(j.activity, j.method(method, "(Ljava/lang/String;I)V"), s, static_cast<jint>(tag));
    const bool ok = j.ok(method);
    j.env->DeleteLocalRef(s);
    if (!ok) answer(refuse, false, tag);
}

}  // namespace

Backend backend() { return play_games_available() ? Backend::PlayGames : Backend::None; }

bool start(AnswerFn fn, void* user) {
    g_games.fn = fn;
    g_games.user = user;
    if (!play_games_available()) return false;
    Jni j;
    j.env->CallVoidMethod(j.activity, j.method("mceGamesStart", "()V"));
    g_games.started = j.ok("mceGamesStart");
    return g_games.started;
}

void stop() { g_games = {}; }

void sign_in(bool interactive) {
    if (!g_games.started) return answer(AnswerKind::SignedIn, false, 0);
    Jni j;
    j.env->CallVoidMethod(j.activity, j.method("mceGamesSignIn", "(Z)V"), static_cast<jboolean>(interactive));
    if (!j.ok("mceGamesSignIn")) answer(AnswerKind::SignedIn, false, 0);
}

void report_achievement(StringView id, u32 steps, u32 total, u16 tag) {
    if (!g_games.started) return answer(AnswerKind::Achievement, false, tag);
    Jni j;
    jstring s = j.str(id);
    j.env->CallVoidMethod(j.activity, j.method("mceGamesAchievement", "(Ljava/lang/String;III)V"), s,
                          static_cast<jint>(steps), static_cast<jint>(total), static_cast<jint>(tag));
    const bool ok = j.ok("mceGamesAchievement");
    j.env->DeleteLocalRef(s);
    if (!ok) answer(AnswerKind::Achievement, false, tag);
}

void submit_score(StringView id, i64 score, u16 tag) {
    if (!g_games.started) return answer(AnswerKind::Score, false, tag);
    Jni j;
    jstring s = j.str(id);
    j.env->CallVoidMethod(j.activity, j.method("mceGamesScore", "(Ljava/lang/String;JI)V"), s,
                          static_cast<jlong>(score), static_cast<jint>(tag));
    const bool ok = j.ok("mceGamesScore");
    j.env->DeleteLocalRef(s);
    if (!ok) answer(AnswerKind::Score, false, tag);
}

void read_blob(StringView name, u16 tag) {
    if (!g_games.started) return answer(AnswerKind::BlobRead, false, tag);
    call_tagged("mceGamesRead", name, tag, AnswerKind::BlobRead);
}

void write_blob(StringView name, Span<const byte> bytes, u16 tag) {
    if (!g_games.started) return answer(AnswerKind::BlobWritten, false, tag);
    Jni j;
    jstring s = j.str(name);
    jbyteArray data = j.env->NewByteArray(static_cast<jsize>(bytes.size()));
    j.env->SetByteArrayRegion(data, 0, static_cast<jsize>(bytes.size()), reinterpret_cast<const jbyte*>(bytes.data()));
    j.env->CallVoidMethod(j.activity, j.method("mceGamesWrite", "(Ljava/lang/String;[BI)V"), s, data,
                          static_cast<jint>(tag));
    const bool ok = j.ok("mceGamesWrite");
    j.env->DeleteLocalRef(data);
    j.env->DeleteLocalRef(s);
    if (!ok) answer(AnswerKind::BlobWritten, false, tag);
}

void show_achievements() {
    if (!g_games.started) return;
    Jni j;
    j.env->CallVoidMethod(j.activity, j.method("mceGamesShowAchievements", "()V"));
    (void)j.ok("mceGamesShowAchievements");
}

void show_leaderboard(StringView id) {
    if (!g_games.started) return;
    Jni j;
    jstring s = j.str(id);
    j.env->CallVoidMethod(j.activity, j.method("mceGamesShowLeaderboard", "(Ljava/lang/String;)V"), s);
    (void)j.ok("mceGamesShowLeaderboard");
    j.env->DeleteLocalRef(s);
}

}  // namespace mce::platform::game_services

// MceGames.nativeAnswer: called on Play Games' threads with one answer (kinds: AnswerKind's values).
extern "C" JNIEXPORT void JNICALL Java_org_meshcontrol_games_MceGames_nativeAnswer(JNIEnv* env, jclass, jint kind,
                                                                                    jboolean ok, jboolean missing,
                                                                                    jint tag, jbyteArray bytes) {
    using mce::platform::game_services::AnswerKind;
    if (kind < 0 || kind > static_cast<jint>(AnswerKind::BlobWritten)) return;
    jbyte* data = bytes ? env->GetByteArrayElements(bytes, nullptr) : nullptr;
    const jsize n = bytes ? env->GetArrayLength(bytes) : 0;
    mce::platform::game_services::answer(
        static_cast<AnswerKind>(kind), ok != JNI_FALSE, static_cast<mce::u16>(tag),
        mce::Span<const mce::byte>(reinterpret_cast<const mce::byte*>(data), static_cast<mce::usize>(n)),
        missing != JNI_FALSE);
    if (data) env->ReleaseByteArrayElements(bytes, data, JNI_ABORT);
}

namespace mce::platform::game_services {

// ---- iOS: GameKit (game_services_gamekit.swift) ---------------------------------------------------
#elif defined(SDL_PLATFORM_IOS) && defined(MCE_GAMEKIT)

// The Swift side's entry points (@_cdecl in game_services_gamekit.swift).
extern "C" {
void mce_gamekit_start();
void mce_gamekit_sign_in(int interactive);
void mce_gamekit_achievement(const char* id, double percent, u16 tag);
void mce_gamekit_score(const char* id, i64 score, u16 tag);
void mce_gamekit_read(const char* name, u16 tag);
void mce_gamekit_write(const char* name, const byte* bytes, usize size, u16 tag);
void mce_gamekit_show_achievements();
void mce_gamekit_show_leaderboard(const char* id);
}

// The Swift side's answers (@_silgen_name there), on GameKit's threads.
extern "C" void mce_gamekit_answer(int kind, int ok, int missing, u16 tag, const byte* bytes, usize size) {
    if (kind < 0 || kind > static_cast<int>(AnswerKind::BlobWritten)) return;
    answer(static_cast<AnswerKind>(kind), ok != 0, tag, Span<const byte>(bytes, bytes ? size : 0), missing != 0);
}

Backend backend() { return Backend::GameCenter; }

bool start(AnswerFn fn, void* user) {
    g_games.fn = fn;
    g_games.user = user;
    g_games.started = true;
    mce_gamekit_start();
    return true;
}

void stop() { g_games = {}; }

void sign_in(bool interactive) {
    if (!g_games.started) return answer(AnswerKind::SignedIn, false, 0);
    mce_gamekit_sign_in(interactive ? 1 : 0);
}

void report_achievement(StringView id, u32 steps, u32 total, u16 tag) {
    if (!g_games.started) return answer(AnswerKind::Achievement, false, tag);
    const FixedString<128> z(id);
    const double percent = total == 0 ? 100.0 : 100.0 * static_cast<double>(steps) / static_cast<double>(total);
    mce_gamekit_achievement(z.c_str(), percent, tag);
}

void submit_score(StringView id, i64 score, u16 tag) {
    if (!g_games.started) return answer(AnswerKind::Score, false, tag);
    const FixedString<128> z(id);
    mce_gamekit_score(z.c_str(), score, tag);
}

void read_blob(StringView name, u16 tag) {
    if (!g_games.started) return answer(AnswerKind::BlobRead, false, tag);
    const FixedString<64> z(name);
    mce_gamekit_read(z.c_str(), tag);
}

void write_blob(StringView name, Span<const byte> bytes, u16 tag) {
    if (!g_games.started) return answer(AnswerKind::BlobWritten, false, tag);
    const FixedString<64> z(name);
    mce_gamekit_write(z.c_str(), bytes.data(), bytes.size(), tag);
}

void show_achievements() {
    if (g_games.started) mce_gamekit_show_achievements();
}

void show_leaderboard(StringView id) {
    if (!g_games.started) return;
    const FixedString<128> z(id);
    mce_gamekit_show_leaderboard(z.c_str());
}

// ---- everywhere else: no game services ------------------------------------------------------------
#else

Backend backend() { return Backend::None; }

bool start(AnswerFn fn, void* user) {
    g_games.fn = fn;
    g_games.user = user;
    return false;
}

void stop() { g_games = {}; }
void sign_in(bool) { answer(AnswerKind::SignedIn, false, 0); }
void report_achievement(StringView, u32, u32, u16 tag) { answer(AnswerKind::Achievement, false, tag); }
void submit_score(StringView, i64, u16 tag) { answer(AnswerKind::Score, false, tag); }
void read_blob(StringView, u16 tag) { answer(AnswerKind::BlobRead, false, tag); }
void write_blob(StringView, Span<const byte>, u16 tag) { answer(AnswerKind::BlobWritten, false, tag); }
void show_achievements() {}
void show_leaderboard(StringView) {}

#endif

}  // namespace mce::platform::game_services
