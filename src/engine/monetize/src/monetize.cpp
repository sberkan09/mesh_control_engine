#include "mce/monetize/monetize.h"

#include <cstring>

#include "mce/ecs/save.h"
#include "mce/log.h"

namespace mce::monetize {

namespace {

constexpr const char* kStage = "monetize";
constexpr u32 kRecordMagic = 0x5A4D434D;       // "MCMZ"
constexpr u32 kEntitlementsMagic = 0x4E45434D;  // "MCEN"
constexpr u16 kFormatVersion = 1;

// The service behind the Store / Ads facades, for the pump system.
struct PumpRef {
    Service* service = nullptr;
};

void pump_system(Frame& f) {
    if (Service* s = f.resource<PumpRef>().service) s->pump(f.world(), f.tick);
}

bool is_fullscreen(AdFormat f) { return f == AdFormat::Rewarded || f == AdFormat::Interstitial; }

bool store_kind_has_product(StoreEventKind k) {
    return k != StoreEventKind::ProductsReady && k != StoreEventKind::RestoreFinished;
}

// ---- little-endian byte codec ----

template <class T>
void put(Array<byte>& out, T v) {
    const usize at = out.size();
    out.resize_uninitialized(at + sizeof(T));
    std::memcpy(out.data() + at, &v, sizeof(T));
}

void put_str(Array<byte>& out, StringView s) {
    put<u8>(out, static_cast<u8>(s.size()));
    for (char c : s) out.push_back(static_cast<byte>(c));
}

struct Reader {
    Span<const byte> in;
    usize pos = 0;
    bool bad = false;

    template <class T>
    T get() {
        T v{};
        if (bad || pos + sizeof(T) > in.size()) {
            bad = true;
            return v;
        }
        std::memcpy(&v, in.data() + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    StringView get_str() {
        const u8 n = get<u8>();
        if (bad || pos + n > in.size()) {
            bad = true;
            return {};
        }
        const StringView s(reinterpret_cast<const char*>(in.data() + pos), n);
        pos += n;
        return s;
    }
};

}  // namespace

// One thing entering the sim: a store or an ad event, plus what the event alone does not carry.
struct Service::Delivery {
    bool is_ad = false;
    StoreEvent store;
    AdEvent ad;
    FixedString<32> price;          // ProductInfo
    FixedString<128> native;        // Purchased consumable: the store's transaction id (never recorded)
};

// ---- Store -------------------------------------------------------------------------------------

bool Store::available() const { return s_ && s_->store_ && s_->store_->available(); }
Span<const ProductInfo> Store::products() const { return s_ ? s_->catalog_.products() : Span<const ProductInfo>(); }
u16 Store::find(StringView product_id) const { return s_ ? s_->catalog_.find_product(product_id) : kNoIndex; }
bool Store::owns(StringView product_id) const { return s_ && s_->owns(find(product_id)); }
bool Store::owns(u16 product) const { return s_ && s_->owns(product); }

StringView Store::price(u16 product) const {
    if (!s_ || product >= s_->prices_.size()) return {};
    return s_->prices_[product].view();
}

u32 Store::query_products() {
    if (!s_) return 0;
    const u32 id = s_->next_request();
    if (!s_->replaying()) s_->store_->query_products(id);
    return id;
}

u32 Store::purchase(StringView product_id) { return purchase(find(product_id)); }

u32 Store::purchase(u16 product) {
    if (!s_ || product >= s_->catalog_.products().size()) return 0;
    if (s_->catalog_.products()[product].type == ProductType::NonConsumable && s_->owns(product)) return 0;
    const u32 id = s_->next_request();
    if (!s_->replaying()) s_->store_->purchase(product, id);
    return id;
}

u32 Store::restore_purchases() {
    if (!s_) return 0;
    const u32 id = s_->next_request();
    if (!s_->replaying()) s_->store_->restore(id);
    return id;
}

void Store::finish(u64 transaction) {
    if (!s_) return;
    Array<Service::Unfinished>& u = s_->unfinished_;
    for (usize i = 0; i < u.size(); ++i) {
        if (u[i].id != transaction) continue;
        if (!s_->replaying() && !u[i].native.empty()) s_->store_->finish(u[i].native.view());
        u.erase(i);
        return;
    }
}

u32 Store::unfinished_count() const { return s_ ? static_cast<u32>(s_->unfinished_.size()) : 0; }

// ---- Ads ---------------------------------------------------------------------------------------

bool Ads::available() const { return s_ && s_->ads_ && s_->ads_->available(); }
Span<const AdPlacementInfo> Ads::placements() const { return s_ ? s_->catalog_.ads() : Span<const AdPlacementInfo>(); }
u16 Ads::find(StringView placement_id) const { return s_ ? s_->catalog_.find_ad(placement_id) : kNoIndex; }

u32 Ads::load(StringView placement_id) {
    const u16 p = find(placement_id);
    if (p == kNoIndex || !is_fullscreen(s_->catalog_.ads()[p].format)) return 0;
    const u32 id = s_->next_request();
    if (!s_->replaying()) s_->ads_->load(p, id);
    return id;
}

u32 Ads::show(StringView placement_id) {
    const u16 p = find(placement_id);
    if (p == kNoIndex || !is_fullscreen(s_->catalog_.ads()[p].format)) return 0;
    const u32 id = s_->next_request();
    s_->ad_ready_[p] = 0;
    if (!s_->replaying()) s_->ads_->show(p, id);
    return id;
}

u32 Ads::show_banner(StringView placement_id, BannerEdge edge) {
    const u16 p = find(placement_id);
    if (p == kNoIndex || s_->catalog_.ads()[p].format != AdFormat::Banner) return 0;
    const u32 id = s_->next_request();
    if (!s_->replaying()) s_->ads_->show_banner(p, id, edge, s_->insets_);
    return id;
}

void Ads::hide_banner() {
    if (s_ && !s_->replaying()) s_->ads_->hide_banner();
}

bool Ads::ready(StringView placement_id) const {
    const u16 p = find(placement_id);
    return p != kNoIndex && s_->ad_ready_[p] != 0;
}

// ---- null providers ----------------------------------------------------------------------------

namespace {

class NullStore final : public IStoreProvider {
public:
    const char* name() const override { return "none"; }
    bool available() const override { return false; }
    void start(const Catalog&, Sink& sink) override { sink_ = &sink; }
    void stop() override { sink_ = nullptr; }
    void query_products(u32 request) override { answer(StoreEventKind::ProductsReady, kNoIndex, request); }
    void purchase(u16 product, u32 request) override { answer(StoreEventKind::Failed, product, request); }
    void restore(u32 request) override { answer(StoreEventKind::RestoreFinished, kNoIndex, request); }
    void finish(StringView) override {}

private:
    void answer(StoreEventKind k, u16 product, u32 request) {
        if (!sink_) return;
        StoreAnswer a;
        a.kind = k;
        a.product = product;
        a.request = request;
        sink_->post(a);
    }
    Sink* sink_ = nullptr;
};

class NullAds final : public IAdProvider {
public:
    const char* name() const override { return "none"; }
    bool available() const override { return false; }
    void start(const Catalog&, Sink& sink) override { sink_ = &sink; }
    void stop() override { sink_ = nullptr; }
    void load(u16 placement, u32 request) override { no_fill(placement, request); }
    void show(u16 placement, u32 request) override { no_fill(placement, request); }
    void show_banner(u16 placement, u32 request, BannerEdge, const SafeInsets&) override { no_fill(placement, request); }
    void hide_banner() override {}

private:
    void no_fill(u16 placement, u32 request) {
        if (!sink_) return;
        AdAnswer a;
        a.kind = AdEventKind::NoFill;
        a.placement = placement;
        a.request = request;
        sink_->post(a);
    }
    Sink* sink_ = nullptr;
};

}  // namespace

IStoreProvider& null_store() {
    static NullStore s;
    return s;
}

IAdProvider& null_ads() {
    static NullAds a;
    return a;
}

// ---- Service -----------------------------------------------------------------------------------

Service::Service(mem::IAllocator& a)
    : alloc_(a), catalog_(a), store_inbox_(a), ad_inbox_(a), inbox_order_(a), owned_(a), prices_(a), ad_ready_(a),
      unfinished_(a) {}

Service::~Service() { shutdown(); }

Result<void> Service::init(const Catalog& catalog, IStoreProvider* store, IAdProvider* ads, const ServiceOptions& opt) {
    shutdown();
    catalog_.clear();
    for (const ProductInfo& p : catalog.products()) catalog_.add_product(p);
    for (const AdPlacementInfo& a : catalog.ads()) catalog_.add_ad(a);
    store_ = store ? store : &null_store();
    ads_ = ads ? ads : &null_ads();
    opt_ = opt;
    owned_.clear();
    prices_.clear();
    ad_ready_.clear();
    for (usize i = 0; i < catalog_.products().size(); ++i) {
        owned_.push_back(0);
        prices_.push_back(FixedString<32>());
    }
    for (usize i = 0; i < catalog_.ads().size(); ++i) ad_ready_.push_back(0);
    unfinished_.clear();
    last_transaction_ = 0;
    last_request_ = 0;
    fullscreen_.store(false, std::memory_order_release);
    if (Result<void> r = load_entitlements(); !r) {
        FixedString<512> why;
        format_error(r.error(), why);
        MCE_LOG_WARN("monetize: the entitlements save was not read ({}); starting with none", why.view());
    }
    store_->start(catalog_, *this);
    ads_->start(catalog_, *this);
    started_ = true;
    MCE_LOG_INFO("monetize: store '{}', ads '{}', {} products, {} ad placements", store_->name(), ads_->name(),
                 catalog_.products().size(), catalog_.ads().size());
    return ok();
}

void Service::shutdown() {
    if (!started_) return;
    store_->stop();
    ads_->stop();
    started_ = false;
    std::lock_guard<std::mutex> lock(inbox_mutex_);
    store_inbox_.clear();
    ad_inbox_.clear();
    inbox_order_.clear();
}

void Service::install(World& world) {
    world.register_event<StoreEvent>(Phase::FixedUpdate_Pre);
    world.register_event<AdEvent>(Phase::FixedUpdate_Pre);
    world.set_resource(Store(this));
    world.set_resource(Ads(this));
    world.set_resource(PumpRef{this});
    world.add_system(Phase::FixedUpdate_Pre, "monetize_pump", &pump_system);
}

void Service::post(const StoreAnswer& a) {
    std::lock_guard<std::mutex> lock(inbox_mutex_);
    store_inbox_.push_back(a);
    inbox_order_.push_back(0);
}

void Service::post(const AdAnswer& a) {
    if (a.placement < catalog_.ads().size() && is_fullscreen(catalog_.ads()[a.placement].format)) {
        if (a.kind == AdEventKind::Shown) fullscreen_.store(true, std::memory_order_release);
        else if (a.kind == AdEventKind::Closed || a.kind == AdEventKind::Failed)
            fullscreen_.store(false, std::memory_order_release);
    }
    std::lock_guard<std::mutex> lock(inbox_mutex_);
    ad_inbox_.push_back(a);
    inbox_order_.push_back(1);
}

void Service::pump(World& world, u64 tick) {
    Array<Delivery> deliveries(alloc_);
    if (replayer_) {
        Replayer& rp = *replayer_;
        while (!rp.corrupt_ && !rp.done()) {
            Reader r{rp.stream_, rp.pos_};
            const u64 at = r.get<u64>();
            if (r.bad) {
                rp.corrupt_ = true;
                break;
            }
            if (at > tick) break;
            const u32 count = r.get<u32>();
            for (u32 i = 0; i < count && !r.bad; ++i) {
                Delivery d;
                d.is_ad = r.get<u8>() != 0;
                if (d.is_ad) {
                    d.ad.kind = static_cast<AdEventKind>(r.get<u8>());
                    d.ad.format = static_cast<AdFormat>(r.get<u8>());
                    d.ad.placement = r.get<u16>();
                    d.ad.request = r.get<u32>();
                    d.ad.reward_amount = r.get<u32>();
                } else {
                    d.store.kind = static_cast<StoreEventKind>(r.get<u8>());
                    d.store.product = r.get<u16>();
                    d.store.request = r.get<u32>();
                    d.store.transaction = r.get<u64>();
                    d.price = r.get_str();
                }
                deliveries.push_back(d);
            }
            if (r.bad) {
                rp.corrupt_ = true;
                deliveries.clear();
                break;
            }
            rp.pos_ = r.pos;
        }
        if (rp.corrupt_) MCE_LOG_ERROR("monetize: the replay stream is corrupt; no more store or ad events");
    } else {
        Array<StoreAnswer> store_in(alloc_);
        Array<AdAnswer> ad_in(alloc_);
        Array<u8> order(alloc_);
        {
            std::lock_guard<std::mutex> lock(inbox_mutex_);
            std::swap(store_in, store_inbox_);
            std::swap(ad_in, ad_inbox_);
            std::swap(order, inbox_order_);
        }
        usize si = 0, ai = 0;
        const usize products = catalog_.products().size();
        for (const u8 which : order) {
            Delivery d;
            if (which == 1) {
                const AdAnswer& a = ad_in[ai++];
                if (a.placement >= catalog_.ads().size()) {
                    MCE_LOG_WARN("monetize: the ad provider answered for placement {} (not in the catalog)", a.placement);
                    continue;
                }
                d.is_ad = true;
                d.ad.kind = a.kind;
                d.ad.format = catalog_.ads()[a.placement].format;
                d.ad.placement = a.placement;
                d.ad.request = a.request;
                d.ad.reward_amount = d.ad.kind == AdEventKind::Rewarded ? a.reward_amount : 0;
            } else {
                const StoreAnswer& a = store_in[si++];
                d.store.kind = a.kind;
                d.store.request = a.request;
                if (store_kind_has_product(a.kind)) {
                    if (a.product >= products) {
                        if (a.kind == StoreEventKind::Failed || a.kind == StoreEventKind::Cancelled) {
                            d.store.product = kNoIndex;  // a refused request for a product the game asked by index
                        } else {
                            MCE_LOG_WARN("monetize: the store answered for product {} (not in the catalog)", a.product);
                            continue;
                        }
                    } else {
                        d.store.product = a.product;
                    }
                }
                if (a.kind == StoreEventKind::ProductInfo) d.price = a.price.view();
                if (a.kind == StoreEventKind::Purchased || a.kind == StoreEventKind::Restored) {
                    const ProductType type = catalog_.products()[a.product].type;
                    if (a.kind == StoreEventKind::Restored && type == ProductType::Consumable) continue;  // stores never restore these
                    d.store.transaction = ++last_transaction_;
                    if (type == ProductType::NonConsumable) {
                        if (!a.transaction.empty()) store_->finish(a.transaction.view());  // owned from here on
                    } else {
                        d.native = a.transaction.view();
                    }
                }
            }
            deliveries.push_back(d);
        }
    }

    if (recorder_ && !replayer_ && !deliveries.empty()) {
        Array<byte>& out = recorder_->bytes_;
        put<u64>(out, tick);
        put<u32>(out, static_cast<u32>(deliveries.size()));
        for (const Delivery& d : deliveries) {
            put<u8>(out, d.is_ad ? 1 : 0);
            if (d.is_ad) {
                put<u8>(out, static_cast<u8>(d.ad.kind));
                put<u8>(out, static_cast<u8>(d.ad.format));
                put<u16>(out, d.ad.placement);
                put<u32>(out, d.ad.request);
                put<u32>(out, d.ad.reward_amount);
            } else {
                put<u8>(out, static_cast<u8>(d.store.kind));
                put<u16>(out, d.store.product);
                put<u32>(out, d.store.request);
                put<u64>(out, d.store.transaction);
                put_str(out, d.price.view());
            }
        }
    }
    for (const Delivery& d : deliveries) deliver(world, d);
}

void Service::deliver(World& world, const Delivery& d) {
    EventAccess events(world);
    if (d.is_ad) {
        if (d.ad.placement < ad_ready_.size()) {
            if (d.ad.kind == AdEventKind::Loaded) ad_ready_[d.ad.placement] = 1;
            else if (d.ad.kind == AdEventKind::Shown || d.ad.kind == AdEventKind::NoFill || d.ad.kind == AdEventKind::Failed)
                ad_ready_[d.ad.placement] = 0;
        }
        events.push(d.ad);
        return;
    }
    const u16 p = d.store.product;
    if (p < owned_.size()) {
        switch (d.store.kind) {
            case StoreEventKind::ProductInfo: prices_[p] = d.price.view(); break;
            case StoreEventKind::Purchased:
            case StoreEventKind::Restored:
                if (catalog_.products()[p].type == ProductType::NonConsumable) {
                    if (!owned_[p]) {
                        owned_[p] = 1;
                        if (!replaying()) persist_entitlements();
                    }
                } else {
                    unfinished_.push_back(Unfinished{d.store.transaction, d.native});
                }
                break;
            default: break;
        }
    }
    events.push(d.store);
}

void Service::set_recorder(Recorder* r) {
    recorder_ = r;
    if (!r) return;
    Array<byte>& out = r->bytes_;
    out.clear();
    put<u32>(out, kRecordMagic);
    put<u16>(out, kFormatVersion);
    put<u16>(out, static_cast<u16>(owned_.size()));
    for (const u8 o : owned_) put<u8>(out, o);
}

void Service::set_replayer(Replayer* r) {
    replayer_ = r;
    if (!r) return;
    Reader rd{r->stream_, 0};
    const u32 magic = rd.get<u32>();
    const u16 version = rd.get<u16>();
    const u16 count = rd.get<u16>();
    if (rd.bad || magic != kRecordMagic || version != kFormatVersion || count != owned_.size()) {
        MCE_LOG_ERROR("monetize: the replay stream does not match this catalog; no store or ad events will replay");
        r->corrupt_ = true;
        return;
    }
    for (u16 i = 0; i < count; ++i) owned_[i] = rd.get<u8>();
    for (FixedString<32>& p : prices_) p.clear();
    for (u8& a : ad_ready_) a = 0;
    unfinished_.clear();
    last_request_ = 0;
    last_transaction_ = 0;
    r->pos_ = rd.pos;
    r->corrupt_ = rd.bad;
}

void Service::persist_entitlements() {
    if (opt_.entitlements_save.empty()) return;
    Array<byte> out(alloc_);
    put<u32>(out, kEntitlementsMagic);
    put<u16>(out, kFormatVersion);
    u16 n = 0;
    for (const u8 o : owned_) n = static_cast<u16>(n + (o ? 1 : 0));
    put<u16>(out, n);
    for (usize i = 0; i < owned_.size(); ++i)
        if (owned_[i]) put_str(out, catalog_.products()[i].id.view());
    if (Result<void> r = save::write(opt_.entitlements_save.view(), out.span()); !r) {
        FixedString<512> why;
        format_error(r.error(), why);
        MCE_LOG_ERROR("monetize: the entitlements were not saved: {}", why.view());
    }
}

Result<void> Service::load_entitlements() {
    if (opt_.entitlements_save.empty() || !save::exists(opt_.entitlements_save.view())) return ok();
    Array<byte> bytes(alloc_);
    MCE_TRY(save::read(opt_.entitlements_save.view(), bytes));
    Reader r{bytes.span()};
    const u32 magic = r.get<u32>();
    const u16 version = r.get<u16>();
    const u16 n = r.get<u16>();
    if (r.bad || magic != kEntitlementsMagic) return Error(Errc::BadMagic, kStage, opt_.entitlements_save.view());
    if (version > kFormatVersion) return Error(Errc::VersionTooNew, kStage, opt_.entitlements_save.view());
    for (u16 i = 0; i < n; ++i) {
        const StringView id = r.get_str();
        if (r.bad) return Error(Errc::Truncated, kStage, opt_.entitlements_save.view());
        const u16 p = catalog_.find_product(id);
        if (p != kNoIndex && catalog_.products()[p].type == ProductType::NonConsumable) owned_[p] = 1;
    }
    return ok();
}

}  // namespace mce::monetize
