#include "mce/platform/firebase.h"

#include <SDL3/SDL.h>

#if defined(SDL_PLATFORM_ANDROID)
#include "android_jni.h"
#elif defined(SDL_PLATFORM_IOS) && defined(MCE_FIREBASE)
#include "mce/fixed_string.h"
#endif

namespace mce::platform::firebase {

const char* backend_name(Backend b) { return b == Backend::Firebase ? "firebase" : "none"; }

// ---- Android: MceFirebase through the stamped MceActivity ----------------------------------------
#if defined(SDL_PLATFORM_ANDROID)
namespace {

using detail::Jni;

bool g_started = false;

bool firebase_available() {
    static const bool has = [] {
        Jni j;
        return j.method("mceFirebaseStart", "()V") != nullptr;
    }();
    return has;
}

}  // namespace

Backend backend() { return firebase_available() ? Backend::Firebase : Backend::None; }

bool start() {
    if (!firebase_available()) return false;
    Jni j;
    j.env->CallVoidMethod(j.activity, j.method("mceFirebaseStart", "()V"));
    g_started = j.ok("mceFirebaseStart");
    return g_started;
}

void set_collection(Collection c) {
    if (!g_started) return;
    Jni j;
    j.env->CallVoidMethod(j.activity, j.method("mceFirebaseCollection", "(I)V"), static_cast<jint>(c));
    (void)j.ok("mceFirebaseCollection");
}

void log_event(StringView name, Span<const Param> params) {
    if (!g_started) return;
    Jni j;
    JNIEnv* env = j.env;
    const jsize n = static_cast<jsize>(params.size());
    jclass string_class = env->FindClass("java/lang/String");
    jobjectArray keys = env->NewObjectArray(n, string_class, nullptr);
    jobjectArray strings = env->NewObjectArray(n, string_class, nullptr);
    jintArray kinds = env->NewIntArray(n);
    jlongArray ints = env->NewLongArray(n);
    jdoubleArray floats = env->NewDoubleArray(n);
    for (jsize i = 0; i < n; ++i) {
        const Param& p = params[static_cast<usize>(i)];
        jstring k = j.str(p.key);
        env->SetObjectArrayElement(keys, i, k);
        env->DeleteLocalRef(k);
        if (p.kind == Param::Kind::String) {
            jstring s = j.str(p.s);
            env->SetObjectArrayElement(strings, i, s);
            env->DeleteLocalRef(s);
        }
        const jint kind = static_cast<jint>(p.kind);
        const jlong iv = static_cast<jlong>(p.i);
        const jdouble fv = p.f;
        env->SetIntArrayRegion(kinds, i, 1, &kind);
        env->SetLongArrayRegion(ints, i, 1, &iv);
        env->SetDoubleArrayRegion(floats, i, 1, &fv);
    }
    jstring jname = j.str(name);
    env->CallVoidMethod(
        j.activity, j.method("mceFirebaseEvent", "(Ljava/lang/String;[Ljava/lang/String;[I[J[D[Ljava/lang/String;)V"),
        jname, keys, kinds, ints, floats, strings);
    (void)j.ok("mceFirebaseEvent");
    env->DeleteLocalRef(jname);
    env->DeleteLocalRef(floats);
    env->DeleteLocalRef(ints);
    env->DeleteLocalRef(kinds);
    env->DeleteLocalRef(strings);
    env->DeleteLocalRef(keys);
    env->DeleteLocalRef(string_class);
}

// ---- iOS: Crashlytics + Analytics (firebase_ios.swift) ---------------------------------------------
#elif defined(SDL_PLATFORM_IOS) && defined(MCE_FIREBASE)

// The Swift side's entry points (@_cdecl in firebase_ios.swift).
extern "C" {
void mce_firebase_start();
void mce_firebase_collection(int collection);
void mce_firebase_event_begin(const char* name);
void mce_firebase_event_int(const char* key, i64 v);
void mce_firebase_event_float(const char* key, f64 v);
void mce_firebase_event_string(const char* key, const char* v);
void mce_firebase_event_end();
}

namespace {
bool g_started = false;
}  // namespace

Backend backend() { return Backend::Firebase; }

bool start() {
    mce_firebase_start();
    g_started = true;
    return true;
}

void set_collection(Collection c) {
    if (g_started) mce_firebase_collection(static_cast<int>(c));
}

void log_event(StringView name, Span<const Param> params) {
    if (!g_started) return;
    const FixedString<64> n(name);
    mce_firebase_event_begin(n.c_str());
    for (const Param& p : params) {
        const FixedString<64> k(p.key);
        switch (p.kind) {
            case Param::Kind::Int:
                mce_firebase_event_int(k.c_str(), p.i);
                break;
            case Param::Kind::Float:
                mce_firebase_event_float(k.c_str(), p.f);
                break;
            case Param::Kind::String: {
                const FixedString<128> s(p.s);
                mce_firebase_event_string(k.c_str(), s.c_str());
                break;
            }
        }
    }
    mce_firebase_event_end();
}

// ---- everywhere else: no vendor ----------------------------------------------------------------------
#else

Backend backend() { return Backend::None; }
bool start() { return false; }
void set_collection(Collection) {}
void log_event(StringView, Span<const Param>) {}

#endif

}  // namespace mce::platform::firebase
