#include "mce/input/bindings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mce/fixed_string.h"

namespace mce::input {

namespace {

constexpr const char* kHeader = "# mce input bindings v1";

void append(Array<char>& out, StringView s) {
    for (char c : s) out.push_back(c);
}

// One reflected scalar as text. Kinds a binding never uses are not written.
bool format_field(const refl::FieldRecord& f, const void* obj, char* buf, usize cap) {
    const byte* p = static_cast<const byte*>(obj) + f.offset;
    auto load = [&](auto v) {
        std::memcpy(&v, p, sizeof(v));
        return v;
    };
    using refl::FieldKind;
    switch (f.kind) {
        case FieldKind::Bool: std::snprintf(buf, cap, "%s", load(bool{}) ? "true" : "false"); return true;
        case FieldKind::U8: std::snprintf(buf, cap, "%u", static_cast<unsigned>(load(u8{}))); return true;
        case FieldKind::U16: std::snprintf(buf, cap, "%u", static_cast<unsigned>(load(u16{}))); return true;
        case FieldKind::U32: std::snprintf(buf, cap, "%lu", static_cast<unsigned long>(load(u32{}))); return true;
        case FieldKind::U64:
            std::snprintf(buf, cap, "0x%016llx", static_cast<unsigned long long>(load(u64{})));
            return true;
        case FieldKind::I32: std::snprintf(buf, cap, "%ld", static_cast<long>(load(i32{}))); return true;
        case FieldKind::F32: std::snprintf(buf, cap, "%.9g", static_cast<double>(load(f32{}))); return true;
        case FieldKind::Enum: {
            i64 v = 0;
            if (f.size == 1) v = load(u8{});
            else if (f.size == 2) v = load(u16{});
            else v = load(u32{});
            const refl::EnumRecord* e = refl::find_enum(f.type_id);
            const refl::EnumValue* ev = e ? e->find(v) : nullptr;
            if (ev) std::snprintf(buf, cap, "%s", ev->name_str);
            else std::snprintf(buf, cap, "%lld", static_cast<long long>(v));
            return true;
        }
        default: return false;
    }
}

bool parse_field(const refl::FieldRecord& f, void* obj, StringView text) {
    byte* p = static_cast<byte*>(obj) + f.offset;
    FixedString<64> tmp(text);
    const char* s = tmp.c_str();
    char* end = nullptr;
    auto store = [&](auto v) { std::memcpy(p, &v, sizeof(v)); };
    using refl::FieldKind;
    switch (f.kind) {
        case FieldKind::Bool:
            if (text == "true") store(true);
            else if (text == "false") store(false);
            else return false;
            return true;
        case FieldKind::U8:
        case FieldKind::U16:
        case FieldKind::U32:
        case FieldKind::U64: {
            const unsigned long long v = std::strtoull(s, &end, 0);
            if (end == s || *end) return false;
            if (f.kind == FieldKind::U8) store(static_cast<u8>(v));
            else if (f.kind == FieldKind::U16) store(static_cast<u16>(v));
            else if (f.kind == FieldKind::U32) store(static_cast<u32>(v));
            else store(static_cast<u64>(v));
            return true;
        }
        case FieldKind::I32: {
            const long v = std::strtol(s, &end, 0);
            if (end == s || *end) return false;
            store(static_cast<i32>(v));
            return true;
        }
        case FieldKind::F32: {
            const f32 v = std::strtof(s, &end);
            if (end == s || *end) return false;
            store(v);
            return true;
        }
        case FieldKind::Enum: {
            const refl::EnumRecord* e = refl::find_enum(f.type_id);
            const refl::EnumValue* ev = e ? e->find(StringId(text)) : nullptr;
            if (!ev) return false;
            if (f.size == 1) store(static_cast<u8>(ev->value));
            else if (f.size == 2) store(static_cast<u16>(ev->value));
            else store(static_cast<u32>(ev->value));
            return true;
        }
        default: return false;
    }
}

StringView trim(StringView s) {
    usize b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

}  // namespace

BindingDef BindingOverride::binding() const {
    BindingDef b;
    b.device = device;
    b.axis = axis;
    b.curve = curve;
    b.flags = flags;
    b.code = code;
    b.scale = scale;
    b.dz = {dz_inner, dz_outer};
    return b;
}

BindingOverride BindingOverride::make(StringId map, StringId action, u32 index, const BindingDef& b) {
    BindingOverride o;
    o.map = map.value;
    o.action = action.value;
    o.index = index;
    o.device = b.device;
    o.axis = b.axis;
    o.curve = b.curve;
    o.flags = b.flags;
    o.code = b.code;
    o.scale = b.scale;
    o.dz_inner = b.dz.inner;
    o.dz_outer = b.dz.outer;
    return o;
}

void write_bindings(Span<const BindingOverride> overrides, Array<char>& out) {
    const refl::TypeRecord& t = mce_refl_type_record(static_cast<BindingOverride*>(nullptr));
    append(out, kHeader);
    out.push_back('\n');
    char buf[64];
    for (const BindingOverride& o : overrides) {
        out.push_back('[');
        append(out, t.name);
        append(out, "]\n");
        for (const refl::FieldRecord& f : t.fields) {
            if (f.is_array() || !format_field(f, &o, buf, sizeof(buf))) continue;
            append(out, f.name_str);
            append(out, " = ");
            append(out, buf);
            out.push_back('\n');
        }
    }
}

Result<void> read_bindings(StringView text, Array<BindingOverride>& out) {
    constexpr const char* kStage = "input.bindings.read";
    const refl::TypeRecord& t = mce_refl_type_record(static_cast<BindingOverride*>(nullptr));
    out.clear();
    bool header = false, in_record = false;
    u32 line_no = 0;
    usize pos = 0;
    while (pos < text.size()) {
        usize nl = pos;
        while (nl < text.size() && text[nl] != '\n') ++nl;
        const StringView line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        ++line_no;
        if (!header) {
            if (line != kHeader) return Error(Errc::BadMagic, kStage, "not an input bindings file");
            header = true;
            continue;
        }
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            if (line.size() < 2 || line[line.size() - 1] != ']' || line.substr(1, line.size() - 2) != t.name)
                return Error(Errc::InvalidArgument, kStage, "unknown record", line_no);
            BindingOverride o;
            out.push_back(o);
            in_record = true;
            continue;
        }
        if (!in_record) return Error(Errc::InvalidArgument, kStage, "field outside a record", line_no);
        usize eq = 0;
        while (eq < line.size() && line[eq] != '=') ++eq;
        if (eq == line.size()) return Error(Errc::InvalidArgument, kStage, "expected `field = value`", line_no);
        const refl::FieldRecord* f = t.find_field(StringId(trim(line.substr(0, eq))));
        if (!f) continue;  // a field a newer build wrote: ignore
        if (!parse_field(*f, &out.back(), trim(line.substr(eq + 1))))
            return Error(Errc::InvalidArgument, kStage, "bad value", line_no);
    }
    if (!header) return Error(Errc::BadMagic, kStage, "empty bindings file");
    return ok();
}

}  // namespace mce::input
