// Translations of the story (M-MOBILE step 11e): which language's table the box shows, and the
// lookups from the run's base text ids to that table's lines; the kit's own words and the story's
// titles through the string tables (SBA-59) (novel.h).
#include <cstdio>
#include <cstring>

#include "mce/kit/novel.h"
#include "mce/loc/loc.h"

namespace mce::novel {
namespace {

bool same_tag(StringView a, StringView b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        char x = a[i] == '_' ? '-' : a[i], y = b[i] == '_' ? '-' : b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

bool less(const u16* a, const u16* b, u32 n) {
    for (u32 i = 0; i < n; ++i)
        if (a[i] != b[i]) return a[i] < b[i];
    return false;
}

// `s` into out[cap], NUL-terminated, cut before a code point that does not fit.
void copy_out(StringView s, char* out, u32 cap) {
    usize n = s.size() < cap - 1 ? s.size() : cap - 1;
    while (n > 0 && n < s.size() && (static_cast<u8>(s[n]) & 0xC0) == 0x80) --n;
    if (n) std::memcpy(out, s.data(), n);
    out[n] = 0;
}

}  // namespace

const LocaleText* story_locale(const Program& p, const loc::Localization* l10n) {
    if (!l10n || p.locale_count == 0) return nullptr;
    const StringView base(p.base_locale ? p.base_locale : "en");
    for (const loc::LocaleTag& tag : l10n->chain()) {
        if (same_tag(tag.view(), base) || same_tag(loc::language_of(tag.view()), base)) return nullptr;
        for (u32 i = 0; i < p.locale_count; ++i)
            if (same_tag(tag.view(), StringView(p.locales[i].locale))) return &p.locales[i];
        for (u32 i = 0; i < p.locale_count; ++i)
            if (same_tag(loc::language_of(tag.view()), loc::language_of(StringView(p.locales[i].locale)))) return &p.locales[i];
    }
    return nullptr;
}

const LocaleText* story_locale(const Program& p, World& world) {
    return p.locale_count ? story_locale(p, world.try_resource<loc::Localization>()) : nullptr;
}

void localized_page(const Program&, const LocaleText* lt, const u16 base[4], u16 out[4]) {
    for (u32 i = 0; i < 4; ++i) out[i] = base[i];
    if (!lt || lt->page_count == 0) return;
    u32 lo = 0, hi = lt->page_count;  // the first entry not less than base
    while (lo < hi) {
        const u32 mid = (lo + hi) / 2;
        if (less(lt->pages[mid].base, base, 4)) lo = mid + 1;
        else hi = mid;
    }
    if (lo < lt->page_count && !less(base, lt->pages[lo].base, 4))
        for (u32 i = 0; i < 4; ++i) out[i] = lt->pages[lo].line[i];
}

void localized_option(const Program&, const LocaleText* lt, const Option& o, u16& text, u16& text2) {
    text = o.text;
    text2 = o.text2;
    if (!lt || lt->option_count == 0) return;
    const u16 key[2] = {o.text, o.text2};
    u32 lo = 0, hi = lt->option_count;
    while (lo < hi) {
        const u32 mid = (lo + hi) / 2;
        const u16 at[2] = {lt->options[mid].base, lt->options[mid].base2};
        if (less(at, key, 2)) lo = mid + 1;
        else hi = mid;
    }
    if (lo < lt->option_count && lt->options[lo].base == o.text && lt->options[lo].base2 == o.text2) {
        text = lt->options[lo].text;
        text2 = lt->options[lo].text2;
    }
}

const char* localized_speaker(const Program& p, const LocaleText* lt, u8 character) {
    if (lt && lt->names && character < p.character_count && lt->names[character]) return text(p, lt->names[character]);
    return speaker_name(p, character);
}

void kit_text(const loc::Localization* l10n, StringView key, StringView fallback, char* out, u32 cap,
              Span<const loc::Arg> args, bool touch) {
    if (cap == 0) return;
    StringView pattern = fallback;
    if (l10n) {
        FixedString<96> touch_key;
        if (touch) {
            touch_key.assign(key);
            touch_key.append(".touch");
        }
        StringView found;
        if ((touch && l10n->try_text(touch_key.view(), found)) || l10n->try_text(key, found)) pattern = found;
    }
    Array<char> text;
    loc::format(pattern, args, text);
    copy_out(StringView(text.data(), text.size()), out, cap);
}

void kit_count(const loc::Localization* l10n, StringView key, i64 count, StringView one, StringView other, char* out,
               u32 cap) {
    if (cap == 0) return;
    bool table_has = false;  // a table on the player's chain has the key (else the English words)
    if (l10n)
        for (const loc::LocaleTag& tag : l10n->chain())
            for (const loc::StringTable& t : l10n->tables())
                table_has = table_has || (t.locale() == tag.view() && t.has_key(key));
    Array<char> text;
    if (table_has) {
        l10n->plural(key, count, {}, text);
    } else {
        char num[24];
        std::snprintf(num, sizeof(num), "%lld", static_cast<long long>(count));
        const loc::Arg arg[] = {{"count", StringView(num)}};
        loc::format(count == 1 ? one : other, arg, text);
    }
    copy_out(StringView(text.data(), text.size()), out, cap);
}

u32 scale_reveal(u32 revealed, u32 base_points, u32 shown_points) {
    if (revealed >= base_points || base_points == 0) return shown_points;
    return static_cast<u32>(static_cast<u64>(revealed) * shown_points / base_points);
}

}  // namespace mce::novel
