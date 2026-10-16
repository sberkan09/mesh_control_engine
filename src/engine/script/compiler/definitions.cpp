// mce_script_compiler (M-SCRIPT 4): mce.d.luau, the `mce` API as Luau type definitions. The cook
// type-checks every script against it and writes it next to the cooked output for editors
// (luau-lsp). The function signatures are fixed; the component, event and field lists come from
// the reflection registry, sorted by name so the text is the same on every run.

#include <cstdio>
#include <cstring>

#include "mce/refl/type.h"
#include "mce/script/compiler.h"

namespace mce::script {

namespace {

void put(Array<char>& out, StringView s) {
    const usize at = out.size();
    out.resize(at + s.size());
    if (!s.empty()) std::memcpy(out.data() + at, s.data(), s.size());
}

// The fixed part: what api_world.cpp and script.cpp register.
constexpr const char* kApi = R"(
type Entity = number
-- A field of a component, from mce.field("Comp", "field"); resolve it once, at the top level.
declare extern type FieldHandle with end
type Fields = { [string]: any }
type RayHit = { entity: Entity, point: vector, normal: vector, distance: number }
type PathOptions = { cell: number?, radius: number?, diagonal: boolean?, smooth: boolean? }
type ShapeHit = { entity: Entity, point: vector, normal: vector, fraction: number }
type MoveResult2D = { position: vector, grounded: boolean, ground: Entity?, ceiling: boolean, wall_left: boolean, wall_right: boolean }

declare mce: {
    -- the fixed step in seconds
    dt: number,
    field: (component: string, field: string) -> FieldHandle,
    -- mce.get(e, handle, ...) or mce.get(e, "Comp", "field"): one value per field
    get: (e: Entity, ...any) -> ...any,
    -- mce.set(e, handle, value, ...) or mce.set(e, "Comp", "field", value)
    set: (e: Entity, ...any) -> (),
    -- mce.each("A", "B", ..., function(e) ... end)
    each: (...any) -> (),
    tick: () -> number,
    random: () -> number,
    log: (message: string) -> (),

    has: (e: Entity, component: string) -> boolean,
    alive: (e: Entity) -> boolean,
    spawn: (components: Fields) -> Entity,
    add: (e: Entity, component: string, fields: Fields?) -> (),
    remove: (e: Entity, component: string) -> (),
    destroy: (e: Entity) -> (),
    instantiate: (prefab: string, position: vector?) -> Entity?,

    events: (event: string) -> { Fields },
    emit: (event: string, fields: Fields?) -> (),
    signal: (name: string, e: Entity?) -> (),
    signals: (name: string) -> { Entity | boolean },
    play_sound: (sfx: string, gain_db: number?) -> (),

    pressed: (action: string) -> boolean,
    held: (action: string) -> boolean,
    released: (action: string) -> boolean,
    axis: (action: string) -> number,
    axis2: (action: string) -> vector,

    load_scene: (target: string) -> (),
    quit: () -> (),

    raycast2d: (origin: vector, dir: vector, max: number, skip: Entity?) -> RayHit?,
    overlap2d: (min: vector, max: vector, skip: Entity?) -> { Entity },
    move_and_collide: (e: Entity, delta: vector) -> MoveResult2D,
    teleport2d: (e: Entity, position: vector) -> (),
    find_path: (from: vector, to: vector, options: PathOptions?) -> { vector }?,
    velocity2d: (e: Entity) -> vector,
    set_velocity2d: (e: Entity, velocity: vector) -> (),
    impulse2d: (e: Entity, impulse: vector, point: vector?) -> (),
    force2d: (e: Entity, force: vector) -> (),
    angular_velocity2d: (e: Entity) -> number,
    set_angular_velocity2d: (e: Entity, w: number) -> (),
    awake2d: (e: Entity) -> boolean,
    set_awake2d: (e: Entity, awake: boolean) -> (),
    set_enabled2d: (e: Entity, enabled: boolean) -> (),
    overlap_point2d: (point: vector, skip: Entity?) -> boolean,
    raycast_all2d: (origin: vector, dir: vector, max: number, skip: Entity?) -> { RayHit },
    circle_cast2d: (from: vector, to: vector, radius: number, skip: Entity?) -> ShapeHit?,
    box_cast2d: (from: vector, to: vector, half: vector, skip: Entity?) -> ShapeHit?,
    raycast3d: (origin: vector, dir: vector, max: number, skip: Entity?) -> RayHit?,
}
)";

const char* luau_type(const refl::FieldRecord& f) {
    using K = refl::FieldKind;
    switch (f.kind) {
        case K::Bool: return "boolean";
        case K::String:
        case K::Enum:
        case K::AssetRef: return "string";
        case K::Vec2:
        case K::Vec3: return "vector";
        case K::Vec4:
        case K::Quat:
        case K::Color: return "number x4";
        case K::EntityRef: return "Entity";
        case K::Struct: return "table";
        default: return "number";
    }
}

void sorted(u32 flag, Array<const refl::TypeRecord*>& out) {
    for (const refl::TypeRecord* t : refl::all())
        if (t && (t->flags & flag) != 0) out.push_back(t);
    for (usize i = 1; i < out.size(); ++i)  // insertion sort by name: a few hundred records
        for (usize j = i; j > 0 && std::strcmp(out[j - 1]->name, out[j]->name) > 0; --j) {
            const refl::TypeRecord* tmp = out[j];
            out[j] = out[j - 1];
            out[j - 1] = tmp;
        }
}

void union_of(Array<char>& out, const char* alias, const Array<const refl::TypeRecord*>& types) {
    put(out, "type ");
    put(out, alias);
    put(out, " = ");
    if (types.empty()) put(out, "string");
    for (usize i = 0; i < types.size(); ++i) {
        if (i) put(out, " | ");
        put(out, "\"");
        put(out, types[i]->name);
        put(out, "\"");
    }
    put(out, "\n");
}

void field_list(Array<char>& out, const Array<const refl::TypeRecord*>& types) {
    for (const refl::TypeRecord* t : types) {
        put(out, "--   ");
        put(out, t->name);
        put(out, " {");
        for (usize i = 0; i < t->fields.size(); ++i) {
            const refl::FieldRecord& f = t->fields[i];
            put(out, i ? ", " : " ");
            put(out, f.name_str);
            put(out, ": ");
            put(out, luau_type(f));
            if (f.is_array()) put(out, "[]");
            if (f.has(refl::FieldFlags::ReadOnly)) put(out, " (read-only)");
        }
        put(out, t->fields.empty() ? "}\n" : " }\n");
    }
}

}  // namespace

void write_definitions(Array<char>& out) {
    out.clear();
    put(out,
        "-- mce.d.luau: the `mce` scripting API for Luau's type checker and editors (luau-lsp).\n"
        "-- Generated by mce_cook from the engine's reflection registry (M-SCRIPT step 4); do not edit.\n");
    put(out, StringView(kApi));
    Array<const refl::TypeRecord*> components;
    Array<const refl::TypeRecord*> events;
    sorted(refl::TypeFlags::Component, components);
    sorted(refl::TypeFlags::Event, events);
    put(out, "\n-- Every component and event a script can name.\n");
    union_of(out, "ComponentName", components);
    union_of(out, "EventName", events);
    put(out, "\n-- Components and their fields (Vec4, Quat and Color read as four numbers).\n");
    field_list(out, components);
    put(out, "\n-- Events and their fields.\n");
    field_list(out, events);
}

}  // namespace mce::script
