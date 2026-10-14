#pragma once
// mce_platform (L1): the phone's own "rate this app" prompt (M-MOBILE step 11c, #147).
//
// The store wall (check_walls.py) covers this too: StoreKit, the Play In-App Review library and JNI
// live only in mce_platform and the stamped Android project.
//
// - Android: Play In-App Review through org.meshcontrol.review.MceReview, stamped into every game's
//   Gradle project (cmake/mobile/android_app.py); the call crosses JNI.
// - iOS: StoreKit's review request (review_storekit.swift; AppStore.requestReview, the successor of
//   SKStoreReviewController), built when MCE_STORE_REVIEW is ON.
// - Everywhere else, and on a phone build without that side: backend() is None and request_review()
//   logs a line and does nothing.
//
// The OS decides whether a prompt actually shows: Apple shows it at most three times a year, Play
// keeps its own quota, and neither says whether it showed or what the player answered. So ask at a
// good moment (after a won level, never from a button and never on the first launch), and expect
// nothing back. On a phone, the call returns at once; the prompt, if any, comes from the UI thread.

#include "mce/types.h"

namespace mce::platform {

enum class ReviewBackend : u8 { None, StoreKit, PlayInAppReview };

ReviewBackend review_backend();
const char* review_backend_name(ReviewBackend b);  // "none", "storekit", "play"

// Asks the OS for its review prompt. True when the request went to the OS (which may still show
// nothing); false when there is no review backend here (desktop: a logged no-op). Main thread.
bool request_review();

// How many times request_review() has been called in this process, backend or not: what tests and
// the desktop's fake check.
u32 review_requests();

}  // namespace mce::platform
