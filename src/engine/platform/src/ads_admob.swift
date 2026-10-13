// mce_platform: Google Mobile Ads, UMP consent and App Tracking Transparency for platform/ads.h on
// iOS (built when MCE_ADMOB is ON; written against GoogleMobileAds 11+ and UserMessagingPlatform 3+,
// the SDKs' Swift names without the GAD/UMP prefixes).
//
// ads.cpp calls the @_cdecl functions below on the main thread; answers go back through
// mce_admob_answer (platform::ads::AnswerKind's values), the consent outcome through
// mce_admob_consent (platform::ads::Consent's values). Order: UMP's form where the law needs one,
// then the ATT prompt, then MobileAds.start; requests made before that wait. Without consent to
// personalized ads, or with tracking denied, requests carry npa=1.

import AppTrackingTransparency
import Foundation
import GoogleMobileAds
import UIKit
import UserMessagingPlatform

@_silgen_name("mce_admob_answer")
func mce_admob_answer(_ kind: Int32, _ request: UInt32, _ unit: UnsafePointer<CChar>?, _ amount: UInt32)

@_silgen_name("mce_admob_consent")
func mce_admob_consent(_ consent: Int32)

private enum Kind: Int32 { case loaded = 0, noFill, shown, rewarded, closed, failed }

private func send(_ kind: Kind, _ request: UInt32, _ unit: String, _ amount: UInt32 = 0) {
    unit.withCString { mce_admob_answer(kind.rawValue, request, $0, amount) }
}

private func rootViewController() -> UIViewController? {
    let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
    return scenes.flatMap { $0.windows }.first { $0.isKeyWindow }?.rootViewController
}

private final class FullscreenDelegate: NSObject, FullScreenContentDelegate {
    let unit: String
    let request: UInt32
    var done: (() -> Void)?
    init(unit: String, request: UInt32) {
        self.unit = unit
        self.request = request
    }
    func adWillPresentFullScreenContent(_ ad: FullScreenPresentingAd) { send(.shown, request, unit) }
    func adDidDismissFullScreenContent(_ ad: FullScreenPresentingAd) {
        send(.closed, request, unit)
        done?()
    }
    func ad(_ ad: FullScreenPresentingAd, didFailToPresentFullScreenContentWithError error: Error) {
        send(.failed, request, unit)
        done?()
    }
}

private final class BannerDelegate: NSObject, BannerViewDelegate {
    let unit: String
    let request: UInt32
    init(unit: String, request: UInt32) {
        self.unit = unit
        self.request = request
    }
    func bannerViewDidReceiveAd(_ bannerView: BannerView) { send(.loaded, request, unit) }
    func bannerView(_ bannerView: BannerView, didFailToReceiveAdWithError error: Error) {
        let code = (error as NSError).code
        send(code == RequestError.noFill.rawValue ? .noFill : .failed, request, unit)
    }
}

private final class MceAdMob {
    static let shared = MceAdMob()

    private var ready = false
    private var gathering = false
    private var personalized = false
    private var waiting: [() -> Void] = []
    private var refusals: [() -> Void] = []
    private var interstitials: [String: InterstitialAd] = [:]
    private var rewarded: [String: RewardedAd] = [:]
    private var showing: FullscreenDelegate?
    private var banner: BannerView?
    private var bannerDelegate: BannerDelegate?

    func start(testDevice: Bool) {
        if gathering || ready { return }
        gathering = true
        let params = RequestParameters()
        if testDevice {
            let debug = DebugSettings()
            debug.geography = .EEA
            params.debugSettings = debug
        }
        ConsentInformation.shared.requestConsentInfoUpdate(with: params) { [weak self] error in
            if let error = error { NSLog("platform.ads: consent update: %@", error.localizedDescription) }
            DispatchQueue.main.async {
                ConsentForm.loadAndPresentIfRequired(from: rootViewController()) { formError in
                    if let formError = formError { NSLog("platform.ads: consent form: %@", formError.localizedDescription) }
                    self?.askTracking()
                }
            }
        }
    }

    func stop() {
        hideBanner()
        interstitials.removeAll()
        rewarded.removeAll()
        waiting.removeAll()
        refusals.removeAll()
    }

    func privacyRequired() -> Bool {
        return ConsentInformation.shared.privacyOptionsRequirementStatus == .required
    }

    func showPrivacy() {
        ConsentForm.presentPrivacyOptionsForm(from: rootViewController()) { [weak self] _ in
            self?.updatePersonalized()
        }
    }

    func load(format: Int32, unit: String, request: UInt32) {
        whenReady({ [weak self] in
            guard let self = self else { return }
            let completion: (Error?) -> Void = { error in
                guard let error = error else { return send(.loaded, request, unit) }
                send((error as NSError).code == RequestError.noFill.rawValue ? .noFill : .failed, request, unit)
            }
            if format == 1 {
                InterstitialAd.load(with: unit, request: self.adRequest()) { ad, error in
                    if let ad = ad { self.interstitials[unit] = ad }
                    completion(error)
                }
            } else if format == 0 {
                RewardedAd.load(with: unit, request: self.adRequest()) { ad, error in
                    if let ad = ad { self.rewarded[unit] = ad }
                    completion(error)
                }
            } else {
                send(.failed, request, unit)
            }
        }, refuse: { send(.noFill, request, unit) })
    }

    func show(format: Int32, unit: String, request: UInt32) {
        guard let root = rootViewController() else { return send(.failed, request, unit) }
        let delegate = FullscreenDelegate(unit: unit, request: request)
        delegate.done = { [weak self] in self?.showing = nil }
        if format == 1, let ad = interstitials.removeValue(forKey: unit) {
            showing = delegate
            ad.fullScreenContentDelegate = delegate
            ad.present(from: root)
        } else if format == 0, let ad = rewarded.removeValue(forKey: unit) {
            showing = delegate
            ad.fullScreenContentDelegate = delegate
            ad.present(from: root) {
                send(.rewarded, request, unit, UInt32(truncating: ad.adReward.amount))
            }
        } else {
            send(.failed, request, unit)  // not loaded
        }
    }

    // Insets are device pixels: the banner sits inside the safe area, centered.
    func banner(unit: String, request: UInt32, top: Bool, insets: UIEdgeInsets) {
        whenReady({ [weak self] in
            guard let self = self, let root = rootViewController() else { return send(.failed, request, unit) }
            self.hideBanner()
            let scale = root.view.window?.screen.scale ?? UIScreen.main.scale
            let width = root.view.bounds.width - (insets.left + insets.right) / scale
            let view = BannerView(adSize: currentOrientationAnchoredAdaptiveBanner(width: width))
            let delegate = BannerDelegate(unit: unit, request: request)
            view.adUnitID = unit
            view.rootViewController = root
            view.delegate = delegate
            view.translatesAutoresizingMaskIntoConstraints = false
            root.view.addSubview(view)
            NSLayoutConstraint.activate([
                view.centerXAnchor.constraint(equalTo: root.view.centerXAnchor,
                                              constant: (insets.left - insets.right) / scale / 2),
                top ? view.topAnchor.constraint(equalTo: root.view.topAnchor, constant: insets.top / scale)
                    : view.bottomAnchor.constraint(equalTo: root.view.bottomAnchor, constant: -insets.bottom / scale),
            ])
            self.banner = view
            self.bannerDelegate = delegate
            view.load(self.adRequest())
        }, refuse: { send(.noFill, request, unit) })
    }

    func hideBanner() {
        banner?.removeFromSuperview()
        banner = nil
        bannerDelegate = nil
    }

    private func askTracking() {
        ATTrackingManager.requestTrackingAuthorization { [weak self] _ in
            DispatchQueue.main.async { self?.consentDone() }
        }
    }

    private func consentDone() {
        gathering = false
        updatePersonalized()
        guard ConsentInformation.shared.canRequestAds else {
            NSLog("platform.ads: consent does not allow ad requests")
            let run = refusals
            waiting.removeAll()
            refusals.removeAll()
            run.forEach { $0() }
            return
        }
        MobileAds.shared.start { [weak self] _ in
            DispatchQueue.main.async {
                guard let self = self else { return }
                self.ready = true
                let run = self.waiting
                self.waiting.removeAll()
                self.refusals.removeAll()
                run.forEach { $0() }
            }
        }
    }

    // Personalized ads need tracking allowed and, where UMP asked, IAB TCF purposes 1, 3 and 4.
    private func updatePersonalized() {
        let tracking = ATTrackingManager.trackingAuthorizationStatus == .authorized
        var tcf = true
        if ConsentInformation.shared.consentStatus != .notRequired {
            let purposes = UserDefaults.standard.string(forKey: "IABTCF_PurposeConsents") ?? ""
            let c = Array(purposes)
            tcf = c.count >= 4 && c[0] == "1" && c[2] == "1" && c[3] == "1"
        }
        personalized = tracking && tcf
        mce_admob_consent(personalized ? 1 : 2)
    }

    private func whenReady(_ call: @escaping () -> Void, refuse: @escaping () -> Void) {
        if ready {
            call()
        } else if gathering {
            waiting.append(call)
            refusals.append(refuse)
        } else {
            refuse()
        }
    }

    private func adRequest() -> Request {
        let request = Request()
        if !personalized {
            let extras = Extras()
            extras.additionalParameters = ["npa": "1"]
            request.register(extras)
        }
        return request
    }
}

@_cdecl("mce_admob_start")
public func mce_admob_start(_ testDevice: Bool) { MceAdMob.shared.start(testDevice: testDevice) }

@_cdecl("mce_admob_stop")
public func mce_admob_stop() { MceAdMob.shared.stop() }

@_cdecl("mce_admob_privacy_required")
public func mce_admob_privacy_required() -> Bool { return MceAdMob.shared.privacyRequired() }

@_cdecl("mce_admob_show_privacy")
public func mce_admob_show_privacy() { MceAdMob.shared.showPrivacy() }

@_cdecl("mce_admob_load")
public func mce_admob_load(_ format: Int32, _ unit: UnsafePointer<CChar>, _ request: UInt32) {
    MceAdMob.shared.load(format: format, unit: String(cString: unit), request: request)
}

@_cdecl("mce_admob_show")
public func mce_admob_show(_ format: Int32, _ unit: UnsafePointer<CChar>, _ request: UInt32) {
    MceAdMob.shared.show(format: format, unit: String(cString: unit), request: request)
}

@_cdecl("mce_admob_banner")
public func mce_admob_banner(_ unit: UnsafePointer<CChar>, _ request: UInt32, _ top: Bool, _ l: UInt32, _ t: UInt32,
                             _ r: UInt32, _ b: UInt32) {
    let insets = UIEdgeInsets(top: CGFloat(t), left: CGFloat(l), bottom: CGFloat(b), right: CGFloat(r))
    MceAdMob.shared.banner(unit: String(cString: unit), request: request, top: top, insets: insets)
}

@_cdecl("mce_admob_hide_banner")
public func mce_admob_hide_banner() { MceAdMob.shared.hideBanner() }
