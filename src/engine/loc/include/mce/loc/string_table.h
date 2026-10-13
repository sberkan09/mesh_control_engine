#pragma once
// String tables (M-MOBILE step 11a, issue #146; reopens N12): one locale's translated text, keyed
// by the names game code uses. Authored as assets/loc/<locale>.csv, cooked to one StringTable chunk
// (CHNK_STR) per locale (the runtime never parses text, FR-AST-010) and handed to loc::Localization.
//
//   key,text
//   menu.play,Play
//   menu.greeting,"Hello, {name}!"
//   score.coins#one,{count} coin
//   score.coins#other,{count} coins
//
// A key is letters, digits, '_', '-' and '.'. A "#form" suffix (zero, one, two, few, many, other)
// marks one CLDR plural form of the key; Localization::plural picks it by count and locale.
//
// Chunk layout v1 (little-endian):
//   StringTableHeader | StringTableEntry[entry_count] (sorted by key, then form) | UTF-8 blob

#include "mce/containers/array.h"
#include "mce/fixed_string.h"
#include "mce/result.h"

namespace mce::loc {

// CLDR plural categories, in CLDR order. Other is the form every key without a suffix has.
enum class PluralForm : u8 { Zero, One, Two, Few, Many, Other };

const char* plural_form_name(PluralForm f);
bool parse_plural_form(StringView s, PluralForm& out);

// "tr-TR", "en", "pt-BR": a BCP 47 language tag, at most 15 bytes.
using LocaleTag = FixedString<16>;

// True for a key the CSV and the code may name: non-empty, letters, digits, '_', '-' and '.'.
bool valid_key(StringView key);
// True when `s` is well-formed UTF-8 (no overlongs, no surrogates, nothing past U+10FFFF).
bool valid_utf8(StringView s);

class StringTable {
public:
    struct Entry {
        StringView key;
        StringView text;
        PluralForm form = PluralForm::Other;
    };

    explicit StringTable(mem::IAllocator& a = mem::heap()) : blob_(a), entries_(a) {}

    StringView locale() const { return locale_.view(); }
    void set_locale(StringView tag) { locale_ = tag; }

    // False when the key is invalid, the text is not UTF-8 or (key, form) is already present.
    bool add(StringView key, StringView text, PluralForm form = PluralForm::Other);
    // The text of (key, form); false when the table does not have it.
    bool find(StringView key, PluralForm form, StringView& out) const;
    bool find(StringView key, StringView& out) const { return find(key, PluralForm::Other, out); }
    // True when the key is present in any form.
    bool has_key(StringView key) const;

    usize size() const { return entries_.size(); }
    // Sorted by key, then form.
    Entry entry(usize i) const;
    void clear() {
        blob_.clear();
        entries_.clear();
    }

private:
    struct Slot {
        u32 key_off, key_size, text_off, text_size;
        PluralForm form;
    };
    StringView key_of(const Slot& s) const { return StringView(blob_.data() + s.key_off, s.key_size); }
    usize lower_bound(StringView key, PluralForm form) const;

    LocaleTag locale_;
    Array<char> blob_;
    Array<Slot> entries_;
};

// The StringTable chunk: written by the cook (encode), read by the runtime (decode, which
// bounds-checks every offset and rejects unsorted, duplicate, invalid-key or non-UTF-8 entries).
void encode_table(const StringTable& t, Array<byte>& out);
Result<void> decode_table(Span<const byte> payload, StringTable& out);

}  // namespace mce::loc
