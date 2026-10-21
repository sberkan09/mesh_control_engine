#pragma once
// mce_platform (L1): the phone's own in-app purchase store (M-MONETIZE step 3).
//
// The store wall (check_walls.py): StoreKit, the Play Billing library and JNI live only in
// mce_platform and the stamped Android project. This header speaks product ids and transaction ids
// as strings; mce_monetize's PlatformStore provider maps them to its catalog.
//
// - Android: Play Billing 7 through org.meshcontrol.billing.MceBilling, stamped into the Gradle
//   project of a game with a store catalog (cmake/mobile/android_app.py); the calls cross JNI.
// - iOS: StoreKit 2 (store_storekit.swift), built when MCE_STOREKIT is ON.
// - Everywhere else, and on a phone build without the Java or Swift side: backend() is None and
//   every call is answered Failed (queries and restores also get their closing answer).
//
// Calls are made on the main thread. Answers arrive through the handler, on any thread (the
// store's own), so the handler must be thread-safe; mce_monetize's sink is.

#include "mce/types.h"

namespace mce::platform::store {

enum class Backend : u8 { None, PlayBilling, StoreKit };

enum class AnswerKind : u8 {
    ProductInfo,      // a product the store knows: id, price
    ProductsReady,    // the query is answered
    Purchased,        // id, transaction (Play: the purchase token; StoreKit: the transaction id)
    Pending,          // id: waiting on the payer (Ask to Buy, a cash payment)
    Cancelled,        // id
    Failed,           // id (may be empty)
    Restored,         // an owned non-consumable: id, transaction
    RestoreFinished,  // the restore is answered
};

struct Answer {
    AnswerKind kind = AnswerKind::Failed;
    u32 request = 0;  // 0: unsolicited (a purchase finished after a restart, a pending one completing)
    StringView id;           // valid during the call only
    StringView price;        // ProductInfo: the localized display price
    StringView transaction;  // Purchased / Restored
};

using AnswerFn = void (*)(void* user, const Answer& a);

Backend backend();
const char* backend_name(Backend b);  // "none", "play", "storekit"

// Connects to the store; answers go to `fn` until stop(). False when backend() is None (the
// handler is kept anyway, so calls still get their Failed answers).
bool start(AnswerFn fn, void* user);
void stop();

// One ProductInfo per id the store knows, then ProductsReady.
void query_products(const StringView* ids, u32 count, u32 request);
// Purchased, Pending, Cancelled or Failed for `id`.
void purchase(StringView id, bool consumable, u32 request);
// One Restored per owned non-consumable, then RestoreFinished.
void restore(u32 request);
// Completes a delivered purchase: Play consumes a consumable and acknowledges the rest; StoreKit
// finishes the transaction. Unfinished purchases come back after a restart (request 0).
void finish(StringView transaction, bool consumable);

}  // namespace mce::platform::store
