#include "mce/editor/strings_grid.h"

#include "mce/cook/fs.h"

namespace mce::editor {
namespace {

// RFC 4180: quoted fields may hold commas, quotes ("") and line breaks. Blank lines are skipped. A BOM is dropped.
bool parse_records(StringView s, Array<Array<cook::String>>& out, cook::String& err) {
    if (s.size() >= 3 && static_cast<u8>(s[0]) == 0xEF && static_cast<u8>(s[1]) == 0xBB && static_cast<u8>(s[2]) == 0xBF) s = s.substr(3);
    usize i = 0;
    while (i < s.size()) {
        Array<cook::String> rec;
        while (true) {
            cook::String f;
            if (i < s.size() && s[i] == '"') {
                ++i;
                bool closed = false;
                while (i < s.size()) {
                    if (s[i] == '"') {
                        if (i + 1 < s.size() && s[i + 1] == '"') {
                            f += '"';
                            i += 2;
                            continue;
                        }
                        ++i;
                        closed = true;
                        break;
                    }
                    if (s[i] != '\r') f += s[i];
                    ++i;
                }
                if (!closed) {
                    err += "a quoted field is never closed";
                    return false;
                }
            } else {
                while (i < s.size() && s[i] != ',' && s[i] != '\n') {
                    if (s[i] != '\r') f += s[i];
                    ++i;
                }
            }
            rec.push_back(static_cast<cook::String&&>(f));
            if (i < s.size() && s[i] == ',') {
                ++i;
                continue;
            }
            break;
        }
        if (i < s.size() && s[i] == '\r') ++i;
        if (i < s.size() && s[i] == '\n') ++i;
        if (!(rec.size() == 1 && rec[0].empty())) out.push_back(static_cast<Array<cook::String>&&>(rec));
    }
    return true;
}

void put_field(cook::String& out, StringView f) {
    bool quote = !f.empty() && (f.front() == ' ' || f.back() == ' ');
    for (const char c : f) quote |= c == ',' || c == '"' || c == '\n' || c == '\r';
    if (!quote) {
        out += f;
        return;
    }
    out += '"';
    for (const char c : f) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
}

StringView trimmed(StringView s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// The key a record defines, or empty for the header, a comment row and a blank key.
StringView record_key(const Array<cook::String>& rec) {
    if (rec.empty()) return {};
    const StringView k = trimmed(rec[0].view());
    return !k.empty() && k.front() == '#' ? StringView() : k;
}

StringView stem_of(StringView path) {
    if (const usize slash = path.rfind('/'); slash != StringView::npos) path = path.substr(slash + 1);
    if (const usize dot = path.rfind('.'); dot != StringView::npos) path = path.substr(0, dot);
    return path;
}

}  // namespace

bool StringsGrid::load(const EditorProject& project, cook::String& err) {
    locales_.clear();
    tables_.clear();
    keys_.clear();
    rows_.clear();
    Array<const ProjectAsset*> found;
    for (const ProjectAsset& a : project.assets())
        if (a.type == asset::ChunkType::StringTable) found.push_back(&a);
    // The base locale first, the rest by path.
    for (u32 i = 0; i < found.size(); ++i)
        for (u32 j = i + 1; j < found.size(); ++j) {
            const bool j_base = stem_of(found[j]->path.view()) == "en";
            const bool i_base = stem_of(found[i]->path.view()) == "en";
            if ((j_base && !i_base) || (!(i_base && !j_base) && found[j]->path.view() < found[i]->path.view())) {
                const ProjectAsset* t = found[i];
                found[i] = found[j];
                found[j] = t;
            }
        }
    for (const ProjectAsset* a : found) {
        cook::String text;
        if (!cook::fs::read_text(project.asset_path(a->path.view()).view(), text)) {
            err += "could not read ";
            err += a->path.view();
            return false;
        }
        Table t;
        cook::String why;
        if (!parse_records(text.view(), t.records, why)) {
            err += a->path.view();
            err += ": ";
            err += why.view();
            return false;
        }
        if (t.records.empty() || t.records[0].size() < 2) {
            err += a->path.view();
            err += ": the first row must be the header \"key,text\"";
            return false;
        }
        Locale l;
        l.path = a->path;
        l.tag = cook::String(stem_of(a->path.view()));
        l.guid = a->guid;
        locales_.push_back(static_cast<Locale&&>(l));
        tables_.push_back(static_cast<Table&&>(t));
    }
    // The union of keys, first-seen order, the base locale's file order first.
    for (const Table& t : tables_)
        for (u32 r = 1; r < t.records.size(); ++r) {
            const StringView k = record_key(t.records[r]);
            if (k.empty()) continue;
            bool known = false;
            for (const cook::String& e : keys_) known |= e.view() == k;
            if (!known) keys_.push_back(cook::String(k));
        }
    rebuild_index();
    return true;
}

void StringsGrid::rebuild_index() {
    rows_.clear();
    for (const Table& t : tables_) {
        Array<i32> row;
        for (u32 k = 0; k < keys_.size(); ++k) row.push_back(-1);
        for (u32 r = 1; r < t.records.size(); ++r) {
            const StringView key = record_key(t.records[r]);
            if (key.empty()) continue;
            for (u32 k = 0; k < keys_.size(); ++k)
                if (keys_[k].view() == key) {
                    row[k] = static_cast<i32>(r);
                    break;
                }
        }
        rows_.push_back(static_cast<Array<i32>&&>(row));
    }
}

StringView StringsGrid::text(u32 locale, u32 key) const {
    if (!has(locale, key)) return {};
    const Record& rec = tables_[locale].records[static_cast<u32>(rows_[locale][key])];
    return rec.size() > 1 ? rec[1].view() : StringView();
}

void StringsGrid::set_text(u32 locale, u32 key, StringView text) {
    if (locale >= tables_.size() || key >= keys_.size()) return;
    Table& t = tables_[locale];
    if (rows_[locale][key] < 0) {
        Record rec;
        const u32 width = static_cast<u32>(t.records[0].size());
        for (u32 i = 0; i < width; ++i) rec.push_back(cook::String());
        rec[0] = keys_[key];
        t.records.push_back(static_cast<Record&&>(rec));
        rows_[locale][key] = static_cast<i32>(t.records.size()) - 1;
    }
    Record& rec = t.records[static_cast<u32>(rows_[locale][key])];
    if (rec[1].view() == text) return;
    rec[1] = cook::String(text);
    locales_[locale].dirty = true;
}

bool StringsGrid::add_key(StringView key) {
    key = trimmed(key);
    if (key.empty() || tables_.empty()) return false;
    for (const cook::String& e : keys_)
        if (e.view() == key) return false;
    keys_.push_back(cook::String(key));
    for (Array<i32>& row : rows_) row.push_back(-1);
    set_text(0, static_cast<u32>(keys_.size()) - 1, "");
    locales_[0].dirty = true;  // set_text of an empty text on a new row still adds it
    return true;
}

void StringsGrid::remove_key(u32 key) {
    if (key >= keys_.size()) return;
    for (u32 l = 0; l < tables_.size(); ++l) {
        const i32 row = rows_[l][key];
        if (row < 0) continue;
        tables_[l].records.erase(static_cast<u32>(row));
        locales_[l].dirty = true;
    }
    keys_.erase(key);
    rebuild_index();
}

u32 StringsGrid::missing(u32 locale) const {
    if (locale >= rows_.size()) return 0;
    u32 n = 0;
    for (u32 k = 0; k < keys_.size(); ++k) n += rows_[locale][k] < 0 ? 1u : 0u;
    return n;
}

bool StringsGrid::dirty() const {
    for (const Locale& l : locales_)
        if (l.dirty) return true;
    return false;
}

bool StringsGrid::save(EditorProject& project, cook::String& err) {
    for (u32 l = 0; l < locales_.size(); ++l) {
        if (!locales_[l].dirty) continue;
        cook::String text;
        for (const Record& rec : tables_[l].records) {
            for (u32 i = 0; i < rec.size(); ++i) {
                if (i) text += ',';
                put_field(text, rec[i].view());
            }
            text += '\n';
        }
        const ProjectAsset* a = project.find(locales_[l].guid);
        if (!a) {
            err += "the table is no longer in the catalog: ";
            err += locales_[l].path.view();
            return false;
        }
        const ProjectAsset asset = *a;  // validate_and_save_source rescans the catalog
        cook::String why;
        if (!validate_and_save_source(project, asset, text.view(), why)) {
            err += locales_[l].path.view();
            err += ": ";
            err += why.view();
            return false;
        }
        locales_[l].dirty = false;
    }
    return true;
}

}  // namespace mce::editor
