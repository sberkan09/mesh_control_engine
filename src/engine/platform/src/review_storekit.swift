// mce_platform: StoreKit's review prompt for platform/review.h on iOS (built when MCE_STORE_REVIEW
// is ON).
//
// review.cpp calls mce_storekit_request_review on the main thread. AppStore.requestReview(in:) is
// SKStoreReviewController's successor (iOS 16+, the engine's minimum); it needs the foreground
// window scene and shows nothing when Apple's quota (three a year) is spent, so nothing comes back.

import StoreKit
import UIKit

@_cdecl("mce_storekit_request_review")
public func mce_storekit_request_review() {
    DispatchQueue.main.async {
        let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        guard let scene = scenes.first(where: { $0.activationState == .foregroundActive }) ?? scenes.first else {
            NSLog("MceReview: no window scene, review request dropped")
            return
        }
        AppStore.requestReview(in: scene)
    }
}
