#include "mce/loc/string_table.h"

#include <cstring>

namespace mce::loc {

namespace {

constexpr const char* kStage = "loc.string_table";

struct StringTableHeader {
    char locale[16];  // NUL-padded
    u32 entry_count;
    u32 blob_size;
    u32 reserved[2];
};
static_assert(sizeof(StringTableHeader) == 32);

struct StringTableEntry {
    u32 key_offset;
    u32 key_size;
    u32 text_offset;
    u32 text_size;
    u8 form;  // PluralForm
    u8 reserved[3];
};
static_assert(sizeof(StringTableEntry) == 20);

constexpr const char* kFormNames[] = {"zero", "one", "two", "few", "many", "other"};

void append(Array<byte>& out, const void* p, usize n) {
    const usize at = out.size();
    out.resize_uninitialized(at + n);
    std::memcpy(out.data() + at, p, n);
}

// (key, form) order: bytewise key, then form.
int compare(StringView ak, PluralForm af, StringView bk, PluralForm bf) {
    if (const int c = ak.compare(bk); c != 0) return c;
    return static_cast<int>(af) - static_cast<int>(bf);
}

}  // namespace

const char* plural_form_name(PluralForm f) {
    const auto i = static_cast<usize>(f);
    return i < 6 ? kFormNames[i] : "?";
}

bool parse_plural_form(StringView s, PluralForm& out) {
    for (u8 i = 0; i < 6; ++i)
        if (s == kFormNames[i]) {
            out = static_cast<PluralForm>(i);
            return true;
        }
    return false;
}

bool valid_key(StringView key) {
    if (key.empty()) return false;
    for (const char c : key) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                        c == '.';
        if (!ok) return false;
    }
    return true;
}

bool valid_utf8(StringView s) {
    const auto* p = reinterpret_cast<const u8*>(s.data());
    const usize n = s.size();
    usize i = 0;
    while (i < n) {
        const u8 c = p[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        u32 len = 0;
        u32 cp = 0;
        if ((c & 0xE0) == 0xC0) len = 2, cp = c & 0x1F;
        else if ((c & 0xF0) == 0xE0) len = 3, cp = c & 0x0F;
        else if ((c & 0xF8) == 0xF0) len = 4, cp = c & 0x07;
        else return false;
        if (n - i < len) return false;
        for (u32 k = 1; k < len; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        const u32 min = len == 2 ? 0x80 : len == 3 ? 0x800 : 0x10000;
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

usize StringTable::lower_bound(StringView key, PluralForm form) const {
    usize lo = 0, hi = entries_.size();
    while (lo < hi) {
        const usize mid = lo + (hi - lo) / 2;
        if (compare(key_of(entries_[mid]), entries_[mid].form, key, form) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

bool StringTable::add(StringView key, StringView text, PluralForm form) {
    if (!valid_key(key) || !valid_utf8(text) || static_cast<u8>(form) > static_cast<u8>(PluralForm::Other)) return false;
    if (key.size() > 0xFFFF || text.size() > 0xFFFFFF || blob_.size() + key.size() + text.size() > 0xFFFFFFFFull) return false;
    const usize at = lower_bound(key, form);
    if (at < entries_.size() && compare(key_of(entries_[at]), entries_[at].form, key, form) == 0) return false;
    Slot s{};
    s.key_off = static_cast<u32>(blob_.size());
    s.key_size = static_cast<u32>(key.size());
    for (const char c : key) blob_.push_back(c);
    s.text_off = static_cast<u32>(blob_.size());
    s.text_size = static_cast<u32>(text.size());
    for (const char c : text) blob_.push_back(c);
    s.form = form;
    entries_.insert(at, s);
    return true;
}

bool StringTable::find(StringView key, PluralForm form, StringView& out) const {
    const usize at = lower_bound(key, form);
    if (at >= entries_.size()) return false;
    const Slot& s = entries_[at];
    if (s.form != form || key_of(s) != key) return false;
    out = StringView(blob_.data() + s.text_off, s.text_size);
    return true;
}

bool StringTable::has_key(StringView key) const {
    const usize at = lower_bound(key, PluralForm::Zero);
    return at < entries_.size() && key_of(entries_[at]) == key;
}

StringTable::Entry StringTable::entry(usize i) const {
    const Slot& s = entries_[i];
    return Entry{key_of(s), StringView(blob_.data() + s.text_off, s.text_size), s.form};
}

void encode_table(const StringTable& t, Array<byte>& out) {
    StringTableHeader h{};
    const StringView tag = t.locale();
    std::memcpy(h.locale, tag.data(), tag.size() < sizeof(h.locale) ? tag.size() : sizeof(h.locale) - 1);
    h.entry_count = static_cast<u32>(t.size());
    Array<char> blob;
    Array<StringTableEntry> entries;
    for (usize i = 0; i < t.size(); ++i) {
        const StringTable::Entry e = t.entry(i);
        StringTableEntry r{};
        r.key_offset = static_cast<u32>(blob.size());
        r.key_size = static_cast<u32>(e.key.size());
        for (const char c : e.key) blob.push_back(c);
        r.text_offset = static_cast<u32>(blob.size());
        r.text_size = static_cast<u32>(e.text.size());
        for (const char c : e.text) blob.push_back(c);
        r.form = static_cast<u8>(e.form);
        entries.push_back(r);
    }
    h.blob_size = static_cast<u32>(blob.size());
    out.clear();
    append(out, &h, sizeof(h));
    if (!entries.empty()) append(out, entries.data(), entries.size() * sizeof(StringTableEntry));
    if (!blob.empty()) append(out, blob.data(), blob.size());
}

Result<void> decode_table(Span<const byte> payload, StringTable& out) {
    out.clear();
    out.set_locale({});
    if (payload.size() < sizeof(StringTableHeader)) return Error(Errc::Truncated, kStage);
    StringTableHeader h;
    std::memcpy(&h, payload.data(), sizeof(h));
    if (h.reserved[0] != 0 || h.reserved[1] != 0) return Error(Errc::ReservedNonZero, kStage);
    usize tag_len = 0;
    while (tag_len < sizeof(h.locale) && h.locale[tag_len] != '\0') ++tag_len;
    LocaleTag tag(StringView(h.locale, tag_len));
    if (tag_len == sizeof(h.locale) || tag.empty()) return Error(Errc::InvalidArgument, kStage);  // no locale tag
    const u64 need = sizeof(h) + u64(h.entry_count) * sizeof(StringTableEntry) + h.blob_size;
    if (payload.size() < need) return Error(Errc::Truncated, kStage, {}, static_cast<u32>(payload.size()));
    const byte* entries = payload.data() + sizeof(h);
    const char* blob = reinterpret_cast<const char*>(entries + usize(h.entry_count) * sizeof(StringTableEntry));
    StringView prev_key;
    PluralForm prev_form = PluralForm::Zero;
    for (u32 i = 0; i < h.entry_count; ++i) {
        StringTableEntry e;
        std::memcpy(&e, entries + usize(i) * sizeof(StringTableEntry), sizeof(e));
        if (e.reserved[0] != 0 || e.reserved[1] != 0 || e.reserved[2] != 0) return Error(Errc::ReservedNonZero, kStage, {}, i);
        if (e.key_offset > h.blob_size || e.key_size > h.blob_size - e.key_offset || e.text_offset > h.blob_size ||
            e.text_size > h.blob_size - e.text_offset)
            return Error(Errc::OutOfBounds, kStage, {}, i);
        if (e.form > static_cast<u8>(PluralForm::Other)) return Error(Errc::InvalidArgument, kStage, {}, i);
        const StringView key(blob + e.key_offset, e.key_size);
        const StringView text(blob + e.text_offset, e.text_size);
        const auto form = static_cast<PluralForm>(e.form);
        if (i > 0 && compare(prev_key, prev_form, key, form) >= 0) {
            out.clear();
            return Error(Errc::InvalidArgument, kStage, key, i);  // unsorted or duplicate
        }
        if (!out.add(key, text, form)) {
            out.clear();
            return Error(Errc::InvalidArgument, kStage, key, i);  // bad key or text
        }
        prev_key = key;
        prev_form = form;
    }
    out.set_locale(tag.view());
    return ok();
}

}  // namespace mce::loc
