#pragma once
// PlaySession — play-in-editor (editor-tools.md §2.8, FR-EDT-060, ADR-0029): the open scene runs
// inside the editor in a world of its own, built the way mce_player builds it.
//
//   enter    serialize the document -> cook it in memory with the scene importer (the same bytes the
//            cook would write for the saved file, prefab instances flattened) -> scene::instantiate
//            into a fresh World with physics2d + the audio bridge on a Silent device: mce_player's
//            headless setup, so the same scene and tick count give the same World::state_hash.
//   stop     destroys the play world. The document and the preview world are never touched, so
//            anything changed in the play world is discarded by design.
//   advance  the app loop's fixed-step accumulator (1/60 s ticks, time scale scales the inflow,
//            at most kMaxCatchup ticks per call); pause stops it and step() runs exactly one tick.
//   input    the project's action map (the first cooked .mcinput by path, else the built-in
//            default map, as mce_player picks it) on an InputSystem of the session's own. The host
//            feeds it window events with push_event; each tick samples the events stamped before
//            its close and publishes the InputState resource before the world ticks. Without
//            events the snapshot stays zero, so headless runs keep mce_player's hash.
//
// The cooked tree the play world reads (textures, fonts, clips) is the project's mounted one: stop
// before anything remounts it (EditorSession::cook does).

#include "mce/ecs/world.h"
#include "mce/editor/project.h"
#include "mce/editor/scene_document.h"

namespace mce::input {
class InputSystem;
}  // namespace mce::input

namespace mce::platform {
struct Event;
}  // namespace mce::platform

namespace mce::audio {
class AudioDevice;
class ClipBank;
}  // namespace mce::audio

namespace mce::editor {

// The game a project editor plays (mce_add_project's <game>_editor links the game in): its install()
// runs on the play world after the engine systems, as a loaded module's would in the host. Null
// (mce_editor itself): engine systems only.
using GameInstallFn = ResultV (*)(World& world);
void set_play_game_install(GameInstallFn install);
GameInstallFn play_game_install();

class PlaySession {
public:
    enum class State : u8 { Stopped, Playing, Paused };

    static constexpr f64 kTickDt = 1.0 / 60.0;
    static constexpr u32 kMaxCatchup = 5;  // ticks per advance() (the app loop's sim.max_catchup)

    PlaySession();
    ~PlaySession();
    PlaySession(const PlaySession&) = delete;
    PlaySession& operator=(const PlaySession&) = delete;

    // Builds the play world from `doc` (a project with a mounted cooked tree is required). On
    // failure nothing is playing and `diags` says why (cook errors carry file:line like a cook).
    Result<void> enter(EditorProject& project, const SceneDocument& doc, StringView scene_path, cook::DiagList& diags);
    void stop();

    // Runs the ticks `real_dt` seconds of wall time buy at the current time scale (none while
    // paused), then syncs audio clips. Returns the number of ticks run.
    u32 advance(f64 real_dt);
    void pause();
    void resume();
    // Paused: runs exactly one tick (single-step). Playing: pauses first.
    void step();
    // Slow motion / fast forward: scales the accumulator inflow, never dt (FR-RUN-050 holds).
    void set_time_scale(f32 scale);
    f32 time_scale() const { return time_scale_; }

    // The game's input (FR-EDT-060): one window event, stamped on the engine clock. The editor
    // passes everything while playing and flags presses it kept for itself ConsumedByEditor, so
    // a key held when the viewport loses focus still releases. Ignored while stopped.
    void push_event(const platform::Event& e);
    // The play world's InputSystem (null while stopped): tools and tests read state and stats.
    input::InputSystem* input() { return input_.get(); }

    State state() const { return state_; }
    bool active() const { return state_ != State::Stopped; }
    World* world() { return world_.get(); }
    const World* world() const { return world_.get(); }
    u64 ticks() const { return ticks_; }
    u64 state_hash() const;
    // The world's hash right after the scene was instantiated, before the first tick.
    u64 enter_hash() const { return enter_hash_; }
    // Wall time of the last enter(): cook-in-memory + instantiate (NFR-M-03 budget: 1 s).
    f64 enter_seconds() const { return enter_seconds_; }
    // Wall time and tick count of the last advance() that ran (the Profiler's simulation stage).
    // The play world profiles its systems (World::system_timings).
    f64 last_advance_ms() const { return last_advance_ms_; }
    u32 last_advance_ticks() const { return last_advance_ticks_; }
    usize entity_count() const { return entities_.size(); }
    // The play-world entity spawned for scene entity `i` (document order).
    Span<const Entity> entities() const { return entities_.span(); }

private:
    // `close_us`: the end of this tick's input window on the engine clock.
    void tick_once(u64 close_us);

    mem::UniquePtr<input::InputSystem> input_;
    mem::UniquePtr<audio::AudioDevice> audio_;
    mem::UniquePtr<audio::ClipBank> clips_;
    mem::UniquePtr<World> world_;
    Array<Entity> entities_;
    mem::UniquePtr<mem::LinearArena> scratch_;  // frame arena for ticks run outside an app frame
    State state_ = State::Stopped;
    f64 accumulator_ = 0.0;
    f32 time_scale_ = 1.0f;
    u64 ticks_ = 0;
    u64 last_close_us_ = 0;
    u64 enter_hash_ = 0;
    f64 enter_seconds_ = 0.0;
    f64 last_advance_ms_ = 0.0;
    u32 last_advance_ticks_ = 0;
};

}  // namespace mce::editor
