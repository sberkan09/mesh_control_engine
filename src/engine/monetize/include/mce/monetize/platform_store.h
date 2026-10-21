#pragma once
// The phone's own store as a provider (M-MONETIZE step 3): Play Billing on Android, StoreKit 2 on
// iOS, through mce_platform's store (mce/platform/store.h), the only place those SDKs are seen.
// Products go to the store by their ProductInfo::android_id or ios_id and come back mapped to the
// catalog; a product without one is not sold on that platform (its purchases fail). Off a phone, or on one built without the store's Java
// or Swift side, available() is false and every request is refused, like the null store.
//
// Answers arrive on the store's own threads and go straight into the Service's inbox.

#include <mutex>

#include "mce/monetize/monetize.h"

namespace mce::platform::store {
struct Answer;
}

namespace mce::monetize {

class PlatformStore final : public IStoreProvider {
public:
    explicit PlatformStore(mem::IAllocator& a = mem::heap());
    ~PlatformStore() override;
    const char* name() const override;
    bool available() const override;
    void start(const Catalog& catalog, Sink& sink) override;
    void stop() override;
    void query_products(u32 request) override;
    void purchase(u16 product, u32 request) override;
    void restore(u32 request) override;
    void finish(StringView transaction) override;

    // The id the store knows `product` by (tests).
    StringView store_id(u16 product) const;
    // One answer from mce_platform's store (any thread), mapped to the catalog and posted.
    void answer(const platform::store::Answer& a);

private:
    struct Open {
        FixedString<512> transaction;
        bool consumable;
    };
    mem::IAllocator& alloc_;
    const Catalog* catalog_ = nullptr;
    Sink* sink_ = nullptr;
    bool started_ = false;
    std::mutex open_mutex_;
    Array<Open> open_;  // purchases reported and not finished: how to finish each
};

}  // namespace mce::monetize
