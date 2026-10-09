// .mcscene component writer (scene_text.h): the inverse of the scene importer's walker, emitting
// only the fields that differ from the C++ defaults.

#include <charconv>
#include <cmath>
#include <cstring>

#include "mce/asset/guid.h"
#include "mce/cook/scene_text.h"

namespace mce::cook {

namespace {

struct Writer {
    String& out;
    SceneEntityIndexFn entity_index;
    void* user;

    template <class T>
    static T load(const u8* p) {
        T v;
        std::memcpy(&v, p, sizeof(T));
        return v;
    }

    void element(const refl::FieldRecord& f, const u8* p, const u8* def) {
        using K = refl::FieldKind;
        switch (f.kind) {
            case K::Bool: out += load<u8>(p) ? "true" : "false"; return;
            case K::I8: appendf(out, "%d", static_cast<int>(load<i8>(p))); return;
            case K::I16: appendf(out, "%d", static_cast<int>(load<i16>(p))); return;
            case K::I32: appendf(out, "%d", load<i32>(p)); return;
            case K::I64: appendf(out, "%lld", static_cast<long long>(load<i64>(p))); return;
            case K::U8: appendf(out, "%u", static_cast<unsigned>(load<u8>(p))); return;
            case K::U16: appendf(out, "%u", static_cast<unsigned>(load<u16>(p))); return;
            case K::U32: appendf(out, "%u", load<u32>(p)); return;
            case K::U64: appendf(out, "%llu", static_cast<unsigned long long>(load<u64>(p))); return;
            case K::F32: append_json_f32(out, load<f32>(p)); return;
            case K::F64: append_json_number(out, std::isfinite(load<f64>(p)) ? load<f64>(p) : 0.0); return;
            case K::String: append_json_string(out, f.string_ops ? f.string_ops->get(p) : StringView()); return;
            case K::Enum: return enum_value(f, p);
            case K::Vec2:
            case K::Vec3:
            case K::Vec4:
            case K::Quat:
            case K::Color: {
                out += '[';
                for (u32 i = 0; i < f.size / 4; ++i) {
                    if (i) out += ", ";
                    append_json_f32(out, load<f32>(p + 4 * i));
                }
                out += ']';
                return;
            }
            case K::AssetRef: {
                asset::Guid g;
                std::memcpy(&g, p, sizeof(g));
                if (g == asset::Guid{}) {
                    out += "null";
                } else {
                    out += "\"guid:";
                    out += asset::to_string(g).view();
                    out += '"';
                }
                return;
            }
            case K::EntityRef: {
                const u32 index = entity_index ? entity_index(p, user) : kNoSceneEntity;
                if (index == kNoSceneEntity) out += "null";
                else appendf(out, "\"$%u\"", index);
                return;
            }
            case K::Struct: {
                const refl::TypeRecord* t = refl::find_type(f.type_id);
                if (t) object(*t, p, def);
                else out += "{}";
                return;
            }
        }
    }

    void enum_value(const refl::FieldRecord& f, const u8* p) {
        i64 v = 0;
        switch (f.size) {
            case 1: v = load<u8>(p); break;
            case 2: v = load<u16>(p); break;
            case 4: v = load<u32>(p); break;
            case 8: v = load<i64>(p); break;
            default: break;
        }
        const refl::EnumRecord* e = f.type_id.valid() ? refl::find_enum(f.type_id) : nullptr;
        if (e && e->is_signed && f.size < 8) {  // sign-extend narrow signed enums
            const u32 shift = 64 - 8 * f.size;
            v = static_cast<i64>(static_cast<u64>(v) << shift) >> shift;
        }
        const refl::EnumValue* ev = e ? e->find(v) : nullptr;
        if (ev) append_json_string(out, ev->name_str);
        else appendf(out, "%lld", static_cast<long long>(v));
    }

    // Is the field (all elements) byte-equal to the default? EntityRef fields compare by the index
    // they would serialize to (a dangling reference is the default: null).
    bool is_default(const refl::FieldRecord& f, const u8* p, const u8* def) const {
        if (f.kind == refl::FieldKind::EntityRef) {
            for (u32 i = 0; i < f.count; ++i)
                if (entity_index && entity_index(p + i * f.size, user) != kNoSceneEntity) return false;
            return true;
        }
        if (f.kind == refl::FieldKind::String && f.string_ops) {
            for (u32 i = 0; i < f.count; ++i)
                if (f.string_ops->get(p + i * f.size) != f.string_ops->get(def + i * f.size)) return false;
            return true;
        }
        if (f.kind == refl::FieldKind::Struct) {
            const refl::TypeRecord* t = refl::find_type(f.type_id);
            if (!t) return std::memcmp(p, def, usize(f.size) * f.count) == 0;
            for (u32 i = 0; i < f.count; ++i)
                for (const refl::FieldRecord& sub : t->fields)
                    if (!sub.has(refl::FieldFlags::Transient) &&
                        !is_default(sub, p + i * f.size + sub.offset, def + i * f.size + sub.offset))
                        return false;
            return true;
        }
        return std::memcmp(p, def, usize(f.size) * f.count) == 0;
    }

    // `mask` (top level only): exactly the fields whose bit is set (field index order), defaults
    // included; null: the non-default fields.
    void object(const refl::TypeRecord& type, const u8* p, const u8* def, const u64* mask = nullptr) {
        out += '{';
        bool first = true;
        for (usize fi = 0; fi < type.fields.size(); ++fi) {
            const refl::FieldRecord& f = type.fields[fi];
            if (f.has(refl::FieldFlags::Transient)) continue;
            if (mask ? fi >= 64 || ((*mask >> fi) & 1u) == 0 : is_default(f, p + f.offset, def + f.offset)) continue;
            out += first ? " " : ", ";
            first = false;
            append_json_string(out, f.name_str);
            out += ": ";
            if (f.count > 1) {
                out += '[';
                for (u32 i = 0; i < f.count; ++i) {
                    if (i) out += ", ";
                    element(f, p + f.offset + i * f.size, def + f.offset + i * f.size);
                }
                out += ']';
            } else {
                element(f, p + f.offset, def + f.offset);
            }
        }
        out += first ? "}" : " }";
    }
};

}  // namespace

void append_json_f32(String& out, f32 v) {
    if (!std::isfinite(v)) v = 0.0f;
    if (v == 0.0f) v = 0.0f;  // fold -0
    char buf[32];
    auto r = std::to_chars(buf, buf + sizeof(buf), v);  // shortest repr that reads back as this float
    out.append(StringView(buf, static_cast<usize>(r.ptr - buf)));
}

void write_scene_component(const refl::TypeRecord& type, const void* value, String& out, SceneEntityIndexFn entity_index,
                           void* user) {
    // Defaults come from the type's own constructor: a u64 buffer keeps any alignment <= 8.
    Array<u64> def;
    def.resize(type.size / 8 + 1, 0);
    if (type.construct_default) type.construct_default(def.data());
    Writer w{out, entity_index, user};
    w.object(type, static_cast<const u8*>(value), reinterpret_cast<const u8*>(def.data()));
}

void write_scene_component_fields(const refl::TypeRecord& type, const void* value, u64 field_mask, String& out,
                                  SceneEntityIndexFn entity_index, void* user) {
    Array<u64> def;
    def.resize(type.size / 8 + 1, 0);
    if (type.construct_default) type.construct_default(def.data());
    Writer w{out, entity_index, user};
    w.object(type, static_cast<const u8*>(value), reinterpret_cast<const u8*>(def.data()), &field_mask);
}

}  // namespace mce::cook
