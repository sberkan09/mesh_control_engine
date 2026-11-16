#pragma once
// StringsGrid — every locale's string table as one grid (M-STRINGSED): a row per key, a column per locale. The tables
// are assets/loc/<locale>.csv (mce/loc/string_table.h has the format); the grid reads them with an RFC 4180 reader
// that keeps comment rows and extra columns, edits cells in memory, and writes back only the locales that changed,
// each through the cook-first write (validate_and_save_source), so a table the cook refuses is not written.
// Headless, tested in CI; the Strings panel draws it.

#include "mce/cook/string.h"
#include "mce/editor/asset_form.h"
#include "mce/editor/project.h"

namespace mce::editor {

class StringsGrid {
public:
    struct Locale {
        cook::String path;  // relative to assets/, e.g. "loc/tr.csv"
        cook::String tag;   // the file stem: "tr"
        asset::Guid guid;
        bool dirty = false;
    };

    // Reads every StringTable asset of the project; the "en" locale (else the first by path) is the base, column 0.
    bool load(const EditorProject& project, cook::String& err);
    Span<const Locale> locales() const { return Span<const Locale>(locales_.data(), locales_.size()); }
    // Every key of every locale, the base locale's keys first in file order, then the others' new keys. A plural
    // form is a key of its own ("score.coins#other").
    Span<const cook::String> keys() const { return Span<const cook::String>(keys_.data(), keys_.size()); }

    // The cell, or an empty view when the locale has no row for the key.
    StringView text(u32 locale, u32 key) const;
    bool has(u32 locale, u32 key) const { return locale < rows_.size() && key < keys_.size() && rows_[locale][key] >= 0; }
    // Sets the cell, adding the row (after the last row) when the locale lacks the key.
    void set_text(u32 locale, u32 key, StringView text);
    // Adds a key with an empty text to the base locale. False when it exists already or is empty. The index of the
    // new key is keys().size() - 1.
    bool add_key(StringView key);
    // Removes the key's row from every locale.
    void remove_key(u32 key);
    // How many keys the locale lacks (the base locale lacks none by definition).
    u32 missing(u32 locale) const;
    bool dirty() const;

    // Writes the changed locales, each cooked first. Stops at the first refusal with `err` set; the locales saved
    // before it stay saved. Rescans the catalog (project asset pointers are invalid afterwards).
    bool save(EditorProject& project, cook::String& err);

private:
    using Record = Array<cook::String>;
    struct Table {
        Array<Record> records;  // records[0] is the header
    };
    void rebuild_index();

    Array<Locale> locales_;
    Array<Table> tables_;
    Array<cook::String> keys_;
    Array<Array<i32>> rows_;  // [locale][key] -> record index, -1 when missing
};

}  // namespace mce::editor
