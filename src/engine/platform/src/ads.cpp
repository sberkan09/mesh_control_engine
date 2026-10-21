#include "mce/platform/ads.h"

#include <SDL3/SDL.h>

#include <atomic>

#include "mce/fixed_string.h"
#include "mce/log.h"

#if defined(SDL_PLATFORM_ANDROID)
#include "android_jni.h"
#endif

namespace mce::platform::ads {
namespace {

struct State {
    AnswerFn fn = nullptr;
    void* user = nullptr;
    bool started = false;
};
State g_ads;
std::atomic<u8> g_consent{static_cast<u8>(Consent::Unknown)};

void answer(AnswerKind kind, u32 request, StringView unit, u32 amount = 0) {
    if (!g_ads.fn) return;
    Answer a;
    a.kind = kind;
    a.request = request;
    a.unit = unit;
    a.amount = amount;
    g_ads.fn(g_ads.user, a);
}

// No network here: every request still gets its answer.
void refuse_load(StringView unit, u32 request) { answer(AnswerKind::NoFill, request, unit); }
void refuse_show(StringView unit, u32 request) { answer(AnswerKind::Failed, request, unit); }

}  // namespace

const char* backend_name(Backend b) { return b == Backend::GoogleMobileAds ? "admob" : "none"; }

StringView test_unit(Format f) {
    // developers.google.com/admob/{android,ios}/test-ads: the demo units, one per format.
#if defined(SDL_PLATFORM_IOS)
    switch (f) {
        case Format::Rewarded: return "ca-app-pub-3940256099942544/1712485313";
        case Format::Interstitial: return "ca-app-pub-3940256099942544/4411468910";
        case Format::Banner: return "ca-app-pub-3940256099942544/2435281174";
    }
#else
    switch (f) {
        case Format::Rewarded: return "ca-app-pub-3940256099942544/5224354917";
        case Format::Interstitial: return "ca-app-pub-3940256099942544/1033173712";
        case Format::Banner: return "ca-app-pub-3940256099942544/9214589741";
    }
#endif
    return {};
}

Consent consent() { return static_cast<Consent>(g_consent.load(std::memory_order_acquire)); }

// ---- Android: Google Mobile Ads + UMP through the stamped MceActivity ----------------------------
#if defined(SDL_PLATFORM_ANDROID)
namespace {

using detail::Jni;

bool gma_available() {
    static const bool has = [] {
        Jni j;
        return j.method("mceAdsStart", "(Z)V") != nullptr;
    }();
    return has;
}

}  // namespace

Backend backend() { return gma_available() ? Backend::GoogleMobileAds : Backend::None; }

bool start(AnswerFn fn, void* user, const Options& opt) {
    g_ads.fn = fn;
    g_ads.user = user;
    g_consent.store(static_cast<u8>(Consent::Unknown), std::memory_order_release);
    if (!gma_available()) return false;
    Jni j;
    j.env->CallVoidMethod(j.activity, j.method("mceAdsStart", "(Z)V"), static_cast<jboolean>(opt.test_device));
    g_ads.started = j.ok("mceAdsStart");
    return g_ads.started;
}

void stop() {
    if (g_ads.started) {
        Jni j;
        j.env->CallVoidMethod(j.activity, j.method("mceAdsStop", "()V"));
        (void)j.ok("mceAdsStop");
    }
    g_ads = {};
}

bool privacy_options_required() {
    if (!g_ads.started) return false;
    Jni j;
    const jboolean r = j.env->CallBooleanMethod(j.activity, j.method("mceAdsPrivacyRequired", "()Z"));
    return j.ok("mceAdsPrivacyRequired") && r;
}

void show_privacy_options() {
    if (!g_ads.started) return;
    Jni j;
    j.env->CallVoidMethod(j.activity, j.method("mceAdsShowPrivacy", "()V"));
    (void)j.ok("mceAdsShowPrivacy");
}

void load(Format f, StringView unit, u32 request) {
    if (!g_ads.started) return refuse_load(unit, request);
    Jni j;
    jstring s = j.str(unit);
    j.env->CallVoidMethod(j.activity, j.method("mceAdsLoad", "(ILjava/lang/String;I)V"), static_cast<jint>(f), s,
                          static_cast<jint>(request));
    const bool ok = j.ok("mceAdsLoad");
    j.env->DeleteLocalRef(s);
    if (!ok) refuse_load(unit, request);
}

void show(Format f, StringView unit, u32 request) {
    if (!g_ads.started) return refuse_show(unit, request);
    Jni j;
    jstring s = j.str(unit);
    j.env->CallVoidMethod(j.activity, j.method("mceAdsShow", "(ILjava/lang/String;I)V"), static_cast<jint>(f), s,
                          static_cast<jint>(request));
    const bool ok = j.ok("mceAdsShow");
    j.env->DeleteLocalRef(s);
    if (!ok) refuse_show(unit, request);
}

void show_banner(StringView unit, u32 request, bool top, u32 inset_left, u32 inset_top, u32 inset_right,
                 u32 inset_bottom) {
    if (!g_ads.started) return refuse_load(unit, request);
    Jni j;
    jstring s = j.str(unit);
    j.env->CallVoidMethod(j.activity, j.method("mceAdsBanner", "(Ljava/lang/String;IZIIII)V"), s,
                          static_cast<jint>(request), static_cast<jboolean>(top), static_cast<jint>(inset_left),
                          static_cast<jint>(inset_top), static_cast<jint>(inset_right), static_cast<jint>(inset_bottom));
    const bool ok = j.ok("mceAdsBanner");
    j.env->DeleteLocalRef(s);
    if (!ok) refuse_load(unit, request);
}

void hide_banner() {
    if (!g_ads.started) return;
    Jni j;
    j.env->CallVoidMethod(j.activity, j.method("mceAdsHideBanner", "()V"));
    (void)j.ok("mceAdsHideBanner");
}

}  // namespace mce::platform::ads

// MceAds.nativeAnswer: one answer, on the UI thread.
extern "C" JNIEXPORT void JNICALL Java_org_meshcontrol_ads_MceAds_nativeAnswer(JNIEnv* env, jclass, jint kind,
                                                                              jint request, jstring unit,
                                                                              jint amount) {
    using mce::platform::ads::AnswerKind;
    if (kind < 0 || kind > static_cast<jint>(AnswerKind::Failed)) return;
    const mce::platform::detail::JStr u(env, unit);
    mce::platform::ads::answer(static_cast<AnswerKind>(kind), static_cast<mce::u32>(request), u.view(),
                               amount < 0 ? 0u : static_cast<mce::u32>(amount));
}

// MceAds.nativeConsent: what the consent flow ended with (Consent's values).
extern "C" JNIEXPORT void JNICALL Java_org_meshcontrol_ads_MceAds_nativeConsent(JNIEnv*, jclass, jint consent) {
    using mce::platform::ads::Consent;
    if (consent < 0 || consent > static_cast<jint>(Consent::NonPersonalized)) return;
    mce::platform::ads::g_consent.store(static_cast<mce::u8>(consent), std::memory_order_release);
}

// ---- iOS: Google Mobile Ads + UMP + ATT (ads_admob.swift) ----------------------------------------
#elif defined(SDL_PLATFORM_IOS) && defined(MCE_ADMOB)

// The Swift side's entry points (@_cdecl in ads_admob.swift).
extern "C" {
void mce_admob_start(bool test_device);
void mce_admob_stop();
bool mce_admob_privacy_required();
void mce_admob_show_privacy();
void mce_admob_load(int format, const char* unit, mce::u32 request);
void mce_admob_show(int format, const char* unit, mce::u32 request);
void mce_admob_banner(const char* unit, mce::u32 request, bool top, mce::u32 l, mce::u32 t, mce::u32 r, mce::u32 b);
void mce_admob_hide_banner();
}

Backend backend() { return Backend::GoogleMobileAds; }

bool start(AnswerFn fn, void* user, const Options& opt) {
    g_ads.fn = fn;
    g_ads.user = user;
    g_ads.started = true;
    g_consent.store(static_cast<u8>(Consent::Unknown), std::memory_order_release);
    mce_admob_start(opt.test_device);
    return true;
}

void stop() {
    if (g_ads.started) mce_admob_stop();
    g_ads = {};
}

bool privacy_options_required() { return g_ads.started && mce_admob_privacy_required(); }
void show_privacy_options() {
    if (g_ads.started) mce_admob_show_privacy();
}

void load(Format f, StringView unit, u32 request) {
    if (!g_ads.started) return refuse_load(unit, request);
    const FixedString<128> z(unit);
    mce_admob_load(static_cast<int>(f), z.c_str(), request);
}

void show(Format f, StringView unit, u32 request) {
    if (!g_ads.started) return refuse_show(unit, request);
    const FixedString<128> z(unit);
    mce_admob_show(static_cast<int>(f), z.c_str(), request);
}

void show_banner(StringView unit, u32 request, bool top, u32 inset_left, u32 inset_top, u32 inset_right,
                 u32 inset_bottom) {
    if (!g_ads.started) return refuse_load(unit, request);
    const FixedString<128> z(unit);
    mce_admob_banner(z.c_str(), request, top, inset_left, inset_top, inset_right, inset_bottom);
}

void hide_banner() {
    if (g_ads.started) mce_admob_hide_banner();
}

}  // namespace mce::platform::ads

// Called by the Swift side with one answer (the unit string is valid during the call).
extern "C" void mce_admob_answer(int kind, mce::u32 request, const char* unit, mce::u32 amount) {
    using mce::platform::ads::AnswerKind;
    if (kind < 0 || kind > static_cast<int>(AnswerKind::Failed)) return;
    mce::platform::ads::answer(static_cast<AnswerKind>(kind), request, unit ? mce::StringView(unit) : mce::StringView(),
                               amount);
}

extern "C" void mce_admob_consent(int consent) {
    using mce::platform::ads::Consent;
    if (consent < 0 || consent > static_cast<int>(Consent::NonPersonalized)) return;
    mce::platform::ads::g_consent.store(static_cast<mce::u8>(consent), std::memory_order_release);
}

// ---- desktop, and phones built without the ads side ----------------------------------------------
#else

Backend backend() { return Backend::None; }

bool start(AnswerFn fn, void* user, const Options&) {
    g_ads.fn = fn;
    g_ads.user = user;
    return false;
}

void stop() { g_ads = {}; }
bool privacy_options_required() { return false; }
void show_privacy_options() {}
void load(Format, StringView unit, u32 request) { refuse_load(unit, request); }
void show(Format, StringView unit, u32 request) { refuse_show(unit, request); }
void show_banner(StringView unit, u32 request, bool, u32, u32, u32, u32) { refuse_load(unit, request); }
void hide_banner() {}

}  // namespace mce::platform::ads

#endif
