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
type SphereHit = { entity: Entity, point: vector, normal: vector, position: vector, fraction: number }
type MarkerOptions = { icon: number?, color: { number } | vector?, colour: { number } | vector?, layer: number?, label: string?, always_visible: boolean?, follow: Entity? }
type MoveResult2D = { position: vector, grounded: boolean, ground: Entity?, ceiling: boolean, wall_left: boolean, wall_right: boolean }

declare mce: {
    -- the fixed step in seconds
    dt: number,
    -- element: for an array field such as ScriptVars.v, a handle to one slot (1-based): mce.get(e, h) is then that element's value
    field: (component: string, field: string, element: number?) -> FieldHandle,
    -- mce.get(e, handle, ...) or mce.get(e, "Comp", "field"): one value per field
    get: (e: Entity, ...any) -> ...any,
    -- mce.set(e, handle, value, ...) or mce.set(e, "Comp", "field", value)
    set: (e: Entity, ...any) -> (),
    -- mce.each("A", "B", ..., function(e) ... end)
    each: (...any) -> (),
    tick: () -> number,
    random: () -> number,
    -- level: "info" (default), "warn" or "error"; a warn or error line counts in a .mctest's log_warnings / log_errors checks
    log: (message: string, level: string?) -> (),

    has: (e: Entity, component: string) -> boolean,
    -- the number the scene gave this entity for `name` in its ScriptParams (names: "speed jump", v: [5, 8]); default when it has none
    param: (e: Entity, name: string, default: number?) -> number,
    alive: (e: Entity) -> boolean,
    find: (name: string) -> Entity?,
    spawn: (components: Fields) -> Entity,
    add: (e: Entity, component: string, fields: Fields?) -> (),
    remove: (e: Entity, component: string) -> (),
    destroy: (e: Entity) -> (),
    instantiate: (prefab: string, position: vector?) -> Entity?,

    events: (event: string) -> { Fields },
    emit: (event: string, fields: Fields?) -> (),
    signal: (name: string, e: Entity?) -> (),
    signals: (name: string) -> { Entity | boolean },
    make_noise: (position: vector, loudness: number?, source: Entity?, team: number?) -> (),
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
    -- the entities with a LocalTransform within radius of center (and the component, if given), nearest first; no physics needed
    near: (center: vector, radius: number, component: string?, exclude: Entity?) -> { Entity },
    -- the entities with a LocalTransform inside the box from min to max (inclusive; a 2D game has z = 0), in storage order; no physics needed
    within: (min: vector, max: vector, component: string?, exclude: Entity?) -> { Entity },
    move_and_collide: (e: Entity, delta: vector) -> MoveResult2D,
    teleport2d: (e: Entity, position: vector) -> (),
    find_path: (from: vector, to: vector, options: PathOptions?) -> { vector }?,
    save: (slot: string) -> (boolean, string?),
    load: (slot: string) -> boolean,
    has_save: (slot: string) -> boolean,
    set_time_scale: (scale: number) -> (),
    time_scale: () -> number,
    shake: (trauma: number) -> (),
    burst: (e: Entity, count: number?) -> (),
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
    overlap_sphere3d: (center: vector, radius: number, skip: Entity?) -> { Entity },
    overlap_box3d: (center: vector, half: vector, skip: Entity?) -> { Entity },
    sphere_cast3d: (from: vector, to: vector, radius: number, skip: Entity?) -> SphereHit?,
    velocity3d: (e: Entity) -> vector,
    set_velocity3d: (e: Entity, v: vector) -> (),
    angular_velocity3d: (e: Entity) -> vector,
    set_angular_velocity3d: (e: Entity, w: vector) -> (),
    impulse3d: (e: Entity, impulse: vector) -> (),
    teleport3d: (e: Entity, position: vector, rotation: vector?) -> (),
    rotation3d: (e: Entity) -> vector,
    set_rotation3d: (e: Entity, rotation: vector) -> (),
    look_rotation3d: (from: vector, to: vector) -> vector,
    find_path3d: (from: vector, to: vector) -> ({ vector }?, boolean?),
}

-- Missions (M-MISSION): thin over the Mission component and the MissionSignal event.
declare mission: {
    -- the idle mission starts on the next tick; false when it already ran
    start: (e: Entity) -> boolean,
    -- an active mission fails now (MissionFailed, reason 3); false in any other state
    fail: (e: Entity) -> boolean,
    -- state ("idle" | "active" | "complete" | "failed"), the active objective (nil unless active), seconds since it started, objectives done
    status: (e: Entity) -> (string, Entity?, number, number),
    -- the named signal that `signal` objectives count (amount: a whole number, default 1)
    signal: (name: string, amount: number?) -> (),
}

-- Inventories (M-INVENTORY): thin over Inventory, Equipment and Container. Slots count from 1 (1 to 24); `where` is
-- "head", "body", "hands", "feet", "weapon", "offhand", "accessory1" or "accessory2". The queued calls land a few ticks later.
declare inventory: {
    add: (e: Entity, id: string, qty: number?) -> (),
    remove: (e: Entity, id: string, qty: number?) -> (),
    count: (e: Entity, id: string) -> number,
    has: (e: Entity, id: string, qty: number?) -> boolean,
    -- carried weight in thousandths of a kilogram
    weight: (e: Entity) -> number,
    -- the item id and quantity in the slot (nil, 0 when empty)
    slot: (e: Entity, i: number) -> (string?, number),
    equip: (e: Entity, i: number, where: string?) -> (),
    unequip: (e: Entity, where: string) -> (),
    use: (e: Entity, i: number) -> (),
    -- takes qty (default the whole stack) out of the slot now and spawns an ItemPickup3D; returns how many
    drop: (e: Entity, i: number, qty: number?) -> number,
    open: (container: Entity, opener: Entity) -> (),
    -- whether the recipe could be made now: inputs present, room for the outputs, and the station when the recipe names one
    can_craft: (e: Entity, recipe: string, station: Entity?) -> boolean,
    -- queues a craft (M-CRAFT); `station` is an entity with a CraftingStation, nil to craft by hand
    craft: (e: Entity, recipe: string, station: Entity?) -> (),
}

-- Map markers (M-MISSION): the MapMarker component, written at the next sync point.
declare marker: {
    add: (e: Entity, options: MarkerOptions?) -> (),
    remove: (e: Entity) -> (),
}

-- World clock (M-DAYNIGHT): thin over the first WorldClock component.
declare clock: {
    -- the hour of the day, 0..24
    hour: () -> number,
    -- whole days passed since the start
    day: () -> number,
    -- sets the hour (0..24); the clock re-seats itself on its next tick
    set: (hour: number) -> (),
    -- stops or resumes the clock
    pause: (paused: boolean) -> (),
    -- WorldClock.time_scale, clamped to 0..100
    speed: (scale: number) -> (),
    paused: () -> boolean,
}

-- Weather (M-WEATHER): thin over the first Weather component.
declare weather: {
    -- the kind in effect: "clear" | "overcast" | "rain" | "storm" | "snow"
    kind: () -> string,
    -- the kind the weather is moving to
    target: () -> string,
    -- writes Weather.target; with seconds also Weather.transition_seconds (0..3600)
    set: (kind: string, transition_seconds: number?) -> (),
    -- 0..1 blend of the kind in effect
    intensity: () -> number,
    wind: () -> number,
    wetness: () -> number,
    -- WetSurfaces (M-WETSURFACE), the first one in the scene; the getters raise when there is none
    wet_strength: () -> number,
    wet_darken: () -> number,
    wet_puddles: () -> number,
    wet_puddle_scale: () -> number,
    -- every key optional, each value clamped to the field's range (strength 0..4, darken 0..1, puddles 0..1,
    -- puddle_scale 0.1..100 metres); an unknown key or a non-number raises
    set_wet: (settings: { strength: number?, darken: number?, puddles: number?, puddle_scale: number? }) -> (),
    -- WindSway (M-WINDSWAY), the first one in the scene; the getters raise when there is none. The foliage wind
    -- is scaled by 1 + (gain - 1) * weather.wind()
    sway_amplitude: () -> number,
    sway_frequency: () -> number,
    sway_gust: () -> number,
    sway_flutter: () -> number,
    -- every key optional, each value clamped to the field's range (amplitude 0..50, frequency 0..10, gust -1..1,
    -- flutter 0..50); an unknown key or a non-number raises
    set_sway: (settings: { amplitude: number?, frequency: number?, gust: number?, flutter: number? }) -> (),
    -- SnowCover (M-SNOWCOVER), the first one in the scene; the getters raise when there is none. snow_cover is the
    -- engine-written cover 0..1 (read-only); the others are the authoring fields
    snow_cover: () -> number,
    snow_strength: () -> number,
    snow_build: () -> number,
    snow_melt: () -> number,
    snow_coverage: () -> number,
    snow_scale: () -> number,
    snow_wind: () -> number,
    -- every key optional, each value clamped to the field's range (strength 0..4, build and melt 0..86400 seconds,
    -- coverage 0..1, scale 0.1..100 metres, wind 0..1); an unknown key or a non-number raises
    set_snow: (settings: { strength: number?, build: number?, melt: number?, coverage: number?, scale: number?, wind: number? }) -> (),
    -- RainRipples (M-RAINFX), the first one in the scene; the getters raise when there is none
    ripples_strength: () -> number,
    ripples_density: () -> number,
    ripples_speed: () -> number,
    ripples_ring_width: () -> number,
    -- every key optional, each value clamped to the field's range (strength 0..1, density 0..1, speed 0.1..10,
    -- ring_width 0..1); an unknown key or a non-number raises
    set_ripples: (settings: { strength: number?, density: number?, speed: number?, ring_width: number? }) -> (),
    -- RainSplash (M-RAINFX), the first one in the scene; the engine-written counter is not reachable
    splash_rate: () -> number,
    splash_height: () -> number,
    splash_life: () -> number,
    splash_radius: () -> number,
    splash_seed: () -> number,
    -- every key optional, each value clamped to the field's range (rate 0..64, height 0.01..5 metres, life 0.02..5
    -- seconds, radius 0.5..500 metres, seed 0..4294967295); an unknown key or a non-number raises
    set_splash: (settings: { rate: number?, height: number?, life: number?, radius: number?, seed: number? }) -> (),
    -- ThunderDelay (M-RAINFX), the first one in the scene; the clip and the engine-written queue are not reachable
    thunder_speed: () -> number,
    thunder_volume: () -> number,
    -- every key optional, each value clamped to the field's range (speed 1..1000000 m/s, volume 0..4); an unknown
    -- key or a non-number raises
    set_thunder: (settings: { speed: number?, volume: number? }) -> (),
    -- WindDirection (M-WINDDIR), the first one in the scene; the getters raise when there is none. wind_degrees and
    -- wind_drift are the authored fields, wind_heading the resolved angle in degrees (0..360, engine-written)
    wind_degrees: () -> number,
    wind_drift: () -> number,
    wind_heading: () -> number,
    -- every key optional, each value clamped to the field's range (degrees 0..360, drift -3600..3600 degrees per
    -- minute); an unknown key (angle_q included) or a non-number raises
    set_wind_direction: (settings: { degrees: number?, drift: number? }) -> (),
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
