// .mcstore -> CHNK_STORE: a project's store catalog (M-MONETIZE step 1; mce/monetize/catalog.h has
// the format). The chunk is the one monetize::decode_catalog reads, written by encode_catalog and
// decoded again before it is stored, so a catalog the runtime would reject never cooks.
//
// {
//   "format": "mce-store", "version": 1, "guid": "<uuid>",
//   "products": [ { "id": "remove_ads", "type": "non_consumable", "ios": "...", "android": "...",
//                   "removes_ads": true } ],
//   "ads": [ { "id": "level_end", "format": "interstitial", "ios": "...", "android": "..." } ],
//   "admob": { "ios": "ca-app-pub-...~...", "android": "ca-app-pub-...~..." }
// }
//
// "type" is consumable or non_consumable; "removes_ads" (non_consumable only) turns every ad off once
// owned; "format" is rewarded, interstitial or banner; "ios" and "android" are optional (a product without one is not sold there; a placement without one shows
// the ad network's test unit there). "admob" holds the AdMob app ids the packaging scripts put in the
// Android manifest and the iOS Info.plist (cmake/mobile/monetize_info.py); the chunk does not carry them.

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/monetize/catalog.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.store";
constexpr u32 kStoreFormatVersion = 1;

void error(DiagList& diags, StringView file, JsonValue at, StringView msg) {
    detail::error_at(diags, Errc::ParseError, file, at, kStage, msg);
}

// Reads an optional string member into `out`; false (with an error) when it is not a string or too long.
template <usize N>
bool read_text(JsonValue obj, StringView key, FixedString<N>& out, StringView file, DiagList& diags) {
    const JsonValue v = obj.get(key);
    if (!v.valid()) return true;
    if (!v.is_string()) {
        String msg;
        appendf(msg, "'%.*s' must be a string", static_cast<int>(key.size()), key.data());
        error(diags, file, v, msg);
        return false;
    }
    out = v.as_string();
    if (out.truncated()) {
        String msg;
        appendf(msg, "'%.*s' is longer than %u characters", static_cast<int>(key.size()), key.data(),
                static_cast<unsigned>(out.capacity()));
        error(diags, file, v, msg);
        return false;
    }
    return true;
}

// The id every entry needs: non-empty, letters, digits, '_', '-' and '.'.
bool read_id(JsonValue obj, FixedString<64>& out, StringView file, DiagList& diags) {
    const JsonValue v = obj.get("id");
    if (!v.is_string() || v.as_string().empty()) {
        error(diags, file, v.valid() ? v : obj, "every entry needs a non-empty \"id\"");
        return false;
    }
    if (!read_text(obj, "id", out, file, diags)) return false;
    for (const char c : out.view()) {
        const bool okc = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                         c == '.';
        if (!okc) {
            error(diags, file, v, "an \"id\" may only use letters, digits, '_', '-' and '.'");
            return false;
        }
    }
    return true;
}

class StoreImporter final : public IImporter {
public:
    StringView id() const override { return "store"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::StoreCatalog; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ".mcstore"; }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue doc = in.settings;
        const StringView file = in.display_path;
        const u32 errors_before = diags.error_count();
        if (!detail::check_keys(doc, {"format", "version", "guid", "products", "ads", "admob"}, file, kStage, diags))
            return false;
        const JsonValue fmt = doc.get("format");
        if (!fmt.is_string() || fmt.as_string() != "mce-store") {
            error(diags, file, fmt.valid() ? fmt : doc, "\"format\" must be \"mce-store\"");
            return false;
        }
        const JsonValue ver = doc.get("version");
        if (!ver.is_integer() || ver.as_number() != kStoreFormatVersion) {
            String msg;
            appendf(msg, "unsupported store catalog version (this cook reads version %u)", kStoreFormatVersion);
            detail::error_at(diags, Errc::VersionTooNew, file, ver.valid() ? ver : doc, kStage, msg);
            return false;
        }

        const JsonValue admob = doc.get("admob");
        if (admob.valid()) {
            if (!admob.is_object()) {
                error(diags, file, admob, "'admob' must be an object of app ids: {\"ios\": ..., \"android\": ...}");
            } else if (detail::check_keys(admob, {"ios", "android"}, file, kStage, diags)) {
                for (const char* key : {"ios", "android"}) {
                    FixedString<128> app_id;
                    if (!read_text(admob, key, app_id, file, diags)) continue;
                    // An AdMob app id is "ca-app-pub-<publisher>~<app>"; a unit id ("/") here is the usual mix-up.
                    const StringView v = app_id.view();
                    if (!v.empty() && (v.substr(0, 11) != "ca-app-pub-" || v.find('~') == StringView::npos)) {
                        String msg;
                        appendf(msg, "'admob.%s' must be an AdMob app id (ca-app-pub-...~...), not an ad unit id", key);
                        error(diags, file, admob.get(key), msg);
                    }
                }
            }
        }

        monetize::Catalog catalog;
        const JsonValue products = doc.get("products");
        if (products.valid() && !products.is_array()) error(diags, file, products, "'products' must be an array");
        else if (products.valid())
            for (u32 i = 0; i < products.size(); ++i) {
                const JsonValue p = products.at(i);
                if (!detail::check_keys(p, {"id", "type", "ios", "android", "removes_ads"}, file, kStage, diags)) continue;
                monetize::ProductInfo info;
                if (!read_id(p, info.id, file, diags)) continue;
                const JsonValue t = p.get("type");
                if (!t.is_string() || !monetize::parse_product_type(t.as_string(), info.type)) {
                    error(diags, file, t.valid() ? t : p, "a product's \"type\" must be consumable or non_consumable");
                    continue;
                }
                if (!read_text(p, "ios", info.ios_id, file, diags) || !read_text(p, "android", info.android_id, file, diags))
                    continue;
                const JsonValue removes = p.get("removes_ads");
                if (removes.valid()) {
                    if (!removes.is_bool()) {
                        error(diags, file, removes, "'removes_ads' must be true or false");
                        continue;
                    }
                    info.removes_ads = removes.as_bool();
                    if (info.removes_ads && info.type != monetize::ProductType::NonConsumable) {
                        error(diags, file, removes, "only a non_consumable product can have \"removes_ads\"");
                        continue;
                    }
                }
                if (!catalog.add_product(info)) error(diags, file, p, "two products share this \"id\"");
            }
        const JsonValue ads = doc.get("ads");
        if (ads.valid() && !ads.is_array()) error(diags, file, ads, "'ads' must be an array");
        else if (ads.valid())
            for (u32 i = 0; i < ads.size(); ++i) {
                const JsonValue a = ads.at(i);
                if (!detail::check_keys(a, {"id", "format", "ios", "android"}, file, kStage, diags)) continue;
                monetize::AdPlacementInfo info;
                if (!read_id(a, info.id, file, diags)) continue;
                const JsonValue f = a.get("format");
                if (!f.is_string() || !monetize::parse_ad_format(f.as_string(), info.format)) {
                    error(diags, file, f.valid() ? f : a, "an ad's \"format\" must be rewarded, interstitial or banner");
                    continue;
                }
                if (!read_text(a, "ios", info.ios_unit, file, diags) || !read_text(a, "android", info.android_unit, file, diags))
                    continue;
                if (!catalog.add_ad(info)) error(diags, file, a, "two ad placements share this \"id\"");
            }
        if (diags.error_count() != errors_before) return false;

        Array<byte> bytes;
        monetize::encode_catalog(catalog, bytes);
        monetize::Catalog check;
        if (Result<void> r = monetize::decode_catalog(bytes.span(), check); !r) {
            FixedString<512> why;
            format_error(r.error(), why);
            String msg("the store catalog is not loadable: ");
            msg += why.view();
            error(diags, file, doc, msg);
            return false;
        }

        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::StoreCatalog;
        d.version = asset::chunk_version(asset::STORE_CATALOG_VERSION, 0);
        d.guid = in.guid;
        out.add(d, bytes.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_store_importer() { return mem::make_unique<StoreImporter>(mem::heap()); }

}  // namespace mce::cook
