// mce_platform: VoiceOver for platform/screen_reader.h on iOS (built when MCE_UIKIT_ACCESS is ON).
//
// screen_reader.cpp calls these on the main thread. The text is copied into a Swift String before
// mce_uikit_announce returns, so the caller's buffer may go.

import UIKit

@_cdecl("mce_uikit_voiceover_running")
public func mce_uikit_voiceover_running() -> Bool {
    return UIAccessibility.isVoiceOverRunning
}

@_cdecl("mce_uikit_announce")
public func mce_uikit_announce(_ utf8: UnsafePointer<CChar>) {
    let text = String(cString: utf8)
    if Thread.isMainThread {
        UIAccessibility.post(notification: .announcement, argument: text)
    } else {
        DispatchQueue.main.async { UIAccessibility.post(notification: .announcement, argument: text) }
    }
}
