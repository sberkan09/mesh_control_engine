#pragma once
// mce_refl (L1) — the sole source of field metadata (core.md §2.8, ARCHITECTURE F4 / ADR-0009).
//
// A generic registry of reflected plain structs and enums. It knows nothing about entities,
// scenes or the inspector; consumers (ECS registration, scene cook, inspector, undo, saves,
// UI binding) read TypeRecord/FieldRecord and address fields with property paths.
//
// Records are built at compile time by MCE_COMPONENT / MCE_REFL_STRUCT / MCE_ENUM
// (mce/refl/macros.h) and linked into the registry by static registrar objects before main.
// The registry is main-thread only after static initialization.

#include "mce/string_id.h"

namespace mce::refl {

enum class FieldKind : u8 {
    Bool,
    I8, I16, I32, I64,
    U8, U16, U32, U64,
    F32, F64,
    String,     // FixedString<N>; read/write through FieldRecord::string_ops
    Enum,       // type_id names an EnumRecord (when the enum is MCE_ENUM-reflected)
    Vec2, Vec3, Vec4, Quat, Color,
    AssetRef,   // declared by mce_asset via a FieldTraits specialization
    EntityRef,  // declared by mce_ecs via a FieldTraits specialization
    Struct,     // type_id names a reflected TypeRecord (nested plain struct)
};

const char* field_kind_name(FieldKind kind);

namespace FieldFlags {
enum : u32 {
    None = 0,
    ReadOnly = 1u << 0,     // inspector shows it, never writes it (computed / derived data)
    Transient = 1u << 1,    // never serialized into scenes (rebuilt at load)
    Save = 1u << 2,         // included in save games (ecs-gameplay §2.13 [Save] marker)
    EditorRange = 1u << 3,  // editor_min/editor_max are meaningful (set by refl::Range)
    Replicated = 1u << 4,   // sent from the server to clients by mce_net's snapshots (M-NET2 step 7)
    WorldPoint = 1u << 5,   // a Vec3 holding a world-space point: moved by a floating-origin rebase (M-OPENWORLD A2)
};
}

// Accessors for String fields; the storage layout is the field type's business.
struct StringOps {
    StringView (*get)(const void* field);
    void (*set)(void* field, StringView value);  // truncates to the field's capacity
    u32 capacity;
};

struct FieldRecord {
    StringId name;
    const char* name_str = "";
    FieldKind kind = FieldKind::Bool;
    u32 offset = 0;               // byte offset inside the owning struct (offsetof)
    u32 size = 0;                 // byte size of one element
    u32 count = 1;                // count > 1: fixed array `T field[count]`
    StringId type_id;             // Struct / Enum / AssetRef / EntityRef: the referenced type
    const char* help = "";
    u32 flags = FieldFlags::None;
    f32 editor_min = 0.0f;        // drag limits; valid when flags & EditorRange
    f32 editor_max = 0.0f;
    const StringOps* string_ops = nullptr;  // String kind only

    constexpr bool is_array() const { return count > 1; }
    constexpr bool has(u32 flag) const { return (flags & flag) == flag; }
};

namespace TypeFlags {
enum : u32 {
    None = 0,
    Component = 1u << 0,  // declared with MCE_COMPONENT (ECS-registrable)
    Event = 1u << 1,      // declared with MCE_EVENT (an EventQueue<T> payload scripts read and push)
};
}

struct TypeRecord {
    StringId id;
    const char* name = "";
    u32 size = 0;
    u32 align = 0;
    u32 flags = TypeFlags::None;
    Span<const FieldRecord> fields;
    const TypeRecord* base = nullptr;  // reserved: flattened plain structs only, no inheritance
    void (*construct_default)(void* dst) = nullptr;  // placement-new T{} (default member values)
    // kTypeHash<T> of the C++ type: the ECS component identity, so code that only has the record
    // (scene loader, editor preview world) addresses the same component a typed World::set<T> does.
    u64 type_hash = 0;

    const FieldRecord* find_field(StringId field_name) const;
    constexpr bool is_component() const { return (flags & TypeFlags::Component) != 0; }
    constexpr bool is_event() const { return (flags & TypeFlags::Event) != 0; }
};

struct EnumValue {
    StringId name;
    const char* name_str = "";
    i64 value = 0;
};

struct EnumRecord {
    StringId id;
    const char* name = "";
    u32 size = 0;  // byte size of the underlying type
    bool is_signed = false;
    Span<const EnumValue> values;

    const EnumValue* find(StringId value_name) const;
    const EnumValue* find(i64 value) const;
};

// ---------------------------------------------------------------- registry
// Lookups are by StringId of the type's declared name. all() is sorted by name, so iteration
// order is deterministic regardless of static-init order across translation units.

const TypeRecord* find_type(StringId id);
Span<const TypeRecord* const> all();

const EnumRecord* find_enum(StringId id);
Span<const EnumRecord* const> all_enums();

// Registration (used by the registrar objects in macros.h; callable directly for tools/tests).
// Returns false when an equivalent record (same name and size) is already registered — the
// first one stays authoritative, which is what happens when a header-declared type is seen by
// more than one module. Two different names hashing to the same StringId assert.
bool register_type(const TypeRecord* type);
void unregister_type(const TypeRecord* type);
bool register_enum(const EnumRecord* e);
void unregister_enum(const EnumRecord* e);

}  // namespace mce::refl
