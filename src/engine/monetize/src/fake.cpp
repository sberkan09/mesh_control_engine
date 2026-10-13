#include "mce/monetize/fake.h"

#include <cstdio>

#include "mce/convar.h"
#include "mce/log.h"

namespace mce::monetize {

namespace {

MCE_CVAR(cv_purchase, "monetize.fake.purchase", "success",
         "Fake store: how every purchase ends (success, cancel, pending or failure)", CVarFlags::None);
MCE_CVAR(cv_pending_updates, "monetize.fake.pending_updates", 60,
         "Fake store: updates before a pending purchase completes", CVarFlags::None);
MCE_CVAR(cv_owned, "monetize.fake.owned", "",
         "Fake store: comma-separated non-consumables the fake account already owns (restore finds them)", CVarFlags::None);
MCE_CVAR(cv_price, "monetize.fake.price", "$0.99", "Fake store: the price every product reports", CVarFlags::None);
MCE_CVAR(cv_fill, "monetize.fake.fill", "fill", "Fake ads: how every ad request ends (fill, nofill or failure)",
         CVarFlags::None);
MCE_CVAR(cv_close, "monetize.fake.close", "reward",
         "Fake ads: a rewarded ad is watched to the end (reward) or closed early (close)", CVarFlags::None);
MCE_CVAR(cv_ad_updates, "monetize.fake.ad_updates", 30, "Fake ads: updates a fullscreen ad stays up", CVarFlags::None);
MCE_CVAR(cv_reward, "monetize.fake.reward", 1, "Fake ads: what a rewarded ad is worth", CVarFlags::None);
MCE_CVAR(cv_delay, "monetize.fake.delay", 1, "Fake store and ads: updates before each answer", CVarFlags::None);

u32 non_negative(i32 v) { return v < 0 ? 0u : static_cast<u32>(v); }

bool is_fullscreen(AdFormat f) { return f != AdFormat::Banner; }

}  // namespace

bool fake_config_from_convars(FakeConfig& out) {
    out = FakeConfig{};
    bool ok_all = true;
    const StringView purchase = cv_purchase.get_string();
    if (purchase == "success") out.purchase = FakePurchase::Success;
    else if (purchase == "cancel") out.purchase = FakePurchase::Cancel;
    else if (purchase == "pending") out.purchase = FakePurchase::Pending;
    else if (purchase == "failure") out.purchase = FakePurchase::Failure;
    else ok_all = false;
    const StringView fill = cv_fill.get_string();
    if (fill == "fill") out.fill = FakeFill::Fill;
    else if (fill == "nofill") out.fill = FakeFill::NoFill;
    else if (fill == "failure") out.fill = FakeFill::Failure;
    else ok_all = false;
    const StringView close = cv_close.get_string();
    if (close == "reward") out.rewarded_completes = true;
    else if (close == "close") out.rewarded_completes = false;
    else ok_all = false;
    out.pending_updates = non_negative(cv_pending_updates.get_int());
    out.owned = cv_owned.get_string();
    out.price = cv_price.get_string();
    out.ad_updates = non_negative(cv_ad_updates.get_int());
    out.reward = non_negative(cv_reward.get_int());
    out.delay = non_negative(cv_delay.get_int());
    return ok_all;
}

// ---- FakeStore ---------------------------------------------------------------------------------

FakeStore::FakeStore(const FakeConfig& cfg, mem::IAllocator& a) : cfg_(cfg), queue_(a), owned_(a), open_(a) {}

void FakeStore::start(const Catalog& catalog, Sink& sink) {
    catalog_ = &catalog;
    sink_ = &sink;
    queue_.clear();
    open_.clear();
    owned_.clear();
    for (usize i = 0; i < catalog.products().size(); ++i) owned_.push_back(0);
    StringView rest = cfg_.owned.view();
    while (!rest.empty()) {
        usize comma = 0;
        while (comma < rest.size() && rest[comma] != ',') ++comma;
        StringView id = rest.substr(0, comma);
        while (!id.empty() && id[0] == ' ') id = id.substr(1);
        while (!id.empty() && id[id.size() - 1] == ' ') id = id.substr(0, id.size() - 1);
        const u16 p = catalog.find_product(id);
        if (p != kNoIndex && catalog.products()[p].type == ProductType::NonConsumable) owned_[p] = 1;
        else if (!id.empty()) MCE_LOG_WARN("monetize: -monetize.fake.owned names '{}', not a non-consumable of the catalog", id);
        rest = comma < rest.size() ? rest.substr(comma + 1) : StringView();
    }
}

void FakeStore::stop() {
    sink_ = nullptr;
    catalog_ = nullptr;
    queue_.clear();
}

void FakeStore::later(u32 updates, const StoreAnswer& a) {
    if (!sink_) return;
    queue_.push_back(Pending{now_ + updates, a});
}

void FakeStore::update() {
    ++now_;
    // In request order; a later answer never overtakes an earlier one that is due the same update.
    usize kept = 0;
    for (usize i = 0; i < queue_.size(); ++i) {
        if (queue_[i].due <= now_) {
            if (sink_) sink_->post(queue_[i].answer);
        } else {
            queue_[kept++] = queue_[i];
        }
    }
    queue_.resize(kept);
}

void FakeStore::query_products(u32 request) {
    if (!catalog_) return;
    for (usize i = 0; i < catalog_->products().size(); ++i) {
        StoreAnswer a;
        a.kind = StoreEventKind::ProductInfo;
        a.product = static_cast<u16>(i);
        a.request = request;
        a.price = cfg_.price.view();
        later(cfg_.delay, a);
    }
    StoreAnswer done;
    done.kind = StoreEventKind::ProductsReady;
    done.request = request;
    later(cfg_.delay, done);
}

void FakeStore::purchase(u16 product, u32 request) {
    StoreAnswer a;
    a.product = product;
    a.request = request;
    if (!catalog_ || product >= catalog_->products().size()) {
        a.kind = StoreEventKind::Failed;
        later(cfg_.delay, a);
        return;
    }
    u32 when = cfg_.delay;
    switch (cfg_.purchase) {
        case FakePurchase::Cancel: a.kind = StoreEventKind::Cancelled; break;
        case FakePurchase::Failure: a.kind = StoreEventKind::Failed; break;
        case FakePurchase::Pending: {
            StoreAnswer p = a;
            p.kind = StoreEventKind::Pending;
            later(cfg_.delay, p);
            when = cfg_.delay + cfg_.pending_updates;
            [[fallthrough]];
        }
        case FakePurchase::Success: {
            a.kind = StoreEventKind::Purchased;
            char tx[32];
            std::snprintf(tx, sizeof(tx), "fake-%u", ++next_transaction_);
            a.transaction = StringView(tx);
            open_.push_back(a.transaction);
            if (catalog_->products()[product].type == ProductType::NonConsumable) owned_[product] = 1;
            break;
        }
    }
    later(when, a);
}

void FakeStore::restore(u32 request) {
    for (usize i = 0; i < owned_.size(); ++i) {
        if (!owned_[i]) continue;
        StoreAnswer a;
        a.kind = StoreEventKind::Restored;
        a.product = static_cast<u16>(i);
        a.request = request;
        char tx[40];
        std::snprintf(tx, sizeof(tx), "fake-restore-%u", static_cast<unsigned>(i));
        a.transaction = StringView(tx);
        later(cfg_.delay, a);
    }
    StoreAnswer done;
    done.kind = StoreEventKind::RestoreFinished;
    done.request = request;
    later(cfg_.delay, done);
}

void FakeStore::finish(StringView transaction) {
    for (usize i = 0; i < open_.size(); ++i) {
        if (open_[i].view() != transaction) continue;
        open_.erase(i);
        return;
    }
}

// ---- FakeAds -----------------------------------------------------------------------------------

FakeAds::FakeAds(const FakeConfig& cfg, mem::IAllocator& a) : cfg_(cfg), queue_(a) {}

void FakeAds::start(const Catalog& catalog, Sink& sink) {
    catalog_ = &catalog;
    sink_ = &sink;
    queue_.clear();
    banner_ = kNoIndex;
}

void FakeAds::stop() {
    sink_ = nullptr;
    catalog_ = nullptr;
    queue_.clear();
}

void FakeAds::later(u32 updates, AdEventKind kind, u16 placement, u32 request, u32 reward) {
    if (!sink_) return;
    AdAnswer a;
    a.kind = kind;
    a.placement = placement;
    a.request = request;
    a.reward_amount = reward;
    queue_.push_back(Pending{now_ + updates, a});
}

void FakeAds::update() {
    ++now_;
    usize kept = 0;
    for (usize i = 0; i < queue_.size(); ++i) {
        if (queue_[i].due <= now_) {
            if (sink_) sink_->post(queue_[i].answer);
        } else {
            queue_[kept++] = queue_[i];
        }
    }
    queue_.resize(kept);
}

// NoFill / Failed per the config; true when the request ends there.
bool FakeAds::fail_request(u16 placement, u32 request) {
    if (!catalog_ || placement >= catalog_->ads().size()) {
        later(cfg_.delay, AdEventKind::Failed, placement, request);
        return true;
    }
    if (cfg_.fill == FakeFill::NoFill) {
        later(cfg_.delay, AdEventKind::NoFill, placement, request);
        return true;
    }
    if (cfg_.fill == FakeFill::Failure) {
        later(cfg_.delay, AdEventKind::Failed, placement, request);
        return true;
    }
    return false;
}

void FakeAds::load(u16 placement, u32 request) {
    if (!fail_request(placement, request)) later(cfg_.delay, AdEventKind::Loaded, placement, request);
}

void FakeAds::show(u16 placement, u32 request) {
    if (fail_request(placement, request)) return;
    const AdFormat format = catalog_->ads()[placement].format;
    if (!is_fullscreen(format)) {
        later(cfg_.delay, AdEventKind::Failed, placement, request);
        return;
    }
    const u32 shown = cfg_.delay;
    const u32 closed = shown + (cfg_.ad_updates > 0 ? cfg_.ad_updates : 1);
    later(shown, AdEventKind::Shown, placement, request);
    if (format == AdFormat::Rewarded && cfg_.rewarded_completes)
        later(closed, AdEventKind::Rewarded, placement, request, cfg_.reward);
    later(closed, AdEventKind::Closed, placement, request);
}

void FakeAds::show_banner(u16 placement, u32 request, BannerEdge, const SafeInsets& insets) {
    if (fail_request(placement, request)) return;
    banner_ = placement;
    banner_insets_ = insets;
    later(cfg_.delay, AdEventKind::Loaded, placement, request);
    later(cfg_.delay, AdEventKind::Shown, placement, request);
}

void FakeAds::hide_banner() {
    if (banner_ == kNoIndex) return;
    later(cfg_.delay, AdEventKind::Closed, banner_, 0);
    banner_ = kNoIndex;
}

}  // namespace mce::monetize
