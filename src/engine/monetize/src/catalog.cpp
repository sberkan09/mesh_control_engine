#include "mce/monetize/catalog.h"

#include <cstring>

namespace mce::monetize {

namespace {

constexpr const char* kStage = "monetize.catalog";

struct StoreCatalogHeader {
    u32 product_count;
    u32 ad_count;
    u32 strings_size;
    u32 reserved;
};
static_assert(sizeof(StoreCatalogHeader) == 16);

struct StrRef {
    u32 offset;
    u32 size;
};

constexpr u8 kFlagRemovesAds = 1;

struct StoreCatalogEntry {
    StrRef id;
    StrRef ios;
    StrRef android;
    u8 kind;   // ProductType or AdFormat
    u8 flags;  // products: kFlagRemovesAds; ads: 0
    u8 reserved[6];
};
static_assert(sizeof(StoreCatalogEntry) == 32);

StrRef add_string(Array<byte>& blob, StringView s) {
    const StrRef r{static_cast<u32>(blob.size()), static_cast<u32>(s.size())};
    for (char c : s) blob.push_back(static_cast<byte>(c));
    return r;
}

void append(Array<byte>& out, const void* p, usize n) {
    const usize at = out.size();
    out.resize_uninitialized(at + n);
    std::memcpy(out.data() + at, p, n);
}

bool read_string(const char* blob, u32 blob_size, StrRef r, StringView& out) {
    if (r.offset > blob_size || r.size > blob_size - r.offset) return false;
    out = StringView(blob + r.offset, r.size);
    return true;
}

}  // namespace

u16 Catalog::find_product(StringView id) const {
    for (usize i = 0; i < products_.size(); ++i)
        if (products_[i].id.view() == id) return static_cast<u16>(i);
    return kNoIndex;
}

u16 Catalog::find_ad(StringView id) const {
    for (usize i = 0; i < ads_.size(); ++i)
        if (ads_[i].id.view() == id) return static_cast<u16>(i);
    return kNoIndex;
}

bool Catalog::add_product(const ProductInfo& p) {
    if (p.id.empty() || find_product(p.id.view()) != kNoIndex || products_.size() >= kNoIndex) return false;
    products_.push_back(p);
    return true;
}

bool Catalog::add_ad(const AdPlacementInfo& a) {
    if (a.id.empty() || find_ad(a.id.view()) != kNoIndex || ads_.size() >= kNoIndex) return false;
    ads_.push_back(a);
    return true;
}

const char* product_type_name(ProductType t) {
    return t == ProductType::NonConsumable ? "non_consumable" : "consumable";
}

const char* ad_format_name(AdFormat f) {
    switch (f) {
        case AdFormat::Rewarded: return "rewarded";
        case AdFormat::Interstitial: return "interstitial";
        case AdFormat::Banner: return "banner";
    }
    return "?";
}

bool parse_product_type(StringView s, ProductType& out) {
    if (s == "consumable") out = ProductType::Consumable;
    else if (s == "non_consumable") out = ProductType::NonConsumable;
    else return false;
    return true;
}

bool parse_ad_format(StringView s, AdFormat& out) {
    if (s == "rewarded") out = AdFormat::Rewarded;
    else if (s == "interstitial") out = AdFormat::Interstitial;
    else if (s == "banner") out = AdFormat::Banner;
    else return false;
    return true;
}

void encode_catalog(const Catalog& c, Array<byte>& out) {
    Array<byte> blob;
    Array<StoreCatalogEntry> entries;
    for (const ProductInfo& p : c.products()) {
        StoreCatalogEntry e{};
        e.id = add_string(blob, p.id.view());
        e.ios = add_string(blob, p.ios_id.view());
        e.android = add_string(blob, p.android_id.view());
        e.kind = static_cast<u8>(p.type);
        e.flags = p.removes_ads ? kFlagRemovesAds : 0;
        entries.push_back(e);
    }
    for (const AdPlacementInfo& a : c.ads()) {
        StoreCatalogEntry e{};
        e.id = add_string(blob, a.id.view());
        e.ios = add_string(blob, a.ios_unit.view());
        e.android = add_string(blob, a.android_unit.view());
        e.kind = static_cast<u8>(a.format);
        entries.push_back(e);
    }
    StoreCatalogHeader h{};
    h.product_count = static_cast<u32>(c.products().size());
    h.ad_count = static_cast<u32>(c.ads().size());
    h.strings_size = static_cast<u32>(blob.size());
    out.clear();
    append(out, &h, sizeof(h));
    if (!entries.empty()) append(out, entries.data(), entries.size() * sizeof(StoreCatalogEntry));
    if (!blob.empty()) append(out, blob.data(), blob.size());
}

Result<void> decode_catalog(Span<const byte> payload, Catalog& out) {
    out.clear();
    if (payload.size() < sizeof(StoreCatalogHeader)) return Error(Errc::Truncated, kStage);
    StoreCatalogHeader h;
    std::memcpy(&h, payload.data(), sizeof(h));
    if (h.reserved != 0) return Error(Errc::ReservedNonZero, kStage);
    const u64 count = u64(h.product_count) + u64(h.ad_count);
    if (h.product_count >= kNoIndex || h.ad_count >= kNoIndex) return Error(Errc::OutOfBounds, kStage);
    const u64 need = sizeof(h) + count * sizeof(StoreCatalogEntry) + h.strings_size;
    if (payload.size() < need) return Error(Errc::Truncated, kStage, {}, static_cast<u32>(payload.size()));
    const byte* entries = payload.data() + sizeof(h);
    const char* blob = reinterpret_cast<const char*>(entries + count * sizeof(StoreCatalogEntry));
    for (u64 i = 0; i < count; ++i) {
        StoreCatalogEntry e;
        std::memcpy(&e, entries + i * sizeof(StoreCatalogEntry), sizeof(e));
        for (u8 r : e.reserved)
            if (r != 0) return Error(Errc::ReservedNonZero, kStage, {}, static_cast<u32>(i));
        StringView id, ios, android;
        if (!read_string(blob, h.strings_size, e.id, id) || !read_string(blob, h.strings_size, e.ios, ios) ||
            !read_string(blob, h.strings_size, e.android, android))
            return Error(Errc::OutOfBounds, kStage, {}, static_cast<u32>(i));
        bool added = false;
        if (i < h.product_count) {
            if (e.kind > static_cast<u8>(ProductType::NonConsumable)) return Error(Errc::InvalidArgument, kStage, id);
            // Only a non-consumable can remove ads (a consumable is never owned).
            if ((e.flags & ~kFlagRemovesAds) != 0 ||
                ((e.flags & kFlagRemovesAds) && e.kind != static_cast<u8>(ProductType::NonConsumable)))
                return Error(Errc::InvalidArgument, kStage, id, static_cast<u32>(i));
            ProductInfo p;
            p.id = id;
            p.ios_id = ios;
            p.android_id = android;
            p.type = static_cast<ProductType>(e.kind);
            p.removes_ads = (e.flags & kFlagRemovesAds) != 0;
            added = !p.id.truncated() && !p.ios_id.truncated() && !p.android_id.truncated() && out.add_product(p);
        } else {
            if (e.kind > static_cast<u8>(AdFormat::Banner) || e.flags != 0)
                return Error(Errc::InvalidArgument, kStage, id, static_cast<u32>(i));
            AdPlacementInfo a;
            a.id = id;
            a.ios_unit = ios;
            a.android_unit = android;
            a.format = static_cast<AdFormat>(e.kind);
            added = !a.id.truncated() && !a.ios_unit.truncated() && !a.android_unit.truncated() && out.add_ad(a);
        }
        if (!added) {
            out.clear();
            return Error(Errc::InvalidArgument, kStage, id, static_cast<u32>(i));  // empty, duplicate or too long
        }
    }
    return ok();
}

}  // namespace mce::monetize
