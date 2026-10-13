#include "mce/platform/store.h"

#include <SDL3/SDL.h>

#include "mce/fixed_string.h"
#include "mce/log.h"

#if defined(SDL_PLATFORM_ANDROID)
#include "android_jni.h"
#endif

namespace mce::platform::store {
namespace {

struct State {
    AnswerFn fn = nullptr;
    void* user = nullptr;
    bool started = false;
};
State g_store;

void answer(AnswerKind kind, u32 request, StringView id = {}, StringView price = {}, StringView transaction = {}) {
    if (!g_store.fn) return;
    Answer a;
    a.kind = kind;
    a.request = request;
    a.id = id;
    a.price = price;
    a.transaction = transaction;
    g_store.fn(g_store.user, a);
}

// No store here: every request still gets its answers, so callers never wait on one.
void refuse_query(u32 request) { answer(AnswerKind::ProductsReady, request); }
void refuse_purchase(StringView id, u32 request) { answer(AnswerKind::Failed, request, id); }
void refuse_restore(u32 request) { answer(AnswerKind::RestoreFinished, request); }

}  // namespace

const char* backend_name(Backend b) {
    switch (b) {
        case Backend::None: return "none";
        case Backend::PlayBilling: return "play";
        case Backend::StoreKit: return "storekit";
    }
    return "none";
}

// ---- Android: Play Billing through the stamped MceActivity ---------------------------------------
#if defined(SDL_PLATFORM_ANDROID)
namespace {

using detail::Jni;

bool play_available() {
    static const bool has = [] {
        Jni j;
        return j.method("mceBillingStart", "()V") != nullptr;
    }();
    return has;
}

}  // namespace

Backend backend() { return play_available() ? Backend::PlayBilling : Backend::None; }

bool start(AnswerFn fn, void* user) {
    g_store.fn = fn;
    g_store.user = user;
    if (!play_available()) return false;
    Jni j;
    j.env->CallVoidMethod(j.activity, j.method("mceBillingStart", "()V"));
    g_store.started = j.ok("mceBillingStart");
    return g_store.started;
}

void stop() {
    if (g_store.started) {
        Jni j;
        j.env->CallVoidMethod(j.activity, j.method("mceBillingStop", "()V"));
        (void)j.ok("mceBillingStop");
    }
    g_store = {};
}

void query_products(const StringView* ids, u32 count, u32 request) {
    if (!g_store.started) return refuse_query(request);
    Jni j;
    jclass string_cls = j.env->FindClass("java/lang/String");
    jobjectArray arr = j.env->NewObjectArray(static_cast<jsize>(count), string_cls, nullptr);
    for (u32 i = 0; i < count; ++i) {
        jstring s = j.str(ids[i]);
        j.env->SetObjectArrayElement(arr, static_cast<jsize>(i), s);
        j.env->DeleteLocalRef(s);
    }
    j.env->CallVoidMethod(j.activity, j.method("mceBillingQuery", "([Ljava/lang/String;I)V"), arr,
                          static_cast<jint>(request));
    const bool ok = j.ok("mceBillingQuery");
    j.env->DeleteLocalRef(arr);
    j.env->DeleteLocalRef(string_cls);
    if (!ok) refuse_query(request);
}

void purchase(StringView id, bool consumable, u32 request) {
    if (!g_store.started) return refuse_purchase(id, request);
    Jni j;
    jstring s = j.str(id);
    j.env->CallVoidMethod(j.activity, j.method("mceBillingPurchase", "(Ljava/lang/String;ZI)V"), s,
                          static_cast<jboolean>(consumable), static_cast<jint>(request));
    const bool ok = j.ok("mceBillingPurchase");
    j.env->DeleteLocalRef(s);
    if (!ok) refuse_purchase(id, request);
}

void restore(u32 request) {
    if (!g_store.started) return refuse_restore(request);
    Jni j;
    j.env->CallVoidMethod(j.activity, j.method("mceBillingRestore", "(I)V"), static_cast<jint>(request));
    if (!j.ok("mceBillingRestore")) refuse_restore(request);
}

void finish(StringView transaction, bool consumable) {
    if (!g_store.started) return;
    Jni j;
    jstring s = j.str(transaction);
    j.env->CallVoidMethod(j.activity, j.method("mceBillingFinish", "(Ljava/lang/String;Z)V"), s,
                          static_cast<jboolean>(consumable));
    (void)j.ok("mceBillingFinish");
    j.env->DeleteLocalRef(s);
}

}  // namespace mce::platform::store

// MceBilling.nativeAnswer: called on Play Billing's threads with one answer.
extern "C" JNIEXPORT void JNICALL Java_org_meshcontrol_billing_MceBilling_nativeAnswer(
    JNIEnv* env, jclass, jint kind, jint request, jstring id, jstring price, jstring transaction) {
    using mce::platform::store::AnswerKind;
    if (kind < 0 || kind > static_cast<jint>(AnswerKind::RestoreFinished)) return;
    const mce::platform::detail::JStr i(env, id), p(env, price), t(env, transaction);
    mce::platform::store::answer(static_cast<AnswerKind>(kind), static_cast<mce::u32>(request), i.view(), p.view(),
                                 t.view());
}

// ---- iOS: StoreKit 2 (store_storekit.swift) ------------------------------------------------------
#elif defined(SDL_PLATFORM_IOS) && defined(MCE_STOREKIT)

// The Swift side's entry points (@_cdecl in store_storekit.swift).
extern "C" {
void mce_storekit_start();
void mce_storekit_stop();
void mce_storekit_query(const char* const* ids, mce::u32 count, mce::u32 request);
void mce_storekit_purchase(const char* id, mce::u32 request);
void mce_storekit_restore(mce::u32 request);
void mce_storekit_finish(const char* transaction);
}

Backend backend() { return Backend::StoreKit; }

bool start(AnswerFn fn, void* user) {
    g_store.fn = fn;
    g_store.user = user;
    g_store.started = true;
    mce_storekit_start();
    return true;
}

void stop() {
    if (g_store.started) mce_storekit_stop();
    g_store = {};
}

void query_products(const StringView* ids, u32 count, u32 request) {
    if (!g_store.started) return refuse_query(request);
    constexpr u32 kMax = 256;
    if (count > kMax) {
        MCE_LOG_WARN("platform.store: {} products queried, StoreKit asked for the first {}", count, kMax);
        count = kMax;
    }
    static FixedString<128> copies[kMax];
    static const char* ptrs[kMax];
    for (u32 i = 0; i < count; ++i) {
        copies[i].assign(ids[i]);
        ptrs[i] = copies[i].c_str();
    }
    mce_storekit_query(ptrs, count, request);
}

void purchase(StringView id, bool, u32 request) {
    if (!g_store.started) return refuse_purchase(id, request);
    const FixedString<128> z(id);
    mce_storekit_purchase(z.c_str(), request);
}

void restore(u32 request) {
    if (!g_store.started) return refuse_restore(request);
    mce_storekit_restore(request);
}

void finish(StringView transaction, bool) {
    if (!g_store.started) return;
    const FixedString<128> z(transaction);
    mce_storekit_finish(z.c_str());
}

}  // namespace mce::platform::store

// Called by the Swift side, on its tasks, with one answer (strings valid during the call).
extern "C" void mce_storekit_answer(int kind, mce::u32 request, const char* id, const char* price,
                                    const char* transaction) {
    using mce::platform::store::AnswerKind;
    if (kind < 0 || kind > static_cast<int>(AnswerKind::RestoreFinished)) return;
    auto view = [](const char* s) { return s ? mce::StringView(s) : mce::StringView(); };
    mce::platform::store::answer(static_cast<AnswerKind>(kind), request, view(id), view(price), view(transaction));
}

// ---- desktop, and phones built without a store side ----------------------------------------------
#else

Backend backend() { return Backend::None; }

bool start(AnswerFn fn, void* user) {
    g_store.fn = fn;
    g_store.user = user;
    return false;
}

void stop() { g_store = {}; }

void query_products(const StringView*, u32, u32 request) { refuse_query(request); }
void purchase(StringView id, bool, u32 request) { refuse_purchase(id, request); }
void restore(u32 request) { refuse_restore(request); }
void finish(StringView, bool) {}

}  // namespace mce::platform::store

#endif
