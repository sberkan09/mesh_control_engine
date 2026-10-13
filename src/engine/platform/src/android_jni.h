#pragma once
// JNI helpers for the Java bridges stamped into the game's activity (store.cpp, ads.cpp). Android only.

#include <SDL3/SDL.h>
#include <jni.h>

#include "mce/fixed_string.h"
#include "mce/log.h"

namespace mce::platform::detail {

// The activity's bridge methods (MceActivity.java.in, filled in per game by android_app.py). The
// game library is loaded by the activity's class loader, so methods are looked up on the activity
// object instead of by class name.
struct Jni {
    JNIEnv* env = nullptr;
    jobject activity = nullptr;
    jclass cls = nullptr;

    Jni() {
        env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
        if (!env) return;
        activity = static_cast<jobject>(SDL_GetAndroidActivity());
        if (activity) cls = env->GetObjectClass(activity);
    }
    ~Jni() {
        if (!env) return;
        if (cls) env->DeleteLocalRef(cls);
        if (activity) env->DeleteLocalRef(activity);
    }
    Jni(const Jni&) = delete;
    Jni& operator=(const Jni&) = delete;

    jmethodID method(const char* name, const char* sig) const {
        if (!cls) return nullptr;
        jmethodID m = env->GetMethodID(cls, name, sig);
        if (!m && env->ExceptionCheck()) env->ExceptionClear();  // NoSuchMethodError: no such bridge
        return m;
    }
    jstring str(StringView s) const {
        FixedString<512> z(s);  // Play purchase tokens run past 128 characters
        return env->NewStringUTF(z.c_str());
    }
    // A Java exception from the bridge is logged and cleared; the call counts as failed.
    bool ok(const char* what) const {
        if (!env->ExceptionCheck()) return true;
        env->ExceptionDescribe();
        env->ExceptionClear();
        MCE_LOG_ERROR("platform: {} threw", what);
        return false;
    }
};

// A Java string's UTF-8 for the duration of one call.
struct JStr {
    JNIEnv* env;
    jstring s;
    const char* chars;
    JStr(JNIEnv* e, jstring str) : env(e), s(str), chars(str ? e->GetStringUTFChars(str, nullptr) : nullptr) {}
    ~JStr() {
        if (chars) env->ReleaseStringUTFChars(s, chars);
    }
    JStr(const JStr&) = delete;
    JStr& operator=(const JStr&) = delete;
    StringView view() const { return chars ? StringView(chars) : StringView(); }
};

}  // namespace mce::platform::detail
