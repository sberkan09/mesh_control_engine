#pragma once
// Playtesting without a person (ROADMAP M-AGENT step 3): field probes on a running world, the
// conditions of a .mctest scenario, the kit bots (walk_to, explore, masher), the feel report of a
// bot run and run traces. Private to mce_mcp_core: tools_test.cpp (test_run, playtest) and
// run_headless's `trace` (tools.cpp) use it through RunHooks (headless_run.h).
//
// Entities are named as the played scene file names them (an id or a unique name) and found in
// the run by file order (spawned[i] is scene entity i), so an entity after a prefab instance (the
// cook expands it into several) is refused.

#include "mce/editor/scene_document.h"
#include "mce/input/script.h"
#include "mce/log.h"
#include "mce/mcp/headless_run.h"
#include "mce/mcp/json_writer.h"
#include "mce/refl/property_path.h"

namespace mce::mcp {

class Server;

// Reads and parses a saved scene of the project (`rel` under assets/).
bool load_scene_doc(Server& server, StringView rel, editor::SceneDocument& doc, cook::String& err);
// The run index of a scene entity given by id (integer) or unique name (string).
bool scene_entity_index(const editor::SceneDocument& doc, const cook::JsonValue& v, usize& index, cook::String& err);

// One component field of one scene entity, resolved once: "LocalTransform.position.x".
struct FieldProbe {
    usize index = 0;
    const refl::TypeRecord* type = nullptr;
    refl::PropertyRef ref;
    cook::String entity;  // as given, for reports
    cook::String field;
};
bool parse_probe(const editor::SceneDocument& doc, const cook::JsonValue& entity, StringView field, FieldProbe& out,
                 cook::String& err);

// A probed (or expected) value: numbers (1-4 lanes: vectors, quats, colors), bools, strings and
// enum names.
struct Value {
    enum class Kind : u8 { None, Number, Bool, String };
    Kind kind = Kind::None;
    u32 lanes = 0;
    f64 v[4] = {};
    FixedString<128> s;
};
// False when the entity is gone or lacks the component (out.kind stays None).
bool read_value(const World& world, Span<const Entity> spawned, const FieldProbe& p, Value& out);
bool parse_value(const cook::JsonValue& v, Value& out);
void write_value(JsonWriter& w, const Value& v);

// An actor's position: WorldTransform when the hierarchy wrote one, else LocalTransform.
bool entity_position(const World& world, Entity e, math::vec3& out);

// ---------------------------------------------------------------- feel report

// What a run felt like for one actor (the bot's, or a test's "actor"): how long it took, how often
// it died (the entity went away, jumped more than `teleport` metres in one tick, or the test's
// "death" condition became true), where it got stuck (asked to move, moved under 0.25 m in a
// second), how long it stood still and how much ground it covered (1 m cells).
struct FeelSpot {
    math::vec3 at;
    u64 first_tick = 0;
    u64 ticks = 0;
};

class FeelTracker {
public:
    static constexpr usize kWindow = 60;
    static constexpr usize kMaxSpots = 16;
    f32 teleport = 3.0f;

    void tick(const World& world, Span<const Entity> spawned, usize actor, u64 tick, bool wants_move, bool died_now);
    void finish(u64 tick) {
        if (finished_tick_ == 0) finished_tick_ = tick;
    }
    u64 finished_tick() const { return finished_tick_; }
    u32 deaths() const { return deaths_; }
    // A named metric for conditions: deaths, idle_ticks, stuck_ticks, stuck_spots, distance,
    // cells, finished_tick (0: never finished). False for an unknown name.
    bool metric(StringView name, f64& out) const;
    void write(JsonWriter& w, u64 ticks) const;

private:
    math::vec3 window_[kWindow];
    u64 samples_ = 0;
    math::vec3 last_;
    bool alive_ = false;
    bool was_alive_ = false;
    u32 deaths_ = 0;
    u64 idle_ = 0;
    u64 stuck_ = 0;
    f64 distance_ = 0;
    u64 finished_tick_ = 0;
    math::vec3 lo_, hi_;
    Array<u64> cells_;  // hashed 1 m cells, sorted
    Array<FeelSpot> spots_;
};

// ---------------------------------------------------------------- conditions

enum class CompareOp : u8 { Eq, Ne, Lt, Le, Gt, Ge };

// One check of a test: a component field, whether an entity is alive, how many events of a type
// were pushed, the log, or a feel metric; at the end of the run or after tick `tick`.
struct Condition {
    enum class What : u8 { Field, Alive, Event, Scene, LogContains, LogErrors, LogWarnings, Feel };
    What what = What::Field;
    FieldProbe probe;          // Field, Alive (index only)
    CompareOp op = CompareOp::Eq;
    Value expected;
    f64 tolerance = 1e-4;      // numbers compare equal within this
    cook::String text;         // event type, scene path, log text, feel metric
    u64 tick = 0;              // 0: at the end
    // Result.
    bool checked = false;
    bool ok = false;
    Value actual;
    cook::String note;  // why the check could not read what it names (an unknown event type)
};
// Field, Alive, Event and Scene: read from the world during the run (the rest after it).
inline bool is_world_check(Condition::What w) {
    return w == Condition::What::Field || w == Condition::What::Alive || w == Condition::What::Event || w == Condition::What::Scene;
}
bool parse_condition(const editor::SceneDocument& doc, const cook::JsonValue& v, Condition& out, cook::String& err);
// The world checks (false for the others: they need the log or the feel report).
bool eval_world(Condition& c, const World& world, Span<const Entity> spawned);
void eval_after(Condition& c, Span<const log::ConsoleLine> lines, const FeelTracker* feel);
void write_condition(JsonWriter& w, const Condition& c);

// ---------------------------------------------------------------- bots

// A built-in player that drives the game's input actions from the actor's position (no game code):
//   walk_to  heads for `target` (a point or an entity), jumps (side view) or sidesteps (top-down)
//            when it stops getting closer; done within `reach` metres. `through` lists waypoints
//            (points or entities) it reaches first, in order: around a pond, past each pickup.
//   explore  wanders in seeded random directions, turning when blocked; reports coverage.
//   masher   presses a seeded random mix of `actions` every `period` ticks (crash and soft-lock hunting).
// `plane`: "xy" (Move = the direction in the XY plane, 2D top-down or flying), "x" (side view:
// Move.x only, Jump to climb), "xz" (3D on the ground: Move = (x, -z), forward is -Z).
struct BotTarget {
    bool is_entity = false;
    usize index = 0;  // the entity's index in the scene (is_entity)
    math::vec3 point;
};

struct BotConfig {
    enum class Kind : u8 { WalkTo, Explore, Masher };
    enum class Plane : u8 { XY, X, XZ };
    Kind kind = Kind::WalkTo;
    Plane plane = Plane::XY;
    bool has_actor = false;
    usize actor = 0;
    Array<BotTarget> targets;  // walk_to: the waypoints (`through`), then `target`
    FixedString<32> move{"Move"};
    FixedString<32> jump{"Jump"};
    Array<FixedString<32>> actions;  // masher
    f32 reach = 0.5f;
    u32 air_jumps = 0;  // walk_to, side view: extra Jump presses in the air (a double jump, a flap)
    u64 seed = 1;
    u32 period = 8;
};
bool parse_bot(const editor::SceneDocument& doc, const cook::JsonValue& v, BotConfig& out, cook::String& err);
const char* bot_kind_name(BotConfig::Kind k);

class Bot final : public app::IInputSource {
public:
    explicit Bot(const BotConfig& cfg) : cfg_(cfg), rng_(cfg.seed * 0x9E3779B97F4A7C15ull + 1) {}
    void attach(const World& world, Span<const Entity> spawned) {
        world_ = &world;
        spawned_ = spawned;
    }
    const input::InputState* sample_for_tick(u64 tick, u64 tick_close_us) override;
    bool reached() const { return reached_; }
    usize leg() const { return leg_; }  // walk_to: the index of the target it heads for
    bool wants_move() const { return wants_move_; }
    const BotConfig& config() const { return cfg_; }

private:
    u64 next();
    f32 unit() { return static_cast<f32>(next() >> 40) / static_cast<f32>(1u << 24); }
    void walk_to(u64 tick, math::vec3 pos);
    void explore(u64 tick, math::vec3 pos);
    void masher(u64 tick);
    void hold_move(u64 tick, math::vec2 dir);

    BotConfig cfg_;
    u64 rng_;
    const World* world_ = nullptr;
    Span<const Entity> spawned_;
    input::InputScript script_;
    bool reached_ = false;
    bool wants_move_ = false;
    usize leg_ = 0;
    // Steering state.
    f32 best_ = 1e30f;
    u64 best_tick_ = 0;
    u64 detour_until_ = 0;
    f32 detour_side_ = 1.f;
    u64 jump_until_ = 0;
    u64 jump_from_ = 0;
    u64 turn_at_ = 0;
    math::vec2 heading_;
    math::vec3 last_pos_;
    u64 still_since_ = 0;
    u64 chord_ = 0;
};

// ---------------------------------------------------------------- traces

// run_headless's `trace` (and a test's): chosen fields every `every` ticks.
struct Trace {
    static constexpr usize kMaxFields = 16;
    static constexpr usize kMaxRows = 1000;
    u64 every = 10;
    Array<FieldProbe> fields;
    Array<u64> ticks;
    Array<Value> values;  // ticks.size() rows of fields.size()
    bool active() const { return !fields.empty(); }
    void sample(const World& world, Span<const Entity> spawned, u64 tick);
    void write(JsonWriter& w) const;
};
bool parse_trace(const editor::SceneDocument& doc, const cook::JsonValue& v, Trace& out, cook::String& err);

// ---------------------------------------------------------------- one run

// Everything a run watches; hooks() hands it to run_headless.
struct SimRun {
    Bot* bot = nullptr;
    const input::InputScript* script = nullptr;  // scripted input instead of a bot: "asked to move" = Move held
    Trace* trace = nullptr;
    FeelTracker* feel = nullptr;
    bool has_actor = false;
    usize actor = 0;
    Array<Condition>* checks = nullptr;  // evaluated at their tick (tick > 0); the rest at the end
    Condition* until = nullptr;          // ends the run when it holds (checked every tick)
    Condition* death = nullptr;          // counts a death each time it becomes true
    bool death_was = false;
    bool stopped_early = false;
    bool left_scene = false;  // the game changed scenes: the starting scene's entities are gone
    u64 reached_at = 0;       // walk_to: the tick it reached its target (the run goes on kSettleTicks)
    static constexpr u64 kSettleTicks = 30;
    u64 last_tick = 0;
    RunHooks hooks();
};

}  // namespace mce::mcp
