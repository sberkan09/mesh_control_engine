#include "mce/loc/loc.h"

#include "mce/ecs/save.h"

namespace mce::loc {

namespace {

char lower_ascii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }
char upper_ascii(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; }
bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool alnum(char c) { return alpha(c) || (c >= '0' && c <= '9'); }

bool turkic(StringView locale) {
    const StringView lang = language_of(locale);
    return lang == "tr" || lang == "az";
}

// Decodes one code point at s[i] (advancing i). A malformed byte comes back as itself with
// `raw` set, so the mapping passes it through untouched.
u32 decode(StringView s, usize& i, bool& raw) {
    const auto* p = reinterpret_cast<const u8*>(s.data());
    const u8 c = p[i];
    raw = false;
    u32 len = 1, cp = c;
    if (c >= 0x80) {
        if ((c & 0xE0) == 0xC0) len = 2, cp = c & 0x1F;
        else if ((c & 0xF0) == 0xE0) len = 3, cp = c & 0x0F;
        else if ((c & 0xF8) == 0xF0) len = 4, cp = c & 0x07;
        else len = 0;
        if (len == 0 || s.size() - i < len) {
            raw = true;
            ++i;
            return c;
        }
        for (u32 k = 1; k < len; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) {
                raw = true;
                ++i;
                return c;
            }
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
    }
    i += len;
    return cp;
}

void encode(u32 cp, Array<char>& out) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Latin Extended-A pairs: in U+0100..U+0137 and U+014A..U+0177 the even code point is the capital,
// in U+0139..U+0148 and U+0179..U+017E the odd one is.
bool ext_a_even_upper(u32 cp) { return (cp >= 0x100 && cp <= 0x137) || (cp >= 0x14A && cp <= 0x177); }
bool ext_a_odd_upper(u32 cp) { return (cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E); }

u32 upper_cp(u32 cp, bool tr) {
    if (cp < 0x80) return (tr && cp == 'i') ? 0x130 : static_cast<u32>(upper_ascii(static_cast<char>(cp)));
    if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) return cp - 0x20;
    if (cp == 0xFF) return 0x178;
    if (cp == 0x131) return 'I';  // dotless i
    if (cp == 0x17F) return 'S';  // long s
    if (ext_a_even_upper(cp) && cp != 0x130) return (cp & 1u) ? cp - 1 : cp;
    if (ext_a_odd_upper(cp)) return (cp & 1u) ? cp : cp - 1;
    if (cp == 0x3C2) return 0x3A3;  // final sigma
    if (cp >= 0x3B1 && cp <= 0x3C9) return cp - 0x20;
    if (cp >= 0x430 && cp <= 0x44F) return cp - 0x20;
    if (cp >= 0x450 && cp <= 0x45F) return cp - 0x50;
    return cp;
}

u32 lower_cp(u32 cp, bool tr) {
    if (cp < 0x80) return (tr && cp == 'I') ? 0x131 : static_cast<u32>(lower_ascii(static_cast<char>(cp)));
    if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return cp + 0x20;
    if (cp == 0x178) return 0xFF;
    if (ext_a_even_upper(cp) && cp != 0x131) return (cp & 1u) ? cp : cp + 1;
    if (ext_a_odd_upper(cp)) return (cp & 1u) ? cp + 1 : cp;
    if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) return cp + 0x20;
    if (cp >= 0x410 && cp <= 0x42F) return cp + 0x20;
    if (cp >= 0x400 && cp <= 0x40F) return cp + 0x50;
    return cp;
}

void append_view(Array<char>& out, StringView s) {
    for (const char c : s) out.push_back(c);
}

void append_int(Array<char>& out, i64 v) {
    char buf[24];
    usize n = 0;
    u64 mag = v < 0 ? u64(0) - static_cast<u64>(v) : static_cast<u64>(v);
    do {
        buf[n++] = static_cast<char>('0' + mag % 10);
        mag /= 10;
    } while (mag);
    if (v < 0) out.push_back('-');
    while (n) out.push_back(buf[--n]);
}

bool contains(const Array<LocaleTag>& list, StringView tag) {
    for (const LocaleTag& t : list)
        if (t.view() == tag) return true;
    return false;
}

}  // namespace

bool normalize_locale(StringView in, LocaleTag& out) {
    out.clear();
    usize i = 0;
    while (i < in.size() && alpha(in[i])) ++i;
    if (i < 2 || i > 3) return false;
    out.clear();
    char lang[4] = {};
    for (usize k = 0; k < i; ++k) lang[k] = lower_ascii(in[k]);
    out.append(StringView(lang, i));
    // Then any subtags up to an encoding or modifier ("tr_TR.UTF-8", "sr_RS@latin").
    while (i < in.size() && (in[i] == '-' || in[i] == '_')) {
        usize j = i + 1;
        while (j < in.size() && alnum(in[j])) ++j;
        const usize len = j - i - 1;
        if (len == 0 || len > 8) break;
        char sub[9] = {};
        for (usize k = 0; k < len; ++k) {
            const char c = in[i + 1 + k];
            // Region (2 letters) upper case, script (4 letters) title case, the rest lower case.
            if (len == 2) sub[k] = upper_ascii(c);
            else if (len == 4) sub[k] = k == 0 ? upper_ascii(c) : lower_ascii(c);
            else sub[k] = lower_ascii(c);
        }
        out.append("-");
        out.append(StringView(sub, len));
        i = j;
    }
    if (out.truncated()) {
        out.clear();
        return false;
    }
    return true;
}

StringView language_of(StringView tag) {
    const usize dash = tag.find('-');
    return dash == StringView::npos ? tag : tag.substr(0, dash);
}

PluralForm plural_form(StringView locale, i64 count) {
    const StringView lang = language_of(locale);
    const u64 n = count < 0 ? u64(0) - static_cast<u64>(count) : static_cast<u64>(count);
    const u64 n10 = n % 10, n100 = n % 100;
    // No plural distinction.
    for (const char* l : {"ja", "zh", "ko", "th", "vi", "id", "ms", "lo", "my", "km", "yue"})
        if (lang == l) return PluralForm::Other;
    // 0 and 1 are singular.
    if (lang == "fr" || lang == "hy" || lang == "ff" || lang == "kab" || (lang == "pt" && locale != "pt-PT"))
        return n <= 1 ? PluralForm::One : PluralForm::Other;
    if (lang == "ru" || lang == "uk" || lang == "be") {
        if (n10 == 1 && n100 != 11) return PluralForm::One;
        if (n10 >= 2 && n10 <= 4 && (n100 < 12 || n100 > 14)) return PluralForm::Few;
        return PluralForm::Many;
    }
    if (lang == "pl") {
        if (n == 1) return PluralForm::One;
        if (n10 >= 2 && n10 <= 4 && (n100 < 12 || n100 > 14)) return PluralForm::Few;
        return PluralForm::Many;
    }
    if (lang == "cs" || lang == "sk") {
        if (n == 1) return PluralForm::One;
        if (n >= 2 && n <= 4) return PluralForm::Few;
        return PluralForm::Other;
    }
    if (lang == "ar") {
        if (n == 0) return PluralForm::Zero;
        if (n == 1) return PluralForm::One;
        if (n == 2) return PluralForm::Two;
        if (n100 >= 3 && n100 <= 10) return PluralForm::Few;
        if (n100 >= 11) return PluralForm::Many;
        return PluralForm::Other;
    }
    if (lang == "he") {
        if (n == 1) return PluralForm::One;
        if (n == 2) return PluralForm::Two;
        return PluralForm::Other;
    }
    // English, Turkish, German, Spanish, Italian, Dutch, the Nordic languages, ...
    return n == 1 ? PluralForm::One : PluralForm::Other;
}

void to_upper(StringView locale, StringView text, Array<char>& out) {
    const bool tr = turkic(locale);
    for (usize i = 0; i < text.size();) {
        bool raw = false;
        const u32 cp = decode(text, i, raw);
        if (raw) out.push_back(static_cast<char>(cp));
        else if (cp == 0xDF) append_view(out, "SS");  // ß
        else encode(upper_cp(cp, tr), out);
    }
}

void to_lower(StringView locale, StringView text, Array<char>& out) {
    const bool tr = turkic(locale);
    for (usize i = 0; i < text.size();) {
        bool raw = false;
        const u32 cp = decode(text, i, raw);
        if (raw) out.push_back(static_cast<char>(cp));
        else if (cp == 0x130) append_view(out, tr ? "i" : "i\xCC\x87");  // İ: "i" + combining dot outside Turkic
        else encode(lower_cp(cp, tr), out);
    }
}

void format(StringView pattern, Span<const Arg> args, Array<char>& out) {
    for (usize i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if ((c == '{' || c == '}') && i + 1 < pattern.size() && pattern[i + 1] == c) {
            out.push_back(c);
            ++i;
            continue;
        }
        if (c == '{') {
            const usize close = pattern.find('}', i + 1);
            if (close != StringView::npos) {
                const StringView name = pattern.substr(i + 1, close - i - 1);
                const Arg* hit = nullptr;
                for (const Arg& a : args)
                    if (a.name == name) hit = &a;
                if (hit) {
                    append_view(out, hit->value);
                    i = close;
                    continue;
                }
            }
        }
        out.push_back(c);
    }
}

void fallback_chain(Span<const LocaleTag> preferred, Span<const LocaleTag> available, StringView default_locale,
                    Array<LocaleTag>& out) {
    out.clear();
    auto add_matches = [&](StringView want) {
        if (want.empty()) return;
        const StringView lang = language_of(want);
        for (const LocaleTag& a : available)  // exact
            if (a.view() == want && !contains(out, a.view())) out.push_back(a);
        for (const LocaleTag& a : available)  // the bare language
            if (a.view() == lang && !contains(out, a.view())) out.push_back(a);
        for (const LocaleTag& a : available)  // any region of it
            if (language_of(a.view()) == lang && !contains(out, a.view())) out.push_back(a);
    };
    for (const LocaleTag& p : preferred) add_matches(p.view());
    add_matches(default_locale);
    if (out.empty() && !available.empty()) out.push_back(available[0]);
}

Localization::Localization(mem::IAllocator& a) : tables_(a), preferred_(a), chain_(a) {}

const StringTable* Localization::table_for(StringView tag) const {
    for (const StringTable& t : tables_)
        if (t.locale() == tag) return &t;
    return nullptr;
}

void Localization::add_table(StringTable table) {
    usize at = 0;
    while (at < tables_.size() && tables_[at].locale() < table.locale()) ++at;  // sorted by tag
    if (at < tables_.size() && tables_[at].locale() == table.locale()) tables_[at] = std::move(table);
    else tables_.insert(at, std::move(table));
    rebuild();
}

void Localization::set_default_locale(StringView tag) {
    LocaleTag t;
    default_ = normalize_locale(tag, t) ? t : LocaleTag();
    rebuild();
}

void Localization::set_preferred(Span<const LocaleTag> device) {
    preferred_.clear();
    for (const LocaleTag& d : device) {
        LocaleTag t;
        if (normalize_locale(d.view(), t)) preferred_.push_back(t);
    }
    rebuild();
}

bool Localization::set_locale(StringView tag) {
    LocaleTag t;
    if (!tag.empty()) {
        if (!normalize_locale(tag, t)) return false;
        bool served = false;
        for (const StringTable& table : tables_) served = served || language_of(table.locale()) == language_of(t.view());
        if (!served) return false;
    }
    user_ = t;
    rebuild();
    if (!persist_.empty()) {
        const StringView v = user_.view();
        if (Result<void> r = save::write(persist_.view(), Span<const byte>(reinterpret_cast<const byte*>(v.data()), v.size())); !r)
            (void)r;  // a read-only save folder keeps the choice for this run only
    }
    return true;
}

Result<void> Localization::load_choice(StringView save_name) {
    if (!save::exists(save_name)) return ok();
    Array<byte> bytes;
    MCE_TRY(save::read(save_name, bytes));
    const StringView v(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    LocaleTag t;
    if (v.empty() || !normalize_locale(v, t)) return ok();  // nothing chosen, or not a tag: follow the device
    for (const StringTable& table : tables_)
        if (language_of(table.locale()) == language_of(t.view())) {
            user_ = t;
            rebuild();
            break;
        }
    return ok();
}

void Localization::rebuild() {
    Array<LocaleTag> want;
    // A language the player chose stands alone: a key its table lacks falls back to the base
    // language (or the caller's own text), never to the device's other languages (SBA-30, 11a).
    if (!user_.empty()) want.push_back(user_);
    else
        for (const LocaleTag& p : preferred_) want.push_back(p);
    Array<LocaleTag> available;
    for (const StringTable& t : tables_) available.push_back(LocaleTag(t.locale()));
    Array<LocaleTag> next;
    fallback_chain(want.span(), available.span(), default_.view(), next);
    bool same = next.size() == chain_.size();
    for (usize i = 0; same && i < next.size(); ++i) same = next[i].view() == chain_[i].view();
    if (!same) {
        chain_ = std::move(next);
        ++generation_;
    }
}

bool Localization::try_text(StringView key, StringView& out) const {
    for (const LocaleTag& tag : chain_)
        if (const StringTable* t = table_for(tag.view()); t && t->find(key, out)) return true;
    return false;
}

StringView Localization::text(StringView key) const {
    StringView s;
    return try_text(key, s) ? s : key;
}

void Localization::format(StringView key, Span<const Arg> args, Array<char>& out) const {
    loc::format(text(key), args, out);
}

void Localization::plural(StringView key, i64 count, Span<const Arg> args, Array<char>& out) const {
    StringView pattern = key;
    for (const LocaleTag& tag : chain_) {
        const StringTable* t = table_for(tag.view());
        if (!t || !t->has_key(key)) continue;
        if (!t->find(key, plural_form(t->locale(), count), pattern) && !t->find(key, PluralForm::Other, pattern))
            continue;  // only other forms here: try the next language
        break;
    }
    Array<char> num;
    append_int(num, count);
    Array<Arg> all;
    all.push_back(Arg{"count", StringView(num.data(), num.size())});
    for (const Arg& a : args) all.push_back(a);
    loc::format(pattern, all.span(), out);
}

}  // namespace mce::loc
