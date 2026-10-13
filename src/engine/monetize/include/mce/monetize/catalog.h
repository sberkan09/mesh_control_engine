#pragma once
// The store catalog (M-MONETIZE step 1): the products a game sells and the ad placements it shows,
// each with its App Store and Google Play id. Authored as a project's .mcstore file, cooked to a
// StoreCatalog chunk (the runtime never parses text, FR-AST-010) and handed to monetize::Service.
//
//   {
//     "format": "mce-store", "version": 1, "guid": "<uuid>",
//     "products": [
//       { "id": "remove_ads", "type": "non_consumable", "ios": "com.studio.game.remove_ads", "android": "remove_ads" },
//       { "id": "gems_100",   "type": "consumable",     "ios": "com.studio.game.gems_100",   "android": "gems_100" } ],
//     "ads": [
//       { "id": "continue",  "format": "rewarded" },
//       { "id": "level_end", "format": "interstitial" },
//       { "id": "menu",      "format": "banner", "ios": "ca-app-pub-...", "android": "ca-app-pub-..." } ]
//   }
//
// Game code names products and placements by "id"; the store and ad SDK ids stay in this file. An
// ad placement without a platform id uses the ad network's test unit on that platform.
//
// Chunk layout v1 (little-endian):
//   StoreCatalogHeader | StoreCatalogEntry[product_count + ad_count] | string blob

#include "mce/containers/array.h"
#include "mce/fixed_string.h"
#include "mce/result.h"

namespace mce::monetize {

enum class ProductType : u8 {
    Consumable,     // bought again and again (gems); the game grants it, then Store::finish
    NonConsumable,  // bought once, owned forever, restorable (remove ads, a level pack)
};

enum class AdFormat : u8 {
    Rewarded,      // fullscreen; the player earns a reward by watching it to the end
    Interstitial,  // fullscreen between levels
    Banner,        // a strip along the top or bottom edge, inside the safe area
};

inline constexpr u16 kNoIndex = 0xFFFF;

struct ProductInfo {
    FixedString<64> id;  // what the game names it by
    FixedString<128> ios_id;
    FixedString<128> android_id;
    ProductType type = ProductType::Consumable;
    // Owning it turns every ad off (M-MONETIZE step 5): a non-consumable such as "remove_ads".
    bool removes_ads = false;
};

struct AdPlacementInfo {
    FixedString<64> id;
    FixedString<128> ios_unit;
    FixedString<128> android_unit;
    AdFormat format = AdFormat::Interstitial;
};

class Catalog {
public:
    explicit Catalog(mem::IAllocator& a = mem::heap()) : products_(a), ads_(a) {}

    Span<const ProductInfo> products() const { return products_.span(); }
    Span<const AdPlacementInfo> ads() const { return ads_.span(); }
    // Index by id, kNoIndex when absent.
    u16 find_product(StringView id) const;
    u16 find_ad(StringView id) const;

    // Code-side authoring (tests, samples): false when the id is empty or already used.
    bool add_product(const ProductInfo& p);
    bool add_ad(const AdPlacementInfo& a);
    void clear() {
        products_.clear();
        ads_.clear();
    }

private:
    Array<ProductInfo> products_;
    Array<AdPlacementInfo> ads_;
};

const char* product_type_name(ProductType t);
const char* ad_format_name(AdFormat f);
bool parse_product_type(StringView s, ProductType& out);
bool parse_ad_format(StringView s, AdFormat& out);

// The StoreCatalog chunk: written by the cook (encode), read by the runtime (decode, which
// bounds-checks every offset and rejects duplicate or empty ids).
void encode_catalog(const Catalog& c, Array<byte>& out);
Result<void> decode_catalog(Span<const byte> payload, Catalog& out);

}  // namespace mce::monetize
