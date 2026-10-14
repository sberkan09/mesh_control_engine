#include "mce/platform/review.h"

#include <SDL3/SDL.h>

#include <atomic>

#include "mce/log.h"

#if defined(SDL_PLATFORM_ANDROID)
#include "android_jni.h"
#endif

namespace mce::platform {
namespace {

std::atomic<u32> g_requests{0};

}  // namespace

const char* review_backend_name(ReviewBackend b) {
    switch (b) {
        case ReviewBackend::StoreKit: return "storekit";
        case ReviewBackend::PlayInAppReview: return "play";
        case ReviewBackend::None: break;
    }
    return "none";
}

u32 review_requests() { return g_requests.load(std::memory_order_relaxed); }

// ---- Android: Play In-App Review through the stamped MceActivity ---------------------------------
#if defined(SDL_PLATFORM_ANDROID)

ReviewBackend review_backend() {
    static const bool has = [] {
        detail::Jni j;
        return j.method("mceReviewRequest", "()V") != nullptr;
    }();
    return has ? ReviewBackend::PlayInAppReview : ReviewBackend::None;
}

bool request_review() {
    g_requests.fetch_add(1, std::memory_order_relaxed);
    if (review_backend() == ReviewBackend::None) {
        MCE_LOG_INFO("review: request_review() ignored, this build has no Play In-App Review");
        return false;
    }
    detail::Jni j;
    j.env->CallVoidMethod(j.activity, j.method("mceReviewRequest", "()V"));
    return j.ok("mceReviewRequest");
}

// ---- iOS: StoreKit (review_storekit.swift) --------------------------------------------------------
#elif defined(SDL_PLATFORM_IOS) && defined(MCE_STORE_REVIEW)

extern "C" void mce_storekit_request_review();  // @_cdecl in review_storekit.swift

ReviewBackend review_backend() { return ReviewBackend::StoreKit; }

bool request_review() {
    g_requests.fetch_add(1, std::memory_order_relaxed);
    mce_storekit_request_review();
    return true;
}

// ---- desktop, and phones built without the review side --------------------------------------------
#else

ReviewBackend review_backend() { return ReviewBackend::None; }

bool request_review() {
    const u32 n = g_requests.fetch_add(1, std::memory_order_relaxed) + 1;
    MCE_LOG_INFO("review: request_review() #{} is a no-op here (the phone's store shows the prompt)", n);
    return false;
}

#endif

}  // namespace mce::platform
