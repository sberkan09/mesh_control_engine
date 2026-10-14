// mce_platform: Firebase Crashlytics and Analytics for platform/firebase.h on iOS (built when
// MCE_FIREBASE is ON; written against the Firebase iOS SDK 11+, from its Firebase.zip distribution).
//
// firebase.cpp calls the @_cdecl functions below on the main thread. The app's Info.plist (stamped
// by cmake/mobile/ios_app.py) starts both SDKs with collection off; mce_firebase_collection turns it
// on once the player consents (platform::firebase::Collection's values).

import FirebaseAnalytics
import FirebaseCore
import FirebaseCrashlytics
import Foundation

private var pendingName = ""
private var pendingParams: [String: Any] = [:]

@_cdecl("mce_firebase_start")
public func mce_firebase_start() {
    if FirebaseApp.app() == nil {
        FirebaseApp.configure()  // reads GoogleService-Info.plist from the bundle
    }
}

@_cdecl("mce_firebase_collection")
public func mce_firebase_collection(_ collection: Int32) {
    let crashlytics = Crashlytics.crashlytics()
    switch collection {
    case 1:  // Granted
        Analytics.setConsent([.analyticsStorage: .granted])
        Analytics.setAnalyticsCollectionEnabled(true)
        crashlytics.setCrashlyticsCollectionEnabled(true)
        crashlytics.sendUnsentReports()
    case 2:  // Denied
        Analytics.setConsent([.analyticsStorage: .denied])
        Analytics.setAnalyticsCollectionEnabled(false)
        crashlytics.setCrashlyticsCollectionEnabled(false)
        crashlytics.deleteUnsentReports()
    default:  // Undecided: keep everything on the device
        Analytics.setAnalyticsCollectionEnabled(false)
        crashlytics.setCrashlyticsCollectionEnabled(false)
    }
}

@_cdecl("mce_firebase_event_begin")
public func mce_firebase_event_begin(_ name: UnsafePointer<CChar>) {
    pendingName = String(cString: name)
    pendingParams = [:]
}

@_cdecl("mce_firebase_event_int")
public func mce_firebase_event_int(_ key: UnsafePointer<CChar>, _ value: Int64) {
    pendingParams[String(cString: key)] = NSNumber(value: value)
}

@_cdecl("mce_firebase_event_float")
public func mce_firebase_event_float(_ key: UnsafePointer<CChar>, _ value: Double) {
    pendingParams[String(cString: key)] = NSNumber(value: value)
}

@_cdecl("mce_firebase_event_string")
public func mce_firebase_event_string(_ key: UnsafePointer<CChar>, _ value: UnsafePointer<CChar>) {
    pendingParams[String(cString: key)] = String(cString: value)
}

@_cdecl("mce_firebase_event_end")
public func mce_firebase_event_end() {
    Analytics.logEvent(pendingName, parameters: pendingParams.isEmpty ? nil : pendingParams)
    pendingName = ""
    pendingParams = [:]
}
