// Translations of the story (M-MOBILE step 11e): which language's table the box shows, and the
// lookups from the run's base text ids to that table's lines (novel.h).
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

u32 scale_reveal(u32 revealed, u32 base_points, u32 shown_points) {
    if (revealed >= base_points || base_points == 0) return shown_points;
    return static_cast<u32>(static_cast<u64>(revealed) * shown_points / base_points);
}

}  // namespace mce::novel
