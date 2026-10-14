// mce_platform: UIKit's feedback generators for platform/haptics.h on iOS (built when
// MCE_UIKIT_HAPTICS is ON).
//
// haptics.cpp calls mce_uikit_haptic on the main thread with a platform::haptics::Preset value. The
// generators are kept and prepared so the next tap fires without the engine's start-up delay; iOS
// itself skips them on devices without a Taptic Engine and when the user turned system haptics off.

import UIKit

private enum Preset: Int32 {
    case selection = 0, light, medium, heavy, success, warning, error
}

private final class MceHaptics {
    static let shared = MceHaptics()

    private let selection = UISelectionFeedbackGenerator()
    private let light = UIImpactFeedbackGenerator(style: .light)
    private let medium = UIImpactFeedbackGenerator(style: .medium)
    private let heavy = UIImpactFeedbackGenerator(style: .heavy)
    private let notification = UINotificationFeedbackGenerator()

    func play(_ preset: Preset) {
        switch preset {
        case .selection: selection.selectionChanged(); selection.prepare()
        case .light: light.impactOccurred(); light.prepare()
        case .medium: medium.impactOccurred(); medium.prepare()
        case .heavy: heavy.impactOccurred(); heavy.prepare()
        case .success: notification.notificationOccurred(.success); notification.prepare()
        case .warning: notification.notificationOccurred(.warning); notification.prepare()
        case .error: notification.notificationOccurred(.error); notification.prepare()
        }
    }
}

@_cdecl("mce_uikit_haptic")
public func mce_uikit_haptic(_ preset: Int32) {
    guard let p = Preset(rawValue: preset) else { return }
    if Thread.isMainThread {
        MceHaptics.shared.play(p)
    } else {
        DispatchQueue.main.async { MceHaptics.shared.play(p) }
    }
}
