#include "mce/monetize/platform_store.h"

#include "mce/log.h"
#include "mce/platform/store.h"

namespace mce::monetize {

namespace {

StoreEventKind kind_of(platform::store::AnswerKind k) {
    using K = platform::store::AnswerKind;
    switch (k) {
        case K::ProductInfo: return StoreEventKind::ProductInfo;
        case K::ProductsReady: return StoreEventKind::ProductsReady;
        case K::Purchased: return StoreEventKind::Purchased;
        case K::Pending: return StoreEventKind::Pending;
        case K::Cancelled: return StoreEventKind::Cancelled;
        case K::Failed: return StoreEventKind::Failed;
        case K::Restored: return StoreEventKind::Restored;
        case K::RestoreFinished: return StoreEventKind::RestoreFinished;
    }
    return StoreEventKind::Failed;
}

void forward(void* self, const platform::store::Answer& a) { static_cast<PlatformStore*>(self)->answer(a); }

}  // namespace

PlatformStore::PlatformStore(mem::IAllocator& a) : alloc_(a), open_(a) {}

PlatformStore::~PlatformStore() { stop(); }

const char* PlatformStore::name() const { return platform::store::backend_name(platform::store::backend()); }

bool PlatformStore::available() const { return platform::store::backend() != platform::store::Backend::None; }

StringView PlatformStore::store_id(u16 product) const {
    if (!catalog_ || product >= catalog_->products().size()) return {};
    const ProductInfo& p = catalog_->products()[product];
    // Empty: the product is not sold on this platform (store_importer.cpp).
    return platform::store::backend() == platform::store::Backend::StoreKit ? p.ios_id.view() : p.android_id.view();
}

void PlatformStore::start(const Catalog& catalog, Sink& sink) {
    stop();
    catalog_ = &catalog;
    sink_ = &sink;
    started_ = true;
    if (!platform::store::start(&forward, this))
        MCE_LOG_INFO("monetize: no platform store here ('{}'); purchases are refused", name());
}

void PlatformStore::stop() {
    if (!started_) return;
    platform::store::stop();
    started_ = false;
    std::lock_guard<std::mutex> lock(open_mutex_);
    open_.clear();
}

void PlatformStore::query_products(u32 request) {
    const usize n = catalog_ ? catalog_->products().size() : 0;
    Array<StringView> ids(alloc_);
    for (usize i = 0; i < n; ++i)
        if (!store_id(static_cast<u16>(i)).empty()) ids.push_back(store_id(static_cast<u16>(i)));
    platform::store::query_products(ids.data(), static_cast<u32>(ids.size()), request);
}

void PlatformStore::purchase(u16 product, u32 request) {
    if (!catalog_ || product >= catalog_->products().size() || store_id(product).empty()) {
        StoreAnswer a;
        a.kind = StoreEventKind::Failed;
        a.product = product;
        a.request = request;
        if (sink_) sink_->post(a);
        return;
    }
    const bool consumable = catalog_->products()[product].type == ProductType::Consumable;
    platform::store::purchase(store_id(product), consumable, request);
}

void PlatformStore::restore(u32 request) { platform::store::restore(request); }

void PlatformStore::finish(StringView transaction) {
    bool known = false;
    bool consumable = false;
    {
        std::lock_guard<std::mutex> lock(open_mutex_);
        for (usize i = 0; i < open_.size(); ++i) {
            if (open_[i].transaction.view() != transaction) continue;
            known = true;
            consumable = open_[i].consumable;
            open_.erase_swap(i);
            break;
        }
    }
    if (!known) {
        MCE_LOG_WARN("monetize: finish for a transaction the store did not report; ignored");
        return;
    }
    platform::store::finish(transaction, consumable);
}

void PlatformStore::answer(const platform::store::Answer& a) {
    if (!sink_ || !catalog_) return;
    StoreAnswer out;
    out.kind = kind_of(a.kind);
    out.request = a.request;
    if (!a.id.empty()) {
        for (usize i = 0; i < catalog_->products().size(); ++i) {
            if (store_id(static_cast<u16>(i)) != a.id) continue;
            out.product = static_cast<u16>(i);
            break;
        }
    }
    const bool has_product = out.kind != StoreEventKind::ProductsReady && out.kind != StoreEventKind::RestoreFinished;
    if (has_product && out.product == kNoIndex && out.kind != StoreEventKind::Failed &&
        out.kind != StoreEventKind::Cancelled) {
        MCE_LOG_WARN("monetize: the store answered for '{}', which the catalog does not sell; ignored", a.id);
        return;
    }
    out.price = a.price;
    out.transaction = a.transaction;
    if ((out.kind == StoreEventKind::Purchased || out.kind == StoreEventKind::Restored) && !a.transaction.empty()) {
        if (out.transaction.truncated()) {
            MCE_LOG_ERROR("monetize: a store transaction id is longer than {} bytes; ignored", out.transaction.capacity());
            return;
        }
        const bool consumable = catalog_->products()[out.product].type == ProductType::Consumable;
        if (out.kind == StoreEventKind::Restored && consumable) return;  // stores never restore these
        std::lock_guard<std::mutex> lock(open_mutex_);
        bool seen = false;
        for (const Open& o : open_) seen = seen || o.transaction.view() == a.transaction;
        // An unfinished purchase the store reports again on its own (its listener and the start-up
        // query both see it) would be granted twice.
        if (seen && a.request == 0) return;
        if (!seen) open_.push_back(Open{out.transaction, consumable});
    }
    sink_->post(out);
}

}  // namespace mce::monetize
