#pragma once
// Localization (M-MOBILE step 11a, issue #146; reopens N12): the game's text in the player's
// language, with plural rules, placeholders and locale-aware case mapping.
//
// The player loads every cooked string table in the pack (assets/loc/<locale>.csv, string_table.h)
// into a Localization and sets it as a world resource; without tables there is no resource
// (try_resource is null). Game code:
//
//   if (const loc::Localization* l10n = f.try_resource<loc::Localization>()) {
//       label = l10n->text("menu.play");                                    // "Play" / "Oyna"
//       l10n->plural("score.coins", coins, {}, buf);                        // "1 coin" / "5 coins"
//       const loc::Arg name[] = {{"name", player_name}};
//       l10n->format("menu.greeting", name, buf);                           // "Hello, Ada!"
//   }
//   if (loc::Localization* l10n = f.try_resource<loc::Localization>()) l10n->set_locale("tr");  // a language menu
//
// Which language: the player's own choice (set_locale, kept in the save folder so it survives a
// relaunch), then the device languages in order (platform::preferred_locales), then the project's
// default locale. Each step tries the exact tag, then its language ("pt-BR" -> "pt"), then any
// region of that language ("pt" -> "pt-PT"). Lookups walk that chain, so a key missing from the
// Turkish table falls back to the English text rather than showing nothing; a key missing
// everywhere shows the key itself (visible in play, never a crash; the cook warns about it first).
//
// Determinism: the language is presentation state. Nothing here enters World::state_hash, and the
// text a system reads must not steer the simulation (compare keys, never translated text).

#include "mce/loc/string_table.h"

namespace mce::loc {

// "tr_TR.UTF-8", "TR-tr", "en" -> "tr-TR", "tr-TR", "en". False when `in` does not start with a
// 2- or 3-letter language (a "C" or "POSIX" locale is not one).
bool normalize_locale(StringView in, LocaleTag& out);
// "tr-TR" -> "tr".
StringView language_of(StringView tag);

// The CLDR plural category of `n` in `locale` (English-like one/other by default; Turkish, French,
// the East and West Slavic languages, Arabic, the CJK languages and others have their own rules).
PluralForm plural_form(StringView locale, i64 n);

// Case mapping of UTF-8 text that knows Turkish and Azeri: upper("i") is "İ" and lower("I") is "ı"
// there, and the usual "I"/"i" everywhere else. Covers ASCII, Latin-1, Latin Extended-A, Greek and
// Cyrillic; "ß" uppercases to "SS". Other code points pass through unchanged. Appends to `out`.
void to_upper(StringView locale, StringView text, Array<char>& out);
void to_lower(StringView locale, StringView text, Array<char>& out);

// A named placeholder value.
struct Arg {
    StringView name;
    StringView value;
};

// "{name}" -> the value of the arg called name; "{{" and "}}" are literal braces; a placeholder no
// arg names stays as written, so a missing value is visible. Appends to `out`.
void format(StringView pattern, Span<const Arg> args, Array<char>& out);

// The locales to try, best first, from what the player prefers and what the tables cover (see the
// header comment). Each available locale appears once; `default_locale` (when available) is last
// unless an earlier step already chose it.
void fallback_chain(Span<const LocaleTag> preferred, Span<const LocaleTag> available, StringView default_locale,
                    Array<LocaleTag>& out);

class Localization {
public:
    explicit Localization(mem::IAllocator& a = mem::heap());

    // Adds one locale's table; a table for the same locale replaces the earlier one.
    void add_table(StringTable table);
    Span<const StringTable> tables() const { return tables_.span(); }
    // The project's base language (default "en"): the last fallback, and what the cook compares
    // the other tables against.
    void set_default_locale(StringView tag);
    StringView default_locale() const { return default_.view(); }

    // The device's languages, best first (platform::preferred_locales).
    void set_preferred(Span<const LocaleTag> device);
    // The player's choice from a language menu, ahead of the device languages; "" goes back to
    // following the device. False (nothing changes) when no table serves the tag. With persistence
    // on (persist_choice) the choice is written to the save folder at once.
    bool set_locale(StringView tag);
    StringView user_locale() const { return user_.view(); }

    // The locale of the first table in the chain ("" without tables) and the whole chain.
    StringView locale() const { return chain_.empty() ? StringView() : chain_[0].view(); }
    Span<const LocaleTag> chain() const { return chain_.span(); }
    // Bumped on every change of the chain, so a UI can re-resolve its labels when it moves.
    u32 generation() const { return generation_; }

    // Walks the chain. text() returns the key itself when no table has it.
    bool try_text(StringView key, StringView& out) const;
    StringView text(StringView key) const;
    // text(key) through loc::format. Appends to `out`.
    void format(StringView key, Span<const Arg> args, Array<char>& out) const;
    // The plural form of `key` for `count` in the locale of the first table that has the key
    // (falling back to its "other" form), formatted with {count} and `args`. Appends to `out`.
    void plural(StringView key, i64 count, Span<const Arg> args, Array<char>& out) const;
    // Case mapping in the active locale (to_upper / to_lower). Appends to `out`.
    void upper(StringView text, Array<char>& out) const { to_upper(locale(), text, out); }
    void lower(StringView text, Array<char>& out) const { to_lower(locale(), text, out); }

    // The choice survives a relaunch: load_choice reads the save (save::read, under save::root())
    // and applies it when a table serves it; persist_choice makes set_locale write it. A missing
    // save is not an error.
    Result<void> load_choice(StringView save_name = "language");
    void persist_choice(StringView save_name = "language") { persist_ = save_name; }

private:
    void rebuild();
    const StringTable* table_for(StringView tag) const;

    Array<StringTable> tables_;
    Array<LocaleTag> preferred_;
    Array<LocaleTag> chain_;
    LocaleTag default_ = "en";
    LocaleTag user_;
    FixedString<64> persist_;
    u32 generation_ = 0;
};

}  // namespace mce::loc
