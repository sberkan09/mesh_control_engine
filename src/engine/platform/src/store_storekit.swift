// mce_platform: StoreKit 2 for platform/store.h on iOS (built when MCE_STOREKIT is ON).
//
// store.cpp calls the @_cdecl functions below on the main thread; every answer goes back through
// mce_storekit_answer (store.cpp) from a Swift task. The kinds are platform::store::AnswerKind's
// values. A verified transaction is reported as Purchased and left unfinished until
// mce_storekit_finish, so a crash between payment and delivery replays it on the next launch.

import Foundation
import StoreKit

@_silgen_name("mce_storekit_answer")
func mce_storekit_answer(_ kind: Int32, _ request: UInt32, _ id: UnsafePointer<CChar>?,
                         _ price: UnsafePointer<CChar>?, _ transaction: UnsafePointer<CChar>?)

private enum Kind: Int32 {
    case productInfo = 0, productsReady, purchased, pending, cancelled, failed, restored, restoreFinished
}

private func send(_ kind: Kind, _ request: UInt32, id: String = "", price: String = "", transaction: String = "") {
    id.withCString { i in
        price.withCString { p in
            transaction.withCString { t in
                mce_storekit_answer(kind.rawValue, request, i, p, t)
            }
        }
    }
}

@available(iOS 15.0, *)
private final class MceStoreKit {
    static let shared = MceStoreKit()

    private let lock = NSLock()
    private var products: [String: Product] = [:]
    private var open: [String: Transaction] = [:]  // transaction id -> unfinished transaction
    private var updates: Task<Void, Never>?

    func start() {
        updates?.cancel()
        // Purchases completed outside a purchase() call: Ask to Buy approvals, interrupted
        // purchases, renewals. Unfinished ones from an earlier run come through here too.
        updates = Task.detached { [weak self] in
            for await result in Transaction.updates { self?.deliver(result, request: 0) }
        }
        Task.detached { [weak self] in
            for await result in Transaction.unfinished { self?.deliver(result, request: 0) }
        }
    }

    func stop() {
        updates?.cancel()
        updates = nil
    }

    func query(_ ids: [String], request: UInt32) {
        Task.detached { [weak self] in
            do {
                let found = try await Product.products(for: ids)
                for p in found {
                    self?.remember(p)
                    send(.productInfo, request, id: p.id, price: p.displayPrice)
                }
            } catch {
                NSLog("platform.store: StoreKit products query failed: %@", String(describing: error))
            }
            send(.productsReady, request)
        }
    }

    func purchase(_ id: String, request: UInt32) {
        Task.detached { [weak self] in
            guard let self = self else { return send(.failed, request, id: id) }
            var product = self.product(id)
            if product == nil { product = try? await Product.products(for: [id]).first }
            guard let p = product else { return send(.failed, request, id: id) }
            do {
                switch try await p.purchase() {
                case .success(let result): self.deliver(result, request: request)
                case .pending: send(.pending, request, id: id)
                case .userCancelled: send(.cancelled, request, id: id)
                @unknown default: send(.failed, request, id: id)
                }
            } catch {
                send(.failed, request, id: id)
            }
        }
    }

    func restore(request: UInt32) {
        Task.detached {
            try? await AppStore.sync()  // may ask for the Apple ID password; owned items still list without it
            for await result in Transaction.currentEntitlements {
                guard case .verified(let t) = result, t.revocationDate == nil, t.productType == .nonConsumable else { continue }
                send(.restored, request, id: t.productID, transaction: String(t.id))
            }
            send(.restoreFinished, request)
        }
    }

    func finish(_ transaction: String) {
        lock.lock()
        let t = open.removeValue(forKey: transaction)
        lock.unlock()
        guard let t = t else { return }
        Task.detached { await t.finish() }
    }

    private func remember(_ p: Product) {
        lock.lock()
        products[p.id] = p
        lock.unlock()
    }

    private func product(_ id: String) -> Product? {
        lock.lock()
        defer { lock.unlock() }
        return products[id]
    }

    private func deliver(_ result: VerificationResult<Transaction>, request: UInt32) {
        switch result {
        case .verified(let t):
            if t.revocationDate != nil { return }
            let key = String(t.id)
            lock.lock()
            let seen = open[key] != nil
            open[key] = t
            lock.unlock()
            if !seen || request != 0 { send(.purchased, request, id: t.productID, transaction: key) }
        case .unverified(let t, let error):
            NSLog("platform.store: unverified StoreKit transaction for %@: %@", t.productID, String(describing: error))
            send(.failed, request, id: t.productID)
        }
    }
}

@_cdecl("mce_storekit_start")
public func mce_storekit_start() {
    if #available(iOS 15.0, *) { MceStoreKit.shared.start() }
}

@_cdecl("mce_storekit_stop")
public func mce_storekit_stop() {
    if #available(iOS 15.0, *) { MceStoreKit.shared.stop() }
}

@_cdecl("mce_storekit_query")
public func mce_storekit_query(_ ids: UnsafePointer<UnsafePointer<CChar>?>?, _ count: UInt32, _ request: UInt32) {
    var list: [String] = []
    if let ids = ids {
        for i in 0..<Int(count) { if let s = ids[i] { list.append(String(cString: s)) } }
    }
    if #available(iOS 15.0, *) { MceStoreKit.shared.query(list, request: request) } else { send(.productsReady, request) }
}

@_cdecl("mce_storekit_purchase")
public func mce_storekit_purchase(_ id: UnsafePointer<CChar>, _ request: UInt32) {
    let s = String(cString: id)
    if #available(iOS 15.0, *) { MceStoreKit.shared.purchase(s, request: request) } else { send(.failed, request, id: s) }
}

@_cdecl("mce_storekit_restore")
public func mce_storekit_restore(_ request: UInt32) {
    if #available(iOS 15.0, *) { MceStoreKit.shared.restore(request: request) } else { send(.restoreFinished, request) }
}

@_cdecl("mce_storekit_finish")
public func mce_storekit_finish(_ transaction: UnsafePointer<CChar>) {
    let s = String(cString: transaction)
    if #available(iOS 15.0, *) { MceStoreKit.shared.finish(s) }
}
