// mce_platform: Game Center for platform/game_services.h on iOS (built when MCE_GAMEKIT is ON).
//
// game_services.cpp calls the @_cdecl functions below on the main thread; every answer goes back
// through mce_gamekit_answer (game_services.cpp) with the tag the call carried. The kinds are
// platform::game_services::AnswerKind's values. Cloud saves are Game Center saved games (iCloud):
// several devices saving under one name leave several copies, and the newest is the one read.

import Foundation
import GameKit
import UIKit

@_silgen_name("mce_gamekit_answer")
func mce_gamekit_answer(_ kind: Int32, _ ok: Int32, _ missing: Int32, _ tag: UInt16,
                        _ bytes: UnsafePointer<UInt8>?, _ size: Int)

private enum Kind: Int32 {
    case signedIn = 0, achievement, score, blobRead, blobWritten
}

private func send(_ kind: Kind, _ ok: Bool, _ tag: UInt16, missing: Bool = false, data: Data? = nil) {
    if let data = data, !data.isEmpty {
        data.withUnsafeBytes { raw in
            mce_gamekit_answer(kind.rawValue, ok ? 1 : 0, missing ? 1 : 0, tag,
                               raw.bindMemory(to: UInt8.self).baseAddress, data.count)
        }
    } else {
        mce_gamekit_answer(kind.rawValue, ok ? 1 : 0, missing ? 1 : 0, tag, nil, 0)
    }
}

private func rootViewController() -> UIViewController? {
    let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
    return scenes.flatMap { $0.windows }.first { $0.isKeyWindow }?.rootViewController
}

private final class MceGameKit: NSObject, GKGameCenterControllerDelegate {
    static let shared = MceGameKit()

    private var signInView: UIViewController?  // Game Center's sign-in, while it asks for one
    private var handled = false                // the authenticate handler has run once
    private var answered = true                // the pending sign_in got its answer
    private var wantsView = false              // the pending sign_in may show Game Center's sign-in

    func start() {
        // Game Center calls this soon after it is set, and again whenever the player's state changes
        // (also when its sign-in closes). A sign_in made before the first call waits for it.
        GKLocalPlayer.local.authenticateHandler = { [weak self] view, _ in
            guard let self = self else { return }
            self.handled = true
            self.signInView = view
            if !self.answered { self.settle() }
        }
    }

    func signIn(interactive: Bool) {
        answered = false
        wantsView = interactive
        if GKLocalPlayer.local.isAuthenticated { return finishSignIn() }
        if handled { settle() }
    }

    private func settle() {
        if let view = signInView, wantsView, let root = rootViewController() {
            wantsView = false  // the handler's next call answers
            root.present(view, animated: true)
            return
        }
        finishSignIn()
    }

    private func finishSignIn() {
        if answered { return }
        answered = true
        send(.signedIn, GKLocalPlayer.local.isAuthenticated, 0)
    }

    func gameCenterViewControllerDidFinish(_ controller: GKGameCenterViewController) {
        controller.dismiss(animated: true)
    }

    func show(_ state: GKGameCenterViewControllerState, leaderboard: String? = nil) {
        guard GKLocalPlayer.local.isAuthenticated, let root = rootViewController() else { return }
        let view: GKGameCenterViewController
        if let id = leaderboard {
            view = GKGameCenterViewController(leaderboardID: id, playerScope: .global, timeScope: .allTime)
        } else {
            view = GKGameCenterViewController(state: state)
        }
        view.gameCenterDelegate = self
        root.present(view, animated: true)
    }
}

// The newest saved game under `name`, if any.
private func newestSave(_ name: String, _ done: @escaping (GKSavedGame?, Bool) -> Void) {
    GKLocalPlayer.local.fetchSavedGames { games, error in
        if error != nil { return done(nil, false) }
        let mine = (games ?? []).filter { $0.name == name }
        done(mine.max { ($0.modificationDate ?? .distantPast) < ($1.modificationDate ?? .distantPast) }, true)
    }
}

@_cdecl("mce_gamekit_start")
public func mce_gamekit_start() {
    MceGameKit.shared.start()
}

@_cdecl("mce_gamekit_sign_in")
public func mce_gamekit_sign_in(_ interactive: Int32) {
    MceGameKit.shared.signIn(interactive: interactive != 0)
}

@_cdecl("mce_gamekit_achievement")
public func mce_gamekit_achievement(_ id: UnsafePointer<CChar>, _ percent: Double, _ tag: UInt16) {
    let achievement = GKAchievement(identifier: String(cString: id))
    achievement.percentComplete = min(percent, 100)
    achievement.showsCompletionBanner = true
    GKAchievement.report([achievement]) { error in send(.achievement, error == nil, tag) }
}

@_cdecl("mce_gamekit_score")
public func mce_gamekit_score(_ id: UnsafePointer<CChar>, _ score: Int64, _ tag: UInt16) {
    let board = String(cString: id)
    GKLeaderboard.submitScore(Int(score), context: 0, player: GKLocalPlayer.local, leaderboardIDs: [board]) { error in
        send(.score, error == nil, tag)
    }
}

@_cdecl("mce_gamekit_read")
public func mce_gamekit_read(_ name: UnsafePointer<CChar>, _ tag: UInt16) {
    newestSave(String(cString: name)) { save, ok in
        guard ok else { return send(.blobRead, false, tag) }
        guard let save = save else { return send(.blobRead, true, tag, missing: true) }
        save.loadData { data, error in
            if error != nil { return send(.blobRead, false, tag) }
            let bytes = data ?? Data()
            send(.blobRead, true, tag, missing: bytes.isEmpty, data: bytes)
        }
    }
}

@_cdecl("mce_gamekit_write")
public func mce_gamekit_write(_ name: UnsafePointer<CChar>, _ bytes: UnsafePointer<UInt8>?, _ size: Int,
                              _ tag: UInt16) {
    let data = bytes.map { Data(bytes: $0, count: size) } ?? Data()
    GKLocalPlayer.local.saveGameData(data, withName: String(cString: name)) { _, error in
        send(.blobWritten, error == nil, tag)
    }
}

@_cdecl("mce_gamekit_show_achievements")
public func mce_gamekit_show_achievements() {
    MceGameKit.shared.show(.achievements)
}

@_cdecl("mce_gamekit_show_leaderboard")
public func mce_gamekit_show_leaderboard(_ id: UnsafePointer<CChar>) {
    MceGameKit.shared.show(.leaderboards, leaderboard: String(cString: id))
}
