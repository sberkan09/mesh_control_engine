// The program's lookups, the story variables and conditions, UTF-8 helpers (novel.h).
#include "mce/kit/novel.h"

namespace mce::novel {

u16 find_label(const Program& p, StringView name) {
    for (u32 i = 0; i < p.label_count; ++i)
        if (name == StringView(p.labels[i].name)) return p.labels[i].op;
    return kNone;
}

u16 find_var(const Program& p, StringView name) {
    for (u32 i = 0; i < p.var_count; ++i)
        if (name == StringView(p.vars[i].name)) return p.vars[i].ref;
    return kNone;
}

i32 get_var(const StoryState& s, u16 ref) {
    if (ref & kCounterBit) {
        const u32 i = ref & ~kCounterBit;
        return i < kMaxCounters ? s.counters[i] : 0;
    }
    return ref < kMaxFlags ? static_cast<i32>((s.flags[ref >> 5] >> (ref & 31)) & 1u) : 0;
}

void set_var(StoryState& s, const Program& p, u16 ref, i32 value) {
    if (ref & kCounterBit) {
        const u32 i = ref & ~kCounterBit;
        if (i >= kMaxCounters) return;
        i32 lo = -32768, hi = 32767;
        for (u32 v = 0; v < p.var_count; ++v)
            if (p.vars[v].ref == ref) {
                lo = p.vars[v].min;
                hi = p.vars[v].max;
                break;
            }
        s.counters[i] = static_cast<i16>(math::clamp(value, lo, hi));
        return;
    }
    if (ref >= kMaxFlags) return;
    const u32 bit = 1u << (ref & 31);
    s.flags[ref >> 5] = value ? (s.flags[ref >> 5] | bit) : (s.flags[ref >> 5] & ~bit);
}

namespace {

bool clause(const StoryState& s, const Clause& c) {
    const i32 v = c.test == Test::Always ? 0 : get_var(s, c.var);
    switch (c.test) {
        case Test::Always: return true;
        case Test::Set: return v != 0;
        case Test::Clear: return v == 0;
        case Test::Ge: return v >= c.n;
        case Test::Le: return v <= c.n;
        case Test::Eq: return v == c.n;
        case Test::Ne: return v != c.n;
        case Test::Gt: return v > c.n;
        case Test::Lt: return v < c.n;
    }
    return false;
}

}  // namespace

bool test(const StoryState& s, const Program& p, u16 cond) {
    if (cond >= p.clause_count) return cond == 0;  // a program with no clause table: 0 is "always"
    // A disjunction of conjunctions: `and` binds tighter than `or`.
    bool any = false, all = true;
    for (u32 i = cond; i < p.clause_count; ++i) {
        const Clause& c = p.clauses[i];
        all = all && clause(s, c);
        if (c.join != Join::And) {
            any = any || all;
            all = true;
        }
        if (c.join == Join::End) break;
    }
    return any;
}

u32 utf8_points(StringView s) {
    u32 n = 0;
    for (char c : s) n += (static_cast<u8>(c) & 0xC0) != 0x80 ? 1u : 0u;
    return n;
}

StringView utf8_prefix(StringView s, u32 points) {
    usize i = 0;
    u32 n = 0;
    while (i < s.size()) {
        if ((static_cast<u8>(s[i]) & 0xC0) != 0x80) {
            if (n == points) break;
            ++n;
        }
        ++i;
    }
    return StringView(s.data(), i);
}

const char* text(const Program& p, u16 id) { return id < p.text_count ? p.text[id] : ""; }

const char* speaker_name(const Program& p, u8 character) {
    return character < p.character_count ? p.characters[character].name : "";
}

}  // namespace mce::novel
