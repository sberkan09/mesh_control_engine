#pragma once
// The fake store and the fake ad network (M-MONETIZE step 2): providers with no SDK, account or
// network behind them, so purchase and ad flows run on a desktop, in the editor and in
// scripts/ci/local_ci.sh. They answer through the same Sink as a real provider, from
// Service::update() (the main thread), so a headless run is deterministic.
//
// Behaviour comes from FakeConfig, which fake_config_from_convars() fills from the command line:
//
//   -monetize.fake.purchase success|cancel|pending|failure   how every purchase ends (default success)
//   -monetize.fake.pending_updates N   a pending purchase completes (Purchased) after N updates (default 60)
//   -monetize.fake.owned "a,b"         non-consumables the fake account already owns (restore finds them)
//   -monetize.fake.price "$0.99"       the price every product reports
//   -monetize.fake.fill fill|nofill|failure   how every ad request ends (default fill)
//   -monetize.fake.close reward|close  a rewarded ad watched to the end, or closed early (default reward)
//   -monetize.fake.ad_updates N        updates a fullscreen ad stays up before it closes (default 30)
//   -monetize.fake.reward N            what a rewarded ad is worth (default 1)
//   -monetize.fake.delay N             updates before each answer (default 1: the next update)
//
// The fake account remembers what it sold during the run, so restore_purchases finds those too.

#include "mce/monetize/monetize.h"

namespace mce::monetize {

enum class FakePurchase : u8 { Success, Cancel, Pending, Failure };
enum class FakeFill : u8 { Fill, NoFill, Failure };

struct FakeConfig {
    FakePurchase purchase = FakePurchase::Success;
    u32 pending_updates = 60;
    FixedString<256> owned;  // comma-separated product ids
    FixedString<32> price = "$0.99";
    FakeFill fill = FakeFill::Fill;
    bool rewarded_completes = true;  // false: the player closes a rewarded ad early (no Rewarded)
    u32 ad_updates = 30;
    u32 reward = 1;
    u32 delay = 1;
};

// The -monetize.fake.* convars; false (with `out` holding the defaults for the bad ones) when one
// has a value it does not know, which the host reports.
bool fake_config_from_convars(FakeConfig& out);

class FakeStore final : public IStoreProvider {
public:
    explicit FakeStore(const FakeConfig& cfg = {}, mem::IAllocator& a = mem::heap());
    const char* name() const override { return "fake"; }
    void start(const Catalog& catalog, Sink& sink) override;
    void stop() override;
    void update() override;
    void query_products(u32 request) override;
    void purchase(u16 product, u32 request) override;
    void restore(u32 request) override;
    void finish(StringView transaction) override;

    void set_config(const FakeConfig& cfg) { cfg_ = cfg; }
    // Transactions the game has not finished yet (the fake's view; tests).
    u32 unfinished() const { return static_cast<u32>(open_.size()); }

private:
    struct Pending {
        u32 due;
        StoreAnswer answer;
    };
    void later(u32 updates, const StoreAnswer& a);

    FakeConfig cfg_;
    const Catalog* catalog_ = nullptr;
    Sink* sink_ = nullptr;
    Array<Pending> queue_;
    Array<u8> owned_;                    // per product: the fake account owns it
    Array<FixedString<512>> open_;       // transactions not finished yet
    u32 now_ = 0;
    u32 next_transaction_ = 0;
};

class FakeAds final : public IAdProvider {
public:
    explicit FakeAds(const FakeConfig& cfg = {}, mem::IAllocator& a = mem::heap());
    const char* name() const override { return "fake"; }
    void start(const Catalog& catalog, Sink& sink) override;
    void stop() override;
    void update() override;
    void load(u16 placement, u32 request) override;
    void show(u16 placement, u32 request) override;
    void show_banner(u16 placement, u32 request, BannerEdge edge, const SafeInsets& insets) override;
    void hide_banner() override;

    void set_config(const FakeConfig& cfg) { cfg_ = cfg; }
    // The banner on screen (kNoIndex: none) and where it was asked to go (tests).
    u16 banner() const { return banner_; }
    SafeInsets banner_insets() const { return banner_insets_; }

private:
    struct Pending {
        u32 due;
        AdAnswer answer;
    };
    void later(u32 updates, AdEventKind kind, u16 placement, u32 request, u32 reward = 0);
    bool fail_request(u16 placement, u32 request);

    FakeConfig cfg_;
    const Catalog* catalog_ = nullptr;
    Sink* sink_ = nullptr;
    Array<Pending> queue_;
    u32 now_ = 0;
    u16 banner_ = kNoIndex;
    SafeInsets banner_insets_;
};

}  // namespace mce::monetize
