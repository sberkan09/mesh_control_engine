#pragma once
// In-app purchases and ads (M-MONETIZE step 1, issues #141 and #142): one engine API over every
// store and ad network, so a game only decides where to sell and where to show an ad.
//
// Off by default: a project turns it on in its host (player::PlayerDesc::monetize = true). The
// player then installs a Service with the pack's store catalog (catalog.h) and the platform's
// providers; without it the Store and Ads resources below do not exist (try_resource is null).
//
// Game code (FixedUpdate systems):
//
//   if (monetize::Store* store = f.try_resource<monetize::Store>()) {
//       if (!store->owns("remove_ads") && pressed_buy) store->purchase("remove_ads");
//       for (const monetize::StoreEvent& e : f.events().read<monetize::StoreEvent>())
//           if (e.kind == monetize::StoreEventKind::Purchased && e.product == store->find("gems_100")) {
//               add_gems(100);              // grant first ...
//               store->finish(e.transaction);  // ... then tell the store (consumables only)
//           }
//   }
//   if (monetize::Ads* ads = f.try_resource<monetize::Ads>()) ads->show("level_end");
//
// Determinism (FR-RUN-040): a store or ad network answers on its own thread whenever it likes.
// Answers wait in the Service's inbox and enter the sim only at the start of a tick, in arrival
// order, as StoreEvent / AdEvent (pushed by the monetize_pump system, the first FixedUpdate_Pre
// system: readable from FixedUpdate_Physics on in the same tick, by FixedUpdate_Pre systems on the
// next). A Recorder captures exactly what entered at which tick and a Replayer feeds it back with
// no provider at all, so replays hash the same with purchases and ads in them.
//
// Entitlements: a bought or restored non-consumable is owned from the tick its event enters, is
// written to the local "entitlements" save (save::write, atomic with a .bak) and is still owned
// after a relaunch with no network. The service finishes non-consumable transactions itself. A
// consumable stays unfinished (the store redelivers it on the next launch) until the game calls
// Store::finish after granting it, so a crash between payment and grant loses nothing.
//
// Fullscreen ads: while a rewarded or interstitial ad covers the screen the player pauses the sim
// and the audio exactly like the phone background path (FR-RUN-100) and resumes on its own when
// the ad closes. fullscreen_ad_showing() flips the moment the provider reports Shown / Closed, not
// at a tick, because the sim is not running in between.
//
// Banners sit along the top or bottom edge inside the safe-area insets the host passes in
// (set_safe_insets), so a notch or home indicator never covers them.
//
// Server-side receipt validation is not done here; a game that needs it sends the transaction to
// its own server from the Purchased event before calling finish (user guide, monetization chapter).

#include <atomic>
#include <mutex>

#include "mce/ecs/world.h"
#include "mce/monetize/catalog.h"

namespace mce::monetize {

class Service;

// ---- what enters the sim ------------------------------------------------------------------

enum class StoreEventKind : u8 {
    ProductInfo,      // a product's localized price arrived (Store::price)
    ProductsReady,    // the query_products answer is complete
    Purchased,        // paid; a consumable waits for Store::finish
    Pending,          // waiting on someone else (parental approval, a slow payment method)
    Cancelled,        // the player backed out
    Failed,           // the store refused or is unavailable; nothing was charged
    Restored,         // a non-consumable came back from restore_purchases (owned again)
    RestoreFinished,  // restore_purchases is complete
};

struct StoreEvent {
    StoreEventKind kind = StoreEventKind::Failed;
    u8 pad_ = 0;
    u16 product = kNoIndex;  // catalog index (Store::find); kNoIndex for ProductsReady / RestoreFinished
    u32 request = 0;         // the id query_products / purchase / restore_purchases returned; 0 = unsolicited
    u64 transaction = 0;     // Purchased / Restored: pass it to Store::finish (consumables)
};

enum class AdEventKind : u8 {
    Loaded,    // ready to show without a wait
    NoFill,    // the network has nothing to show; try again later
    Shown,     // on screen (fullscreen: the sim is paused until Closed)
    Rewarded,  // a rewarded ad was watched far enough: grant the reward now
    Closed,    // gone; the game continues
    Failed,    // could not load or show
};

struct AdEvent {
    AdEventKind kind = AdEventKind::Failed;
    AdFormat format = AdFormat::Interstitial;
    u16 placement = kNoIndex;  // catalog index (Ads::find)
    u32 request = 0;           // the id load / show returned
    u32 reward_amount = 0;     // Rewarded: what the network says the ad is worth (often 1)
};

const char* store_event_name(StoreEventKind k);  // "Purchased"
const char* ad_event_name(AdEventKind k);        // "Rewarded"

// ---- the game's side: world resources -------------------------------------------------------

class Store {
public:
    explicit Store(Service* s = nullptr) : s_(s) {}
    // False when the platform has no store (desktop without a fake provider): purchase fails.
    bool available() const;
    Span<const ProductInfo> products() const;
    u16 find(StringView product_id) const;  // kNoIndex when the catalog has no such product
    // Owned non-consumables (the entitlements). Unknown ids are never owned.
    bool owns(StringView product_id) const;
    bool owns(u16 product) const;
    // Localized price text ("$0.99", "₺34,99"), empty until its ProductInfo event.
    StringView price(u16 product) const;

    // Requests: each returns a request id that comes back in the answering events (0 = rejected
    // at once: unknown product, or a non-consumable already owned).
    u32 query_products();
    u32 purchase(StringView product_id);
    u32 purchase(u16 product);
    u32 restore_purchases();
    // Consumable granted: the store may forget the transaction. Unknown ids are ignored.
    void finish(u64 transaction);
    // Consumable transactions bought but not finished yet (also the ones the store redelivered).
    u32 unfinished_count() const;

private:
    Service* s_;
};

enum class BannerEdge : u8 { Bottom, Top };

class Ads {
public:
    explicit Ads(Service* s = nullptr) : s_(s) {}
    bool available() const;  // false: every request ends in NoFill
    Span<const AdPlacementInfo> placements() const;
    u16 find(StringView placement_id) const;
    // Preload a rewarded or interstitial ad (Loaded / NoFill). show() loads on demand too, slower.
    u32 load(StringView placement_id);
    // A rewarded or interstitial placement: fullscreen, pauses the sim until Closed.
    u32 show(StringView placement_id);
    // A banner placement, kept inside the safe area until hide_banner.
    u32 show_banner(StringView placement_id, BannerEdge edge = BannerEdge::Bottom);
    void hide_banner();
    // Advisory: a fullscreen placement reported Loaded and has not been shown since.
    bool ready(StringView placement_id) const;

private:
    Service* s_;
};

// ---- the platform's side: providers ---------------------------------------------------------

// What a provider reports, from any thread. Product and placement indices are catalog indices.
struct StoreAnswer {
    StoreEventKind kind = StoreEventKind::Failed;
    u16 product = kNoIndex;
    u32 request = 0;
    FixedString<128> transaction;  // the store's own transaction id (Purchased / Restored)
    FixedString<32> price;         // ProductInfo
};

struct AdAnswer {
    AdEventKind kind = AdEventKind::Failed;
    u16 placement = kNoIndex;
    u32 request = 0;
    u32 reward_amount = 0;
};

class Sink {
public:
    virtual void post(const StoreAnswer& a) = 0;
    virtual void post(const AdAnswer& a) = 0;

protected:
    ~Sink() = default;
};

// A store (StoreKit 2, Play Billing, the fake). Called on the main thread; answers through the
// sink on any thread, now or later. Every request is answered at least once.
class IStoreProvider {
public:
    virtual ~IStoreProvider() = default;
    virtual const char* name() const = 0;
    virtual bool available() const { return true; }
    virtual void start(const Catalog& catalog, Sink& sink) = 0;
    virtual void stop() {}
    // Service::update, once per host tick, paused or not (providers that poll or time things out).
    virtual void update() {}
    // One ProductInfo per product the store knows, then ProductsReady.
    virtual void query_products(u32 request) = 0;
    virtual void purchase(u16 product, u32 request) = 0;
    // One Restored per owned non-consumable, then RestoreFinished.
    virtual void restore(u32 request) = 0;
    virtual void finish(StringView transaction) = 0;
};

struct SafeInsets {
    u32 left = 0, top = 0, right = 0, bottom = 0;  // device pixels
};

// An ad network (Google Mobile Ads, the fake). Same threading as IStoreProvider.
class IAdProvider {
public:
    virtual ~IAdProvider() = default;
    virtual const char* name() const = 0;
    virtual bool available() const { return true; }
    virtual void start(const Catalog& catalog, Sink& sink) = 0;
    virtual void stop() {}
    virtual void update() {}  // as IStoreProvider::update
    virtual void load(u16 placement, u32 request) = 0;
    virtual void show(u16 placement, u32 request) = 0;
    virtual void show_banner(u16 placement, u32 request, BannerEdge edge, const SafeInsets& insets) = 0;
    virtual void hide_banner() = 0;
};

// No store and no ads (the default where the platform has none): purchases fail, restore finds
// nothing, every ad is NoFill.
IStoreProvider& null_store();
IAdProvider& null_ads();

// ---- record / replay ------------------------------------------------------------------------

// Everything that entered the sim, by tick, plus the entitlements owned when recording started.
class Recorder {
public:
    explicit Recorder(mem::IAllocator& a = mem::heap()) : bytes_(a) {}
    Span<const byte> bytes() const { return bytes_.span(); }
    void clear() { bytes_.clear(); }

private:
    friend class Service;
    Array<byte> bytes_;
};

class Replayer {
public:
    // `stream` (Recorder bytes) must outlive the replayer.
    explicit Replayer(Span<const byte> stream) : stream_(stream) {}
    bool done() const { return pos_ >= stream_.size(); }
    bool corrupt() const { return corrupt_; }

private:
    friend class Service;
    Span<const byte> stream_;
    usize pos_ = 0;
    bool corrupt_ = false;
};

// ---- the host's side ---------------------------------------------------------------------------

struct ServiceOptions {
    // save::write name of the entitlements file (under save::root()); empty: not persisted.
    FixedString<64> entitlements_save = "entitlements";
};

class Service final : public Sink {
public:
    explicit Service(mem::IAllocator& a = mem::heap());
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    // Null providers: null_store() / null_ads(). Loads the persisted entitlements (a missing file
    // is not an error) and starts the providers. The catalog is copied.
    Result<void> init(const Catalog& catalog, IStoreProvider* store, IAdProvider* ads, const ServiceOptions& opt = {});
    void shutdown();
    // Registers StoreEvent / AdEvent, sets the Store and Ads resources and adds the monetize_pump
    // system. Install before the game module so the pump runs first.
    void install(World& world);

    // Main thread, once per host tick, also while the sim is paused (a fullscreen ad must be able to
    // close): lets the providers poll and time things out. Not called while replaying.
    void update();

    // Main thread; the host keeps it current (the window's safe-area insets).
    void set_safe_insets(const SafeInsets& s) { insets_ = s; }
    // True from the provider's Shown of a fullscreen ad until its Closed (any thread).
    bool fullscreen_ad_showing() const { return fullscreen_.load(std::memory_order_acquire); }

    // Record what enters the sim (null stops). Starting a recording writes the owned entitlements.
    void set_recorder(Recorder* r);
    // Replay instead of the providers (null returns to them): the recorded entitlements replace
    // the current ones, nothing is persisted, no provider is called.
    void set_replayer(Replayer* r);

    // Sink (providers, any thread).
    void post(const StoreAnswer& a) override;
    void post(const AdAnswer& a) override;

    // Moves the inbox into the sim (the monetize_pump system calls it).
    void pump(World& world, u64 tick);

    const Catalog& catalog() const { return catalog_; }
    bool owns(u16 product) const { return product < owned_.size() && owned_[product] != 0; }

private:
    friend class Store;
    friend class Ads;

    struct Delivery;
    void deliver(World& world, const Delivery& d);
    void persist_entitlements();
    Result<void> load_entitlements();
    u32 next_request() { return ++last_request_; }
    bool replaying() const { return replayer_ != nullptr; }

    mem::IAllocator& alloc_;
    Catalog catalog_;
    IStoreProvider* store_ = nullptr;
    IAdProvider* ads_ = nullptr;
    ServiceOptions opt_;
    SafeInsets insets_;
    bool started_ = false;

    // Inbox (any thread -> main).
    std::mutex inbox_mutex_;
    Array<StoreAnswer> store_inbox_;
    Array<AdAnswer> ad_inbox_;
    Array<u8> inbox_order_;  // 0 = store, 1 = ad, in arrival order
    std::atomic<bool> fullscreen_{false};

    // Sim-side state (main thread).
    Array<u8> owned_;                    // per product
    Array<FixedString<32>> prices_;      // per product
    Array<u8> ad_ready_;                 // per placement
    struct Unfinished {
        u64 id;
        FixedString<128> native;
    };
    Array<Unfinished> unfinished_;
    u64 last_transaction_ = 0;
    u32 last_request_ = 0;

    Recorder* recorder_ = nullptr;
    Replayer* replayer_ = nullptr;
};

}  // namespace mce::monetize
