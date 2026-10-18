// .mcitems -> CHNK_ITEMS: a project's item definitions and loot tables (M-INVENTORY 1; mce/asset/items_format.h has the
// format and the chunk layout). The catalog is built here, checked by asset::item_catalog_problem (the rules the
// runtime reader enforces too), encoded, and decoded again before it is stored, so a catalog the runtime would
// reject never cooks.
//
// {
//   "format": "mce-items", "version": 1, "guid": "<uuid>",
//   "items": [ { "id": "medkit", "name": "Medkit", "stack": 5, "weight": 0.4, "tags": ["consumable"],
//                "stats": { "heal": 25 }, "icon": "guid:<texture>", "prefab": "guid:<prefab>" } ],
//   "tables": [ { "id": "chest_common", "rolls": [1, 3],
//                 "entries": [ { "item": "medkit", "weight": 5, "qty": [1, 2] },
//                              { "table": "chest_rare", "weight": 1 },
//                              { "item": "key", "guaranteed": true } ] } ]
// }
//
// "stack" (default 1) is a whole number, "weight" is kilograms (default 0, kept as thousandths); "rolls" and "qty" are a
// whole number or [min, max] (defaults 1); an entry's "weight" defaults to 1 and means nothing for a guaranteed entry.
// "icon" and "prefab" are dependencies that must exist with the right type (a Texture, a Prefab); the chunk keeps
// only their GUIDs, so they are links (no cook ordering, a prefab may name this catalog back).

#include <cmath>

#include "importer_common.h"
#include "mce/asset/items_format.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.items";
constexpr u32 kItemsFormatVersion = 1;

void error(DiagList& diags, StringView file, JsonValue at, StringView msg) { detail::error_at(diags, Errc::ParseError, file, at, kStage, msg); }

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
        appendf(msg, "'%.*s' is longer than %u characters", static_cast<int>(key.size()), key.data(), static_cast<unsigned>(out.capacity()));
        error(diags, file, v, msg);
        return false;
    }
    return true;
}

// A whole number within [lo, hi]; absent keeps `out`.
bool read_whole(JsonValue obj, StringView key, u32 lo, u32 hi, u32& out, StringView file, DiagList& diags) {
    const JsonValue v = obj.get(key);
    if (!v.valid()) return true;
    if (!v.is_integer() || v.as_number() < static_cast<f64>(lo) || v.as_number() > static_cast<f64>(hi)) {
        String msg;
        appendf(msg, "'%.*s' must be a whole number from %u to %u", static_cast<int>(key.size()), key.data(), lo, hi);
        error(diags, file, v, msg);
        return false;
    }
    out = static_cast<u32>(v.as_number());
    return true;
}

// A whole number or [min, max] within [lo, hi]; absent keeps the defaults in `out`.
bool read_span(JsonValue obj, StringView key, u32 lo, u32 hi, u32 out[2], StringView file, DiagList& diags) {
    const JsonValue v = obj.get(key);
    if (!v.valid()) return true;
    auto whole = [&](JsonValue n) { return n.is_integer() && n.as_number() >= static_cast<f64>(lo) && n.as_number() <= static_cast<f64>(hi); };
    if (whole(v)) {
        out[0] = out[1] = static_cast<u32>(v.as_number());
        return true;
    }
    if (v.is_array() && v.size() == 2 && whole(v.at(0)) && whole(v.at(1)) && v.at(0).as_number() <= v.at(1).as_number()) {
        out[0] = static_cast<u32>(v.at(0).as_number());
        out[1] = static_cast<u32>(v.at(1).as_number());
        return true;
    }
    String msg;
    appendf(msg, "'%.*s' must be a whole number or [min, max] within %u to %u", static_cast<int>(key.size()), key.data(), lo, hi);
    error(diags, file, v, msg);
    return false;
}

bool read_guid(JsonValue obj, StringView key, Guid& out, StringView file, DiagList& diags) {
    const JsonValue v = obj.get(key);
    if (!v.valid()) return true;
    if (!v.is_string() || !parse_guid_ref(v.as_string(), out)) {
        String msg;
        appendf(msg, "'%.*s' must be \"guid:<uuid>\"", static_cast<int>(key.size()), key.data());
        error(diags, file, v, msg);
        return false;
    }
    return true;
}

void add_dep(Array<DepRef>& out, JsonValue obj, StringView key, ChunkType expected, u32 index) {
    const JsonValue v = obj.get(key);
    Guid g;
    if (!v.is_string() || !parse_guid_ref(v.as_string(), g)) return;  // cook() reports it
    DepRef d;
    d.guid = g;
    d.expected = expected;
    d.line = v.line();
    d.col = v.col();
    d.link = true;
    String where;
    appendf(where, "items[%u].%.*s", index, static_cast<int>(key.size()), key.data());
    d.where.assign(where.view());
    out.push_back(d);
}

class ItemsImporter final : public IImporter {
public:
    StringView id() const override { return "items"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::ItemCatalog; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ".mcitems"; }

    void scan_dependencies(const ImportInput& in, Array<DepRef>& out, DiagList&) const override {
        const JsonValue items = in.settings.get("items");
        if (!items.is_array()) return;
        for (u32 i = 0; i < items.size() && i < asset::kItemMaxItems; ++i) {
            const JsonValue it = items.at(i);
            if (!it.is_object()) continue;
            add_dep(out, it, "icon", ChunkType::Texture, i);
            add_dep(out, it, "prefab", ChunkType::Prefab, i);
        }
    }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue doc = in.settings;
        const StringView file = in.display_path;
        const u32 errors_before = diags.error_count();
        if (!detail::check_keys(doc, {"format", "version", "guid", "items", "tables"}, file, kStage, diags)) return false;
        const JsonValue fmt = doc.get("format");
        if (!fmt.is_string() || fmt.as_string() != "mce-items") {
            error(diags, file, fmt.valid() ? fmt : doc, "\"format\" must be \"mce-items\"");
            return false;
        }
        const JsonValue ver = doc.get("version");
        if (!ver.is_integer() || ver.as_number() != kItemsFormatVersion) {
            String msg;
            appendf(msg, "unsupported item catalog version (this cook reads version %u)", kItemsFormatVersion);
            detail::error_at(diags, Errc::VersionTooNew, file, ver.valid() ? ver : doc, kStage, msg);
            return false;
        }

        asset::ItemCatalog catalog;
        const JsonValue items = doc.get("items");
        if (items.valid() && !items.is_array()) error(diags, file, items, "'items' must be an array");
        else if (items.valid() && items.size() > asset::kItemMaxItems) error(diags, file, items, "a catalog holds at most 4096 items");
        else if (items.valid())
            for (u32 i = 0; i < items.size(); ++i) {
                const JsonValue it = items.at(i);
                if (!detail::check_keys(it, {"id", "name", "stack", "weight", "tags", "stats", "icon", "prefab"}, file, kStage, diags)) continue;
                asset::ItemDef d;
                if (!it.get("id").is_string() || it.get("id").as_string().empty()) {
                    error(diags, file, it, "every item needs a non-empty \"id\"");
                    continue;
                }
                bool good = read_text(it, "id", d.id, file, diags);
                good &= read_text(it, "name", d.name, file, diags);
                if (d.name.empty()) d.name = d.id.view();
                good &= read_whole(it, "stack", 1, asset::kItemMaxStack, d.stack_max, file, diags);
                const JsonValue w = it.get("weight");
                if (w.valid()) {
                    if (!w.is_number() || !std::isfinite(w.as_number()) || w.as_number() < 0.0 || w.as_number() > 1000000.0) {
                        error(diags, file, w, "'weight' must be kilograms from 0 to 1000000");
                        good = false;
                    } else {
                        d.weight_milli = static_cast<u32>(std::llround(w.as_number() * 1000.0));
                    }
                }
                good &= read_guid(it, "icon", d.icon, file, diags);
                good &= read_guid(it, "prefab", d.prefab, file, diags);
                Array<asset::ItemStat> stats;
                const JsonValue st = it.get("stats");
                if (st.valid()) {
                    if (!st.is_object() || st.size() > asset::kItemMaxStats) {
                        error(diags, file, st, "'stats' must be an object of up to 32 numbers: {\"damage\": 12}");
                        good = false;
                    } else {
                        for (u32 k = 0; k < st.size(); ++k) {
                            const JsonValue v = st.at(k);
                            asset::ItemStat s;
                            s.key = st.key_at(k);
                            if (!v.is_number() || !std::isfinite(v.as_number()) || std::fabs(v.as_number()) > 1.0e9) {
                                error(diags, file, v, "a stat must be a number");
                                good = false;
                                continue;
                            }
                            s.value = static_cast<f32>(v.as_number());
                            stats.push_back(s);
                        }
                    }
                }
                Array<FixedString<32>> tags;
                const JsonValue tg = it.get("tags");
                if (tg.valid()) {
                    if (!tg.is_array() || tg.size() > asset::kItemMaxTags) {
                        error(diags, file, tg, "'tags' must be an array of up to 16 strings");
                        good = false;
                    } else {
                        for (u32 k = 0; k < tg.size(); ++k) {
                            if (!tg.at(k).is_string()) {
                                error(diags, file, tg.at(k), "a tag must be a string");
                                good = false;
                                continue;
                            }
                            FixedString<32> t;
                            t = tg.at(k).as_string();
                            tags.push_back(t);
                        }
                    }
                }
                if (!good) continue;
                if (!catalog.add_item(d, stats.span(), tags.span())) error(diags, file, it, "two items share this \"id\"");
            }

        const JsonValue tables = doc.get("tables");
        if (tables.valid() && !tables.is_array()) error(diags, file, tables, "'tables' must be an array");
        else if (tables.valid() && tables.size() > asset::kItemMaxTables) error(diags, file, tables, "a catalog holds at most 1024 loot tables");
        else if (tables.valid()) {
            // Two passes, so a table may name one that comes later: the ids first, then the entries.
            Array<FixedString<64>> ids;
            for (u32 i = 0; i < tables.size(); ++i) {
                FixedString<64> tid;
                const JsonValue idv = tables.at(i).get("id");
                if (idv.is_string()) tid = idv.as_string();
                ids.push_back(tid);
            }
            auto find_table = [&](StringView name) -> u32 {
                for (u32 k = 0; k < ids.size(); ++k)
                    if (ids[k].view() == name) return k;
                return 0xFFFFFFFFu;
            };
            for (u32 i = 0; i < tables.size(); ++i) {
                const JsonValue t = tables.at(i);
                if (!detail::check_keys(t, {"id", "rolls", "entries"}, file, kStage, diags)) continue;
                asset::LootTable lt;
                if (!t.get("id").is_string() || t.get("id").as_string().empty()) {
                    error(diags, file, t, "every loot table needs a non-empty \"id\"");
                    continue;
                }
                bool good = read_text(t, "id", lt.id, file, diags);
                u32 rolls[2] = {1, 1};
                good &= read_span(t, "rolls", 0, asset::kItemMaxRolls, rolls, file, diags);
                lt.rolls_min = static_cast<u16>(rolls[0]);
                lt.rolls_max = static_cast<u16>(rolls[1]);
                Array<asset::LootEntry> entries;
                const JsonValue es = t.get("entries");
                if (!es.is_array() || es.size() == 0 || es.size() > asset::kItemMaxEntries) {
                    error(diags, file, es.valid() ? es : t, "'entries' must list 1 to 256 entries");
                    continue;
                }
                for (u32 k = 0; k < es.size(); ++k) {
                    const JsonValue e = es.at(k);
                    if (!detail::check_keys(e, {"item", "table", "weight", "qty", "guaranteed"}, file, kStage, diags)) {
                        good = false;
                        continue;
                    }
                    asset::LootEntry le;
                    const JsonValue item = e.get("item"), table = e.get("table");
                    if (item.valid() == table.valid()) {
                        error(diags, file, e, "an entry names exactly one of \"item\" and \"table\"");
                        good = false;
                        continue;
                    }
                    if (item.valid()) {
                        const u16 idx = item.is_string() ? catalog.find_item(item.as_string()) : asset::kNoItem;
                        if (idx == asset::kNoItem) {
                            error(diags, file, item, "this entry names an item that is not in \"items\"");
                            good = false;
                            continue;
                        }
                        le.kind = asset::LootKind::Item;
                        le.ref = idx;
                    } else {
                        const u32 idx = table.is_string() ? find_table(table.as_string()) : 0xFFFFFFFFu;
                        if (idx == 0xFFFFFFFFu) {
                            error(diags, file, table, "this entry names a loot table that is not in \"tables\"");
                            good = false;
                            continue;
                        }
                        le.kind = asset::LootKind::Table;
                        le.ref = static_cast<u16>(idx);
                    }
                    bool guaranteed = false;
                    good &= detail::read_bool(e, "guaranteed", false, guaranteed, file, kStage, diags);
                    le.guaranteed = guaranteed;
                    good &= read_whole(e, "weight", guaranteed ? 0 : 1, asset::kItemMaxEntryWeight, le.weight, file, diags);
                    if (guaranteed) le.weight = 1;  // unused for a guaranteed entry
                    u32 qty[2] = {1, 1};
                    good &= read_span(e, "qty", 1, asset::kItemMaxStack, qty, file, diags);
                    le.qty_min = qty[0];
                    le.qty_max = qty[1];
                    entries.push_back(le);
                }
                if (!good) continue;
                if (!catalog.add_table(lt, entries.span())) error(diags, file, t, "two loot tables share this \"id\"");
            }
        }
        if (diags.error_count() != errors_before) return false;

        u32 at = 0;
        if (const char* why = asset::item_catalog_problem(catalog, &at)) {
            String msg;
            const bool table = why[0] == 't';
            const StringView name = table ? catalog.tables()[at].id.view() : catalog.items()[at].id.view();
            appendf(msg, "%s (%s \"%.*s\")", why + (table ? 7 : 6), table ? "table" : "item", static_cast<int>(name.size()), name.data());
            error(diags, file, doc, msg);
            return false;
        }

        Array<byte> bytes;
        asset::encode_item_catalog(catalog, bytes);
        asset::ItemCatalog check;
        if (Result<void> r = asset::decode_item_catalog(bytes.span(), check); !r) {
            FixedString<512> why;
            format_error(r.error(), why);
            String msg("the item catalog is not loadable: ");
            msg += why.view();
            error(diags, file, doc, msg);
            return false;
        }

        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::ItemCatalog;
        d.version = asset::chunk_version(asset::ITEM_CATALOG_VERSION, 0);
        d.guid = in.guid;
        out.add(d, bytes.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_items_importer() { return mem::make_unique<ItemsImporter>(mem::heap()); }

}  // namespace mce::cook
