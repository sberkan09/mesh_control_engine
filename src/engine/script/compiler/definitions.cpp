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

-- Timed status effects (M-STATUS): thin over StatusEffects. `kind` is "regen" or "poison" (amount: hp per second, positive),
-- "armor" or "max_health" (amount: the value added, +-1000000); seconds 0.01 to 3600. Adding an active id refreshes it.
declare status: {
    -- queues an effect; `source` is credited for poison damage
    add: (e: Entity, id: string, kind: string, amount: number, seconds: number, source: Entity?) -> (),
    has: (e: Entity, id: string) -> boolean,
    -- queues the end of one effect, or of every effect when `id` is nil
    clear: (e: Entity, id: string?) -> (),
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
    -- CloudLayer (M-CLOUDS), the first one in the scene; the getters raise when there is none
    cloud_strength: () -> number,
    cloud_fair_coverage: () -> number,
    cloud_overcast_coverage: () -> number,
    cloud_scale: () -> number,
    cloud_speed_scale: () -> number,
    cloud_height: () -> number,
    -- every key optional, each value clamped to the field's range (strength, fair_coverage and overcast_coverage
    -- 0..1, scale 50..20000 m, speed_scale 0..10, height 100..20000 m); an unknown key or a non-number raises
    set_clouds: (settings: { strength: number?, fair_coverage: number?, overcast_coverage: number?, scale: number?, speed_scale: number?, height: number? }) -> (),
    -- WeatherAudio (M-WEATHERAUDIO), the first one in the scene; the getters raise when there is none.
    -- rain_level and wind_level are the engine-written bed levels (0..1), read-only
    rain_level: () -> number,
    wind_level: () -> number,
    audio_rain_db: () -> number,
    audio_wind_db: () -> number,
    audio_fade: () -> number,
    audio_enabled: () -> number,
    -- every key optional, each value clamped to the field's range (rain_db and wind_db -60..12 dB, fade 0..60 s;
    -- enabled: any non-zero number is 1); an unknown key or a non-number raises
    set_audio: (settings: { rain_db: number?, wind_db: number?, fade: number?, enabled: number? }) -> (),
    -- LightningBolt (M-LIGHTNINGBOLT), the first one in the scene; the getters raise when there is none.
    -- bolt_distance, bolt_pos (x, z of the ground point) and bolt_active (flash ticks left) are the engine-written
    -- last strike, read-only
    bolt_min_range: () -> number,
    bolt_max_range: () -> number,
    bolt_ground_y: () -> number,
    bolt_height: () -> number,
    bolt_width: () -> number,
    bolt_segments: () -> number,
    bolt_jitter: () -> number,
    bolt_fork: () -> number,
    bolt_enabled: () -> number,
    bolt_light: () -> number,
    bolt_light_radius: () -> number,
    bolt_distance: () -> number,
    bolt_pos: () -> (number, number),
    bolt_active: () -> number,
    -- every key optional, each value clamped to the field's range (min_range and max_range 10..5000 m, height
    -- 50..3000 m, width 0.05..20 m, segments 2..7, jitter 0..0.6, light 0..100000, light_radius 1..5000 m;
    -- fork and enabled: any non-zero number is 1); an unknown key or a non-number raises
    set_bolt: (settings: { min_range: number?, max_range: number?, ground_y: number?, height: number?, width: number?, segments: number?, jitter: number?, fork: number?, light: number?, light_radius: number?, enabled: number? }) -> (),
    -- WindGust (M-WINDGUST), the first one in the scene; the getters raise when there is none (set_gust never adds
    -- one). gust_level (0..1) and effective_wind (the gusted wind, all consumer bits) are engine-derived, read-only
    gust_strength: () -> number,
    gust_min_gap: () -> number,
    gust_max_gap: () -> number,
    gust_min_rise: () -> number,
    gust_max_rise: () -> number,
    gust_min_fall: () -> number,
    gust_max_fall: () -> number,
    gust_min_peak: () -> number,
    gust_max_peak: () -> number,
    gust_seed: () -> number,
    gust_affects: () -> number,
    gust_enabled: () -> number,
    gust_level: () -> number,
    effective_wind: () -> number,
    -- every key optional, each value clamped to the field's range (strength 0..3, min_gap and max_gap 0..120 s,
    -- min_rise and max_rise 0.05..30 s, min_fall and max_fall 0.05..60 s, min_peak and max_peak 0..1, seed and affects
    -- u32 rounded down; enabled: any non-zero number is 1); an unknown key or a non-number raises
    set_gust: (settings: { strength: number?, min_gap: number?, max_gap: number?, min_rise: number?, max_rise: number?, min_fall: number?, max_fall: number?, min_peak: number?, max_peak: number?, seed: number?, affects: number?, enabled: number? }) -> (),
    -- GroundMist (M-GROUNDMIST), the first one in the scene; the getters raise when there is none (set_mist never adds
    -- one). mist_level (0..1) is the level the mist has now from the hour (Environment3D.time_of_day, -1 = no clock
    -- gives 0) and the weather, read-only
    mist_strength: () -> number,
    mist_peak_hour: () -> number,
    mist_rise: () -> number,
    mist_burn: () -> number,
    mist_base_humidity: () -> number,
    mist_wet_gain: () -> number,
    mist_weather_damp: () -> number,
    mist_density: () -> number,
    mist_thickness: () -> number,
    mist_base_height: () -> number,
    mist_exp2_density: () -> number,
    mist_color: () -> (number, number, number),
    mist_enabled: () -> number,
    mist_level: () -> number,
    -- every key optional, each value clamped to the field's range (strength 0..2, peak_hour 0..24, rise_hours and
    -- burn_hours 0.1..12, base_humidity, wet_gain, weather_damp and density 0..1, thickness 0.5..200 m, base_height
    -- -1000..1000 m, exp2_density 0..0.2, color three lanes 0..1; enabled: any non-zero number is 1); an unknown key,
    -- a non-number or a color that is not three numbers raises
    set_mist: (settings: { strength: number?, peak_hour: number?, rise_hours: number?, burn_hours: number?, base_humidity: number?, wet_gain: number?, weather_damp: number?, density: number?, thickness: number?, base_height: number?, exp2_density: number?, color: { number }?, enabled: number? }) -> (),
    -- Moon (M-NIGHTSKY), the first one in the scene; the getters raise when there is none (set_moon never adds one).
    -- moon_phase (0 new, 0.5 full), moon_illumination (0..1), moon_direction (the unit direction the moonlight
    -- travels; negate it for the direction toward the moon) and moon_level (0..1) are derived from the clock
    -- (Environment3D.time_of_day, -1 = no clock gives level 0 and no moon; WorldClock.day) and the weather, read-only
    moon_phase_offset: () -> number,
    moon_cycle_days: () -> number,
    moon_freeze_phase: () -> number,
    moon_inclination: () -> number,
    moon_light_intensity: () -> number,
    moon_light_color: () -> (number, number, number),
    moon_light_shadows: () -> number,
    moon_disk_size: () -> number,
    moon_disk_intensity: () -> number,
    moon_star_density: () -> number,
    moon_star_brightness: () -> number,
    moon_star_twinkle: () -> number,
    moon_enabled: () -> number,
    moon_phase: () -> number,
    moon_illumination: () -> number,
    moon_direction: () -> (number, number, number),
    moon_level: () -> number,
    -- every key optional, each value clamped to the field's range (phase_offset 0..1, cycle_days 2..120, freeze_phase
    -- -1..1 where 0..1 pins the phase and a negative follows the clock, inclination 0..30 degrees (Moon.inclination_deg),
    -- light_intensity 0..2, light_color three lanes 0..1, disk_size 0.99..1, disk_intensity 0..50, star_density 0..1,
    -- star_brightness 0..4, star_twinkle 0..1; light_shadows and enabled: any non-zero number is 1); an unknown key, a
    -- non-number or a color that is not three numbers raises
    set_moon: (settings: { phase_offset: number?, cycle_days: number?, freeze_phase: number?, inclination: number?, light_intensity: number?, light_color: { number }?, light_shadows: number?, disk_size: number?, disk_intensity: number?, star_density: number?, star_brightness: number?, star_twinkle: number?, enabled: number? }) -> (),

    -- Aurora (M-AURORA), the first one in the scene; every aurora function raises when there is none (set_aurora never
    -- adds one). The getters use the set keys' names with an aurora_ prefix. aurora_gate (0..1) is the latitude gate of the
    -- authored latitude; aurora_level (0..4) is the brightness gain the renderer gets now, from the night fade of the
    -- current hour (0 with no clock, so only night_only 0 shows) and the cloud coverage of the first Weather and
    -- CloudLayer; both are read-only and read the authoring even when enabled is 0 (the level is then 0).
    aurora_latitude: () -> number,
    aurora_gate_low: () -> number,
    aurora_gate_high: () -> number,
    aurora_strength: () -> number,
    aurora_curtain_count: () -> number,
    aurora_base_height: () -> number,
    aurora_height_span: () -> number,
    aurora_azimuth: () -> number,
    aurora_width: () -> number,
    aurora_speed: () -> number,
    aurora_night_only: () -> number,
    aurora_enabled: () -> number,
    aurora_low_color: () -> (number, number, number),
    aurora_high_color: () -> (number, number, number),
    aurora_gate: () -> number,
    aurora_level: () -> number,
    -- every key optional, each value clamped to the field's range (latitude -90..90 degrees, gate_low and gate_high 0..90,
    -- strength 0..4, curtain_count 1..6 rounded down, base_height 0.02..0.6, height_span 0.05..0.8, azimuth 0..360
    -- degrees, width 10..180 degrees, speed 0..4, low_color and high_color three lanes 0..1; night_only and enabled: any
    -- non-zero number is 1); an unknown key, a non-number or a color that is not three numbers raises and nothing is
    -- written
    set_aurora: (settings: { latitude: number?, gate_low: number?, gate_high: number?, strength: number?, curtain_count: number?, base_height: number?, height_span: number?, azimuth: number?, width: number?, speed: number?, low_color: { number }?, high_color: { number }?, night_only: number?, enabled: number? }) -> (),

    -- Rainbow (M-RAINBOW), the first one in the scene; every rainbow function raises when there is none (set_rainbow never
    -- adds one). The getters use the set keys' names with a rainbow_ prefix. rainbow_level (0..4) is the brightness gain
    -- the renderer gets now, from the sun elevation of the current hour, the first Weather's wetness and rain and the cloud
    -- coverage of the first Weather and CloudLayer (0 without a Weather); read-only, and the getters read the authoring
    -- even when enabled is 0 (the level is then 0).
    rainbow_strength: () -> number,
    rainbow_band_width: () -> number,
    rainbow_secondary: () -> number,
    rainbow_min_wetness: () -> number,
    rainbow_max_sun_elevation: () -> number,
    rainbow_enabled: () -> number,
    rainbow_level: () -> number,
    -- every key optional, each value clamped to the field's range (strength 0..4, band_width 0.5..2, secondary 0..1,
    -- min_wetness 0..1, max_sun_elevation 10..60 degrees; enabled: any non-zero number is 1); an unknown key or a
    -- non-number raises and nothing is written
    set_rainbow: (settings: { strength: number?, band_width: number?, secondary: number?, min_wetness: number?, max_sun_elevation: number?, enabled: number? }) -> (),

    -- Meteor (M-METEOR), the first one in the scene; every meteor function raises when there is none (set_meteor never adds
    -- one). The getters use the set keys' names with a meteor_ prefix. meteor_level (0..1) is the level the renderer gets
    -- now, from the night fade of the current hour (0 with no clock, so only night_only 0 shows then), the cloud coverage of
    -- the first Weather and CloudLayer and the first Weather's rain; read-only, and the getters read the authoring even
    -- when enabled is 0 (the level is then 0).
    meteor_rate: () -> number,
    meteor_brightness: () -> number,
    meteor_length: () -> number,
    meteor_speed: () -> number,
    meteor_min_darkness: () -> number,
    meteor_night_only: () -> number,
    meteor_seed: () -> number,
    meteor_enabled: () -> number,
    meteor_level: () -> number,
    -- every key optional, each value clamped to the field's range (rate 0..30, brightness 0..4, length 0.5..2, speed 0.5..2,
    -- min_darkness 0..1, seed 0..65535 rounded down; night_only and enabled: any non-zero number is 1); an unknown key or a
    -- non-number raises and nothing is written
    set_meteor: (settings: { rate: number?, brightness: number?, length: number?, speed: number?, min_darkness: number?, night_only: number?, seed: number?, enabled: number? }) -> (),

    -- Season (M-SEASONS), the first one in the scene; every season function raises when there is none (set_season never adds
    -- one). The getters use the set keys' names with a season_ prefix; the tints return r, g, b. season_phase (0..1) is the
    -- year phase now, from the first WorldClock's day and hour (the fixed start_phase with no clock); read-only, and the
    -- getters read the authoring even when enabled is 0.
    season_enabled: () -> number,
    season_year_days: () -> number,
    season_start_phase: () -> number,
    season_winter_density: () -> number,
    season_snow_cover_bias: () -> number,
    season_cold_bias: () -> number,
    season_strength: () -> number,
    season_spring_tint: () -> (number, number, number),
    season_autumn_tint: () -> (number, number, number),
    season_winter_tint: () -> (number, number, number),
    season_phase: () -> number,
    -- every key optional, each value clamped to the field's range (year_days 1..3650, start_phase 0..1, tints 0..4 per
    -- channel, winter_density, snow_cover_bias, cold_bias and strength 0..1; enabled: any non-zero number is 1); an
    -- unknown key or a non-number raises and nothing is written
    set_season: (settings: { enabled: number?, year_days: number?, start_phase: number?, spring_tint: { number }?, autumn_tint: { number }?, winter_tint: { number }?, winter_density: number?, snow_cover_bias: number?, cold_bias: number?, strength: number? }) -> (),

    -- Water ice (M-FREEZE), the first WaterIce in the scene; every water-ice function raises when there is none
    -- (set_water_ice never adds one). The getters are water_ice_ plus the set key; the colour returns r, g, b. They read the
    -- authoring even when enabled is 0. water_frozen (0..1) is the freeze amount now, the same one the renderer draws: the
    -- Season's winter weight at the first WorldClock's day and hour (day 0 hour 0 with no clock), floored by manual; with
    -- no Season it is manual only; 0 when enabled or strength is 0. Read-only.
    water_ice_enabled: () -> number,
    water_ice_freeze_start: () -> number,
    water_ice_freeze_full: () -> number,
    water_ice_manual: () -> number,
    water_ice_ice_color: () -> (number, number, number),
    water_ice_ice_roughness: () -> number,
    water_ice_ice_reflectivity: () -> number,
    water_ice_ice_depth_m: () -> number,
    water_ice_calm: () -> number,
    water_ice_strength: () -> number,
    water_frozen: () -> number,
    -- every key optional, each value clamped to the field's range (ice_depth_m 0.05..10, the others and each colour
    -- channel 0..1; enabled: any non-zero number is 1); an unknown key or a non-number raises and nothing is written
    set_water_ice: (settings: { enabled: number?, freeze_start: number?, freeze_full: number?, manual: number?, ice_color: { number }?, ice_roughness: number?, ice_reflectivity: number?, ice_depth_m: number?, calm: number?, strength: number? }) -> (),

    -- Leaf fall (M-LEAFFALL), the first LeafFall in the scene; every leaf-fall function raises when there is none
    -- (set_leaf_fall never adds one). The getters are leaf_fall_ plus the set key; the colour returns r, g, b. They read the
    -- authoring even when enabled is 0. leaf_falling (0..1) is the leaf amount now, the same one the renderer draws: the
    -- autumn window of the first Season at the first WorldClock's day and hour (day 0 hour 0 with no clock), floored by
    -- manual; with no Season it is manual only; 0 when enabled or strength is 0. Read-only.
    leaf_fall_enabled: () -> number,
    leaf_fall_autumn_start: () -> number,
    leaf_fall_autumn_full: () -> number,
    leaf_fall_autumn_end_full: () -> number,
    leaf_fall_autumn_end: () -> number,
    leaf_fall_manual: () -> number,
    leaf_fall_rate: () -> number,
    leaf_fall_box_xz_m: () -> number,
    leaf_fall_height_m: () -> number,
    leaf_fall_size_m: () -> number,
    leaf_fall_lifetime_s: () -> number,
    leaf_fall_fall_speed: () -> number,
    leaf_fall_wind_follow: () -> number,
    leaf_fall_leaf_color: () -> (number, number, number),
    leaf_fall_tint_follow: () -> number,
    leaf_fall_max_particles: () -> number,
    leaf_fall_seed: () -> number,
    leaf_fall_strength: () -> number,
    leaf_falling: () -> number,
    -- every key optional, each value clamped to the field's range (autumn_*, manual, tint_follow, strength and each colour
    -- channel 0..1, rate 0..500, box_xz_m 1..100, height_m 1..50, size_m 0.02..1, lifetime_s 1..30, fall_speed 0..5,
    -- wind_follow 0..2, max_particles 64..8192, seed 0..4294967295; enabled: any non-zero number is 1); an unknown key or a
    -- non-number raises and nothing is written
    set_leaf_fall: (settings: { enabled: number?, autumn_start: number?, autumn_full: number?, autumn_end_full: number?, autumn_end: number?, manual: number?, rate: number?, box_xz_m: number?, height_m: number?, size_m: number?, lifetime_s: number?, fall_speed: number?, wind_follow: number?, leaf_color: { number }?, tint_follow: number?, max_particles: number?, seed: number?, strength: number? }) -> (),

    -- Fireflies (M-FIREFLIES), the first Firefly in the scene; every firefly function raises when there is none
    -- (set_firefly never adds one). The getters are firefly_ plus the set key; the colour returns r, g, b. They read the
    -- authoring even when enabled is 0. fireflies_glowing (0..1) is the firefly amount now, the same one the renderer draws:
    -- the summer window of the first Season at the first WorldClock's day and hour (day 0 hour 0 with no clock), floored by
    -- manual, times the night gate (the Environment3D hour; 0 with no clock, so only night_only 0 shows) and the dryness
    -- (1 - rain); with no Season it is the manual floor; 0 when enabled or strength is 0. Read-only.
    firefly_enabled: () -> number,
    firefly_summer_start: () -> number,
    firefly_summer_full: () -> number,
    firefly_summer_end_full: () -> number,
    firefly_summer_end: () -> number,
    firefly_manual: () -> number,
    firefly_night_only: () -> number,
    firefly_min_darkness: () -> number,
    firefly_rate: () -> number,
    firefly_box_xz_m: () -> number,
    firefly_height_m: () -> number,
    firefly_size_m: () -> number,
    firefly_lifetime_s: () -> number,
    firefly_drift_speed: () -> number,
    firefly_max_particles: () -> number,
    firefly_seed: () -> number,
    firefly_strength: () -> number,
    firefly_color: () -> (number, number, number),
    fireflies_glowing: () -> number,
    -- every key optional, each value clamped to the field's range (summer_*, manual, min_darkness, strength and each colour
    -- channel 0..1, rate 0..200, box_xz_m 1..100, height_m 1..20, size_m 0.02..0.5, lifetime_s 1..30, drift_speed 0..2,
    -- max_particles 16..4096, seed 0..4294967295; enabled and night_only: any non-zero number is 1); an unknown key or a
    -- non-number raises and nothing is written
    set_firefly: (settings: { enabled: number?, summer_start: number?, summer_full: number?, summer_end_full: number?, summer_end: number?, manual: number?, night_only: number?, min_darkness: number?, rate: number?, box_xz_m: number?, height_m: number?, size_m: number?, lifetime_s: number?, drift_speed: number?, color: { number }?, max_particles: number?, seed: number?, strength: number? }) -> (),
}

declare water: {
    -- Water (M-WATER), the first one in the scene; every function raises when there is none (set never adds one).
    -- The getters use the set keys' names; height_at, normal_at and submerged are the drawn surface at the scene time
    -- (tick * dt), with the WindDirection heading as the main direction when wind_follow > 0 and the scene has one;
    -- read-only, and they read the authoring even when enabled is 0. submerged is true when y is below height_at.
    level: () -> number,
    half_extent: () -> number,
    wave_count: () -> number,
    amplitude: () -> number,
    wavelength: () -> number,
    steepness: () -> number,
    wind_dir: () -> number,
    wind_follow: () -> number,
    speed_scale: () -> number,
    shallow_color: () -> (number, number, number),
    deep_color: () -> (number, number, number),
    depth_scale: () -> number,
    reflectivity: () -> number,
    roughness: () -> number,
    foam_amount: () -> number,
    foam_width: () -> number,
    enabled: () -> number,
    height_at: (x: number, z: number) -> number,
    normal_at: (x: number, z: number) -> (number, number, number),
    submerged: (x: number, y: number, z: number) -> boolean,
    -- every key optional, each value clamped to the field's range (level -1000..1000 m, half_extent 1..20000 m,
    -- wave_count 1..8 rounded down, amplitude 0..10 m, wavelength 1..500 m, steepness 0..1, wind_dir 0..360 degrees,
    -- wind_follow 0..1, speed_scale 0..4, shallow_color and deep_color three lanes 0..1, depth_scale 0.1..200 m,
    -- reflectivity 0..1, roughness 0..1, foam_amount 0..1, foam_width 0.05..20 m; enabled: any non-zero number is 1);
    -- an unknown key, a non-number or a colour that is not three numbers raises and nothing is written
    set: (settings: { level: number?, half_extent: number?, wave_count: number?, amplitude: number?, wavelength: number?, steepness: number?, wind_dir: number?, wind_follow: number?, speed_scale: number?, shallow_color: { number }?, deep_color: { number }?, depth_scale: number?, reflectivity: number?, roughness: number?, foam_amount: number?, foam_width: number?, enabled: number? }) -> (),
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
