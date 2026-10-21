// .csv -> CHNK_STR: one locale's string table (M-MOBILE step 11a, issue #146). The file lives at
// assets/loc/<locale>.csv (strings_csv.h has the format); its stem is the locale unless the sidecar
// says otherwise:
//
//   { "guid": "<uuid>", "importer": "strings", "settings": { "locale": "pt-BR" } }
//
// The chunk is the one loc::decode_table reads, written by encode_table and decoded again before
// it is stored, so a table the runtime would reject never cooks. Keys a locale lacks against the
// project's base table are reported by the cook as a whole (cook.cpp), not here.

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/loc/loc.h"
#include "strings_csv.h"

namespace mce::cook {

namespace detail {

namespace {

constexpr const char* kCsvStage = "cook.strings";

struct Field {
    String text;
    u32 line = 0;
};

// Splits the CSV into records of fields. False (with an error) on an unterminated quote or text
// after a closing quote.
bool split_records(StringView s, StringView file, Array<Array<Field>>& records, DiagList& diags) {
    if (s.size() >= 3 && static_cast<u8>(s[0]) == 0xEF && static_cast<u8>(s[1]) == 0xBB && static_cast<u8>(s[2]) == 0xBF)
        s = s.substr(3);
    u32 line = 1;
    usize i = 0;
    while (i < s.size()) {
        Array<Field> rec;
        while (true) {
            Field f;
            f.line = line;
            if (i < s.size() && s[i] == '"') {
                const u32 open_line = line;
                ++i;
                bool closed = false;
                while (i < s.size()) {
                    const char c = s[i];
                    if (c == '"') {
                        if (i + 1 < s.size() && s[i + 1] == '"') {
                            f.text += '"';
                            i += 2;
                            continue;
                        }
                        ++i;
                        closed = true;
                        break;
                    }
                    if (c == '\n') ++line;
                    if (c != '\r') f.text += c;
                    ++i;
                }
                if (!closed) {
                    diags.error(Errc::ParseError, file, open_line, 0, kCsvStage, "a quoted field is never closed");
                    return false;
                }
                if (i < s.size() && s[i] != ',' && s[i] != '\n' && s[i] != '\r') {
                    diags.error(Errc::ParseError, file, line, 0, kCsvStage, "text after a closing quote (write \"\" for a quote inside quotes)");
                    return false;
                }
            } else {
                while (i < s.size() && s[i] != ',' && s[i] != '\n') {
                    if (s[i] != '\r') f.text += s[i];
                    ++i;
                }
            }
            rec.push_back(std::move(f));
            if (i < s.size() && s[i] == ',') {
                ++i;
                continue;
            }
            break;
        }
        if (i < s.size() && s[i] == '\r') ++i;
        if (i < s.size() && s[i] == '\n') {
            ++i;
            ++line;
        }
        const bool blank = rec.size() == 1 && rec[0].text.empty();
        if (!blank) records.push_back(std::move(rec));
    }
    return true;
}

StringView trim(StringView s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

}  // namespace

bool parse_string_csv(StringView text, StringView file, loc::StringTable& out, DiagList& diags) {
    const u32 errors_before = diags.error_count();
    Array<Array<Field>> records;
    if (!split_records(text, file, records, diags)) return false;
    if (records.empty() || records[0].size() < 2 || trim(records[0][0].text.view()) != "key" ||
        trim(records[0][1].text.view()) != "text") {
        diags.error(Errc::ParseError, file, 1, 0, kCsvStage, "the first row must be the header \"key,text\"");
        return false;
    }
    for (usize r = 1; r < records.size(); ++r) {
        const Array<Field>& rec = records[r];
        const StringView raw_key = trim(rec[0].text.view());
        if (!raw_key.empty() && raw_key.front() == '#') continue;  // a comment row
        const u32 line = rec[0].line;
        if (rec.size() < 2) {
            diags.error(Errc::ParseError, file, line, 0, kCsvStage, "a row needs a key and a text");
            continue;
        }
        StringView key = raw_key;
        loc::PluralForm form = loc::PluralForm::Other;
        if (const usize hash = key.find('#'); hash != StringView::npos) {
            if (!loc::parse_plural_form(key.substr(hash + 1), form)) {
                diags.error(Errc::ParseError, file, line, 0, kCsvStage, "a plural form is one of #zero, #one, #two, #few, #many, #other");
                continue;
            }
            key = key.substr(0, hash);
        }
        if (!loc::valid_key(key)) {
            diags.error(Errc::ParseError, file, line, 0, kCsvStage, "a key may only use letters, digits, '_', '-' and '.'");
            continue;
        }
        const StringView value = rec[1].text.view();
        if (!loc::valid_utf8(value)) {
            diags.error(Errc::ParseError, file, rec[1].line, 0, kCsvStage, "the text is not valid UTF-8");
            continue;
        }
        if (!out.add(key, value, form)) {
            String msg;
            appendf(msg, "'%.*s' appears twice", static_cast<int>(raw_key.size()), raw_key.data());
            diags.error(Errc::ParseError, file, line, 0, kCsvStage, msg);
        }
    }
    // A key with plural forms needs "other", the form every locale falls back to.
    for (usize i = 0; i < out.size(); ++i) {
        const loc::StringTable::Entry e = out.entry(i);
        StringView unused;
        if (e.form != loc::PluralForm::Other && !out.find(e.key, loc::PluralForm::Other, unused)) {
            String msg;
            appendf(msg, "'%.*s' has plural forms but no '%.*s#other'", static_cast<int>(e.key.size()), e.key.data(),
                    static_cast<int>(e.key.size()), e.key.data());
            diags.error(Errc::ParseError, file, 0, 0, kCsvStage, msg);
            while (i + 1 < out.size() && out.entry(i + 1).key == e.key) ++i;
        }
    }
    return diags.error_count() == errors_before;
}

bool locale_from_path(StringView source_path, loc::LocaleTag& out) {
    StringView stem = source_path;
    if (const usize slash = stem.rfind('/'); slash != StringView::npos) stem = stem.substr(slash + 1);
    if (const usize dot = stem.rfind('.'); dot != StringView::npos) stem = stem.substr(0, dot);
    return loc::normalize_locale(stem, out) && out.size() == stem.size();
}

}  // namespace detail

namespace {

constexpr const char* kStage = "cook.strings";

class StringImporter final : public IImporter {
public:
    StringView id() const override { return "strings"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::StringTable; }
    bool native() const override { return false; }
    bool handles_extension(StringView ext) const override { return ext == ".csv"; }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue s = in.settings;
        const StringView file = in.display_path;
        if (!detail::check_keys(s, {"locale"}, file, kStage, diags)) return false;

        loc::LocaleTag tag;
        const JsonValue want = s.valid() ? s.get("locale") : JsonValue();
        if (want.valid()) {
            if (!want.is_string() || !loc::normalize_locale(want.as_string(), tag)) {
                detail::error_at(diags, Errc::ParseError, file, want, kStage, "\"locale\" must be a language tag such as \"tr\" or \"pt-BR\"");
                return false;
            }
        } else if (!detail::locale_from_path(in.source_path, tag)) {
            diags.error(Errc::ParseError, file, 0, 0, kStage,
                        "name a string table after its locale (assets/loc/tr.csv, assets/loc/pt-BR.csv) or set \"locale\" in its sidecar");
            return false;
        }

        loc::StringTable table;
        table.set_locale(tag.view());
        const StringView text(reinterpret_cast<const char*>(in.source.data()), in.source.size());
        if (!detail::parse_string_csv(text, file, table, diags)) return false;

        Array<byte> bytes;
        loc::encode_table(table, bytes);
        loc::StringTable check;
        if (Result<void> r = loc::decode_table(bytes.span(), check); !r) {
            FixedString<512> why;
            format_error(r.error(), why);
            String msg("the string table is not loadable: ");
            msg += why.view();
            diags.error(Errc::ImportFailed, file, 0, 0, kStage, msg);
            return false;
        }

        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::StringTable;
        d.version = asset::chunk_version(asset::STRING_TABLE_VERSION, 0);
        d.guid = in.guid;
        out.add(d, bytes.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_string_importer() { return mem::make_unique<StringImporter>(mem::heap()); }

}  // namespace mce::cook
