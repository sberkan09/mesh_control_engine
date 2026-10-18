#include "mce/asset/items_format.h"

#include <cmath>
#include <cstring>

namespace mce::asset {

namespace {

constexpr const char* kStage = "asset.items";

bool id_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}

template <usize N>
bool id_ok(const FixedString<N>& s) {
    if (s.empty() || s.truncated()) return false;
    for (const char c : s.view())
        if (!id_char(c)) return false;
    return true;
}

// ---- encode

struct Writer {
    Array<byte>& out;
    void raw(const void* p, usize n) {
        const byte* b = static_cast<const byte*>(p);
        for (usize i = 0; i < n; ++i) out.push_back(b[i]);
    }
    void u8_(u8 v) { out.push_back(static_cast<byte>(v)); }
    void u16_(u16 v) {
        u8_(static_cast<u8>(v & 0xFF));
        u8_(static_cast<u8>(v >> 8));
    }
    void u32_(u32 v) {
        u16_(static_cast<u16>(v & 0xFFFF));
        u16_(static_cast<u16>(v >> 16));
    }
    void u64_(u64 v) {
        u32_(static_cast<u32>(v & 0xFFFFFFFFu));
        u32_(static_cast<u32>(v >> 32));
    }
    void f32_(f32 v) {
        u32 bits;
        std::memcpy(&bits, &v, sizeof bits);
        u32_(bits);
    }
    template <usize N>
    void str(const FixedString<N>& s) {
        u16_(static_cast<u16>(s.size()));
        raw(s.c_str(), s.size());
    }
};

// ---- decode

struct Reader {
    Span<const byte> d;
    usize pos = 0;
    bool ok = true;

    bool need(usize n) {
        if (!ok || n > d.size() - pos) ok = false;
        return ok;
    }
    u8 u8_() {
        if (!need(1)) return 0;
        return static_cast<u8>(d[pos++]);
    }
    u16 u16_() {
        const u16 lo = u8_();
        const u16 hi = u8_();
        return static_cast<u16>(lo | (hi << 8));
    }
    u32 u32_() {
        const u32 lo = u16_();
        const u32 hi = u16_();
        return lo | (hi << 16);
    }
    u64 u64_() {
        const u64 lo = u32_();
        const u64 hi = u32_();
        return lo | (hi << 32);
    }
    f32 f32_() {
        const u32 bits = u32_();
        f32 v;
        std::memcpy(&v, &bits, sizeof v);
        return v;
    }
    template <usize N>
    void str(FixedString<N>& s) {
        const u16 n = u16_();
        if (!need(n)) return;
        s = StringView(reinterpret_cast<const char*>(d.data() + pos), n);
        pos += n;
        if (s.truncated()) ok = false;
    }
};

Error bad(u32 detail) { return Error(Errc::InvalidArgument, kStage, {}, detail); }

}  // namespace

u16 ItemCatalog::find_item(StringView id) const {
    for (usize i = 0; i < items_.size(); ++i)
        if (items_[i].id.view() == id) return static_cast<u16>(i);
    return kNoItem;
}

u16 ItemCatalog::find_table(StringView id) const {
    for (usize i = 0; i < tables_.size(); ++i)
        if (tables_[i].id.view() == id) return static_cast<u16>(i);
    return kNoItem;
}

bool ItemCatalog::has_tag(const ItemDef& d, StringView tag) const {
    for (const FixedString<32>& t : tags_of(d))
        if (t.view() == tag) return true;
    return false;
}

f32 ItemCatalog::stat(const ItemDef& d, StringView key, f32 def) const {
    for (const ItemStat& s : stats_of(d))
        if (s.key.view() == key) return s.value;
    return def;
}

bool ItemCatalog::add_item(const ItemDef& d, Span<const ItemStat> stats, Span<const FixedString<32>> tags) {
    if (items_.size() >= kItemMaxItems || stats.size() > kItemMaxStats || tags.size() > kItemMaxTags) return false;
    if (d.id.empty() || find_item(d.id.view()) != kNoItem) return false;
    ItemDef copy = d;
    copy.first_stat = static_cast<u32>(stats_.size());
    copy.stat_count = static_cast<u16>(stats.size());
    copy.first_tag = static_cast<u32>(tags_.size());
    copy.tag_count = static_cast<u16>(tags.size());
    for (const ItemStat& s : stats) stats_.push_back(s);
    for (const FixedString<32>& t : tags) tags_.push_back(t);
    items_.push_back(copy);
    return true;
}

bool ItemCatalog::add_table(const LootTable& t, Span<const LootEntry> entries) {
    if (tables_.size() >= kItemMaxTables || entries.size() > kItemMaxEntries) return false;
    if (t.id.empty() || find_table(t.id.view()) != kNoItem) return false;
    LootTable copy = t;
    copy.first_entry = static_cast<u32>(entries_.size());
    copy.entry_count = static_cast<u16>(entries.size());
    for (const LootEntry& e : entries) entries_.push_back(e);
    tables_.push_back(copy);
    return true;
}

void ItemCatalog::clear() {
    items_.clear();
    stats_.clear();
    tags_.clear();
    tables_.clear();
    entries_.clear();
}

const char* item_catalog_problem(const ItemCatalog& c, u32* index) {
    u32 at = 0;
    auto fail = [&](const char* what, u32 i) {
        at = i;
        if (index) *index = at;
        return what;
    };
    const Span<const ItemDef> items = c.items();
    const Span<const LootTable> tables = c.tables();
    if (items.size() > kItemMaxItems) return fail("item: too many items", 0);
    if (tables.size() > kItemMaxTables) return fail("table: too many loot tables", 0);
    for (usize i = 0; i < items.size(); ++i) {
        const ItemDef& it = items[i];
        const u32 n = static_cast<u32>(i);
        if (!id_ok(it.id)) return fail("item: the id must be 1-63 letters, digits, '_', '-' or '.'", n);
        if (it.name.truncated()) return fail("item: the name is too long", n);
        if (it.name.empty()) return fail("item: the name is empty", n);
        for (const char ch : it.name.view())
            if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F) return fail("item: the name has a control character", n);
        for (usize j = 0; j < i; ++j)
            if (items[j].id.view() == it.id.view()) return fail("item: two items share this id", n);
        if (it.stack_max < 1 || it.stack_max > kItemMaxStack) return fail("item: the stack size must be 1 to 1000000", n);
        if (it.weight_milli > kItemMaxWeightMilli) return fail("item: the weight is over 1000000 kg", n);
        if (it.stat_count > kItemMaxStats || usize(it.first_stat) + it.stat_count > c.stats().size()) return fail("item: too many stats", n);
        if (it.tag_count > kItemMaxTags || usize(it.first_tag) + it.tag_count > c.tags().size()) return fail("item: too many tags", n);
        const Span<const ItemStat> st = c.stats_of(it);
        for (usize s = 0; s < st.size(); ++s) {
            if (!id_ok(st[s].key)) return fail("item: a stat name must be 1-31 letters, digits, '_', '-' or '.'", n);
            if (!std::isfinite(st[s].value)) return fail("item: a stat value is not a finite number", n);
            for (usize k = 0; k < s; ++k)
                if (st[k].key.view() == st[s].key.view()) return fail("item: two stats share a name", n);
        }
        const Span<const FixedString<32>> tg = c.tags_of(it);
        for (usize s = 0; s < tg.size(); ++s) {
            if (!id_ok(tg[s])) return fail("item: a tag must be 1-31 letters, digits, '_', '-' or '.'", n);
            for (usize k = 0; k < s; ++k)
                if (tg[k].view() == tg[s].view()) return fail("item: a tag is listed twice", n);
        }
    }
    for (usize i = 0; i < tables.size(); ++i) {
        const LootTable& t = tables[i];
        const u32 n = static_cast<u32>(i);
        if (!id_ok(t.id)) return fail("table: the id must be 1-63 letters, digits, '_', '-' or '.'", n);
        for (usize j = 0; j < i; ++j)
            if (tables[j].id.view() == t.id.view()) return fail("table: two loot tables share this id", n);
        if (t.entry_count == 0 || t.entry_count > kItemMaxEntries || usize(t.first_entry) + t.entry_count > c.entries().size())
            return fail("table: a loot table needs 1 to 256 entries", n);
        if (t.rolls_min > t.rolls_max || t.rolls_max > kItemMaxRolls) return fail("table: rolls must be [min, max] with max at most 64", n);
        u64 weighted = 0;
        for (const LootEntry& e : c.entries_of(t)) {
            if (e.kind == LootKind::Item ? e.ref >= items.size() : e.ref >= tables.size())
                return fail("table: an entry names an item or table that does not exist", n);
            if (e.qty_min < 1 || e.qty_min > e.qty_max || e.qty_max > kItemMaxStack) return fail("table: an entry's quantity must be [min, max] within 1 to 1000000", n);
            if (!e.guaranteed) {
                if (e.weight < 1 || e.weight > kItemMaxEntryWeight) return fail("table: an entry's weight must be 1 to 1000000", n);
                weighted += e.weight;
            }
            if (e.kind == LootKind::Table && e.ref == i) return fail("table: a loot table cannot name itself", n);
        }
        if (t.rolls_max > 0 && weighted == 0) return fail("table: it rolls but has no weighted entry to roll", n);
    }
    // Nesting: the depth of every table's deepest path through other tables, with loops refused. A table's depth is
    // 1 + the deepest table it names; kItemMaxTableDepth caps the chain, so the walk is bounded.
    Array<u8> depth;  // 0 = not computed, 255 = on the current path
    for (usize i = 0; i < tables.size(); ++i) depth.push_back(0);
    struct Frame {
        u32 table;
        u32 next;
        u32 deepest;
    };
    Array<Frame> stack;
    for (usize root = 0; root < tables.size(); ++root) {
        if (depth[root] != 0) continue;
        stack.clear();
        stack.push_back({static_cast<u32>(root), 0, 0});
        depth[root] = 255;
        while (!stack.empty()) {
            Frame& f = stack[stack.size() - 1];
            const Span<const LootEntry> es = c.entries_of(tables[f.table]);
            bool descended = false;
            while (f.next < es.size()) {
                const LootEntry& e = es[f.next++];
                if (e.kind != LootKind::Table) continue;
                if (depth[e.ref] == 255) return fail("table: loot tables loop (a table names one that names it back)", f.table);
                if (depth[e.ref] == 0) {
                    if (stack.size() >= kItemMaxTableDepth) return fail("table: loot tables nest deeper than 8", f.table);
                    depth[e.ref] = 255;
                    stack.push_back({e.ref, 0, 0});
                    descended = true;
                    break;
                }
                if (depth[e.ref] > f.deepest) f.deepest = depth[e.ref];
            }
            if (descended) continue;
            const u32 mine = f.deepest + 1;
            const u32 table = f.table;
            stack.pop_back();
            if (mine > kItemMaxTableDepth) return fail("table: loot tables nest deeper than 8", table);
            depth[table] = static_cast<u8>(mine);
            if (!stack.empty() && mine > stack[stack.size() - 1].deepest) stack[stack.size() - 1].deepest = mine;
        }
    }
    if (index) *index = 0;
    return nullptr;
}

void encode_item_catalog(const ItemCatalog& c, Array<byte>& out) {
    out.clear();
    Writer w{out};
    w.u32_(static_cast<u32>(c.items().size()));
    w.u32_(static_cast<u32>(c.tables().size()));
    for (const ItemDef& it : c.items()) {
        w.str(it.id);
        w.str(it.name);
        w.u32_(it.stack_max);
        w.u32_(it.weight_milli);
        w.u64_(it.icon.hi);
        w.u64_(it.icon.lo);
        w.u64_(it.prefab.hi);
        w.u64_(it.prefab.lo);
        w.u8_(static_cast<u8>(it.stat_count));
        for (const ItemStat& s : c.stats_of(it)) {
            w.str(s.key);
            w.f32_(s.value);
        }
        w.u8_(static_cast<u8>(it.tag_count));
        for (const FixedString<32>& t : c.tags_of(it)) w.str(t);
    }
    for (const LootTable& t : c.tables()) {
        w.str(t.id);
        w.u16_(t.rolls_min);
        w.u16_(t.rolls_max);
        w.u16_(t.entry_count);
        for (const LootEntry& e : c.entries_of(t)) {
            w.u8_(static_cast<u8>(e.kind));
            w.u8_(e.guaranteed ? 1 : 0);
            w.u16_(e.ref);
            w.u32_(e.weight);
            w.u32_(e.qty_min);
            w.u32_(e.qty_max);
        }
    }
}

static Result<void> decode_item_catalog_impl(Span<const byte> payload, ItemCatalog& out) {
    out.clear();
    Reader r{payload};
    const u32 item_count = r.u32_();
    const u32 table_count = r.u32_();
    if (!r.ok) return Error(Errc::Truncated, kStage);
    if (item_count > kItemMaxItems || table_count > kItemMaxTables) return bad(1);
    for (u32 i = 0; i < item_count; ++i) {
        ItemDef d;
        r.str(d.id);
        r.str(d.name);
        d.stack_max = r.u32_();
        d.weight_milli = r.u32_();
        d.icon.hi = r.u64_();
        d.icon.lo = r.u64_();
        d.prefab.hi = r.u64_();
        d.prefab.lo = r.u64_();
        const u32 stat_count = r.u8_();
        if (stat_count > kItemMaxStats) return bad(2);
        Array<ItemStat> stats;
        for (u32 s = 0; s < stat_count; ++s) {
            ItemStat st;
            r.str(st.key);
            st.value = r.f32_();
            stats.push_back(st);
        }
        const u32 tag_count = r.u8_();
        if (tag_count > kItemMaxTags) return bad(2);
        Array<FixedString<32>> tags;
        for (u32 t = 0; t < tag_count; ++t) {
            FixedString<32> tag;
            r.str(tag);
            tags.push_back(tag);
        }
        if (!r.ok) return Error(Errc::Truncated, kStage);
        if (!out.add_item(d, stats.span(), tags.span())) return bad(3);  // an empty or repeated id
    }
    for (u32 i = 0; i < table_count; ++i) {
        LootTable t;
        r.str(t.id);
        t.rolls_min = r.u16_();
        t.rolls_max = r.u16_();
        const u32 entry_count = r.u16_();
        if (entry_count > kItemMaxEntries) return bad(4);
        Array<LootEntry> entries;
        for (u32 e = 0; e < entry_count; ++e) {
            LootEntry en;
            const u8 kind = r.u8_();
            const u8 guaranteed = r.u8_();
            if (kind > 1 || guaranteed > 1) return bad(5);
            en.kind = static_cast<LootKind>(kind);
            en.guaranteed = guaranteed != 0;
            en.ref = r.u16_();
            en.weight = r.u32_();
            en.qty_min = r.u32_();
            en.qty_max = r.u32_();
            entries.push_back(en);
        }
        if (!r.ok) return Error(Errc::Truncated, kStage);
        if (!out.add_table(t, entries.span())) return bad(3);
    }
    if (!r.ok) return Error(Errc::Truncated, kStage);
    if (r.pos != payload.size()) return bad(6);  // trailing bytes
    u32 at = 0;
    if (item_catalog_problem(out, &at)) return bad(100 + at);
    return ok();
}

Result<void> decode_item_catalog(Span<const byte> payload, ItemCatalog& out) {
    Result<void> r = decode_item_catalog_impl(payload, out);
    if (!r) out.clear();  // a caller that keeps going after an error must not see a half-read or invalid catalog
    return r;
}

}  // namespace mce::asset
