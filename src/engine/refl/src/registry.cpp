#include <cstring>

#include "mce/assert.h"
#include "mce/refl/type.h"

namespace mce::refl {
namespace {

// Fixed-capacity, name-sorted pointer tables. constinit so registrars running in any
// static-init order see a valid (empty) registry; no heap.
constexpr u32 kMaxTypes = 1024;
constexpr u32 kMaxEnums = 256;

constinit const TypeRecord* g_types[kMaxTypes] = {};
constinit u32 g_type_count = 0;
constinit const EnumRecord* g_enums[kMaxEnums] = {};
constinit u32 g_enum_count = 0;

// Inserts rec into the name-sorted table, or returns false for an equivalent duplicate.
template <class Rec, class SameFn>
bool insert_sorted(const Rec** table, u32& count, u32 capacity, const Rec* rec, SameFn same) {
    MCE_ASSERT(rec != nullptr && rec->id.valid());
    for (u32 i = 0; i < count; ++i) {
        const Rec* other = table[i];
        if (other == rec) return false;
        if (other->id == rec->id) {
            MCE_ASSERT_MSG(std::strcmp(other->name, rec->name) == 0, "reflection StringId collision");
            MCE_ASSERT_MSG(same(*other, *rec), "type re-registered with a different layout");
            return false;
        }
    }
    MCE_ASSERT_MSG(count < capacity, "reflection registry full");
    if (count >= capacity) return false;
    u32 pos = count;
    while (pos > 0 && std::strcmp(table[pos - 1]->name, rec->name) > 0) {
        table[pos] = table[pos - 1];
        --pos;
    }
    table[pos] = rec;
    ++count;
    return true;
}

template <class Rec>
void erase(const Rec** table, u32& count, const Rec* rec) {
    for (u32 i = 0; i < count; ++i) {
        if (table[i] == rec) {
            for (u32 j = i + 1; j < count; ++j) table[j - 1] = table[j];
            table[--count] = nullptr;
            return;
        }
    }
}

template <class Rec>
const Rec* find_in(const Rec* const* table, u32 count, StringId id) {
    for (u32 i = 0; i < count; ++i) {
        if (table[i]->id == id) return table[i];
    }
    return nullptr;
}

}  // namespace

const char* field_kind_name(FieldKind kind) {
    switch (kind) {
        case FieldKind::Bool: return "bool";
        case FieldKind::I8: return "i8";
        case FieldKind::I16: return "i16";
        case FieldKind::I32: return "i32";
        case FieldKind::I64: return "i64";
        case FieldKind::U8: return "u8";
        case FieldKind::U16: return "u16";
        case FieldKind::U32: return "u32";
        case FieldKind::U64: return "u64";
        case FieldKind::F32: return "f32";
        case FieldKind::F64: return "f64";
        case FieldKind::String: return "string";
        case FieldKind::Enum: return "enum";
        case FieldKind::Vec2: return "vec2";
        case FieldKind::Vec3: return "vec3";
        case FieldKind::Vec4: return "vec4";
        case FieldKind::Quat: return "quat";
        case FieldKind::Color: return "color";
        case FieldKind::AssetRef: return "asset_ref";
        case FieldKind::EntityRef: return "entity_ref";
        case FieldKind::Struct: return "struct";
    }
    return "?";
}

const FieldRecord* TypeRecord::find_field(StringId field_name) const {
    for (const FieldRecord& f : fields) {
        if (f.name == field_name) return &f;
    }
    return nullptr;
}

const EnumValue* EnumRecord::find(StringId value_name) const {
    for (const EnumValue& v : values) {
        if (v.name == value_name) return &v;
    }
    return nullptr;
}

const EnumValue* EnumRecord::find(i64 value) const {
    for (const EnumValue& v : values) {
        if (v.value == value) return &v;
    }
    return nullptr;
}

const TypeRecord* find_type(StringId id) { return find_in(g_types, g_type_count, id); }
Span<const TypeRecord* const> all() { return Span<const TypeRecord* const>(g_types, g_type_count); }

const EnumRecord* find_enum(StringId id) { return find_in(g_enums, g_enum_count, id); }
Span<const EnumRecord* const> all_enums() { return Span<const EnumRecord* const>(g_enums, g_enum_count); }

bool register_type(const TypeRecord* type) {
    return insert_sorted(g_types, g_type_count, kMaxTypes, type, [](const TypeRecord& a, const TypeRecord& b) {
        return a.size == b.size && a.align == b.align && a.fields.size() == b.fields.size();
    });
}

void unregister_type(const TypeRecord* type) { erase(g_types, g_type_count, type); }

bool register_enum(const EnumRecord* e) {
    return insert_sorted(g_enums, g_enum_count, kMaxEnums, e, [](const EnumRecord& a, const EnumRecord& b) {
        return a.size == b.size && a.values.size() == b.values.size();
    });
}

void unregister_enum(const EnumRecord* e) { erase(g_enums, g_enum_count, e); }

}  // namespace mce::refl
