#pragma once
// Action-list sequences (FR-ENT-100, scripting.md §2.6): scripted beats as data. A sequence is a
// closed list of steps (asset::SequenceStep: wait, wait_event, move_to, anim, sound, stop_sound,
// emit, call, spawn, destroy); a SequencePlayer component (components.h) plays one on its entity.
//
//   kit::install_sequences(world, &assets);                   // registry + "sequence_runner"
//   auto& seqs = world.resource<kit::SequenceRegistry>();
//   seqs.register_call("shake_camera", &shake_camera);       // the only code hook
//   ... an entity with LocalTransform + SequencePlayer{sequence = guid of intro.mcseq}
//
// The runner is one FixedUpdate_Post system (after "particles_advance" when installed). Each
// tick, every active player runs steps until one waits; a waiting step uses the whole tick, and
// the step after it starts on the next tick, so a sequence's timing is tick math (Wait 0.5 s at
// 60 Hz completes on its 30th tick). The progress is the SequencePlayer's own fields, which is
// what makes sequences save/resume-able: a save mid-step resumes on the same tick.
//
// What the steps reach, all through existing paths:
//   move_to     physics2d bodies through World2D::set_linear_velocity (it lands the next step);
//               other entities through LocalTransform directly
//   anim        a SeqAnimRequest event (the game's animator consumes it)
//   sound       an audio::PlaySfx event (played by audio_bridge on the next tick)
//   stop_sound  removes the entity's AudioSource (the bridge fades it out)
//   emit        a SeqSignal event; wait_event waits for one (signals from the last tick)
//   call        a function registered by name; a missing one stops the player with an error
//   spawn       instantiates a prefab asset at a position (Commands; appears at the sync point)
//   destroy     destroys the playing entity (Commands)

#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/ecs/world.h"
#include "mce/kit/components.h"

namespace mce::kit {

// emit / wait_event. Readable from the tick after it was emitted; `entity` is the emitter (null
// for signals pushed by game code). wait_event matches any signal with its id.
struct SeqSignal {
    StringId id;
    Entity entity;
};

// anim: the game's animation system plays `clip` on `entity`.
struct SeqAnimRequest {
    Entity entity;
    StringId clip;
};

using SequenceCallFn = void (*)(Frame& f, Entity e);

class SequenceRegistry {
public:
    // Steps per player per tick before the runner declares a loop and stops the player.
    static constexpr u32 kStepBudget = 64;

    SequenceRegistry() = default;
    explicit SequenceRegistry(asset::AssetSystem* assets) : assets_(assets) {}
    void set_assets(asset::AssetSystem* assets) { assets_ = assets; }
    asset::AssetSystem* assets() const { return assets_; }

    // A sequence built from code, played by SequencePlayer::name. `steps` may omit the closing
    // End. Errors (nothing defined): an empty name or list, an End before the last step, more
    // than 4095 steps, a loop with no waiting step.
    Result<void> define(StringView name, Span<const asset::SequenceStep> steps, bool loops = false);

    // The call table (stable names, part of a game module's contract).
    void register_call(StringView name, SequenceCallFn fn);
    SequenceCallFn call(StringId id) const;

    // Call ids used by the defined and loaded sequences that have no registered function, as an
    // Errc::NotFound naming the first (the editor console runs this after modules load).
    Result<void> validate_calls() const;

    struct Def {
        StringId name;
        asset::Guid guid;
        asset::AssetKey key = asset::kInvalidAssetKey;
        u32 generation = 0;
        bool loops = false;
        Array<asset::SequenceStep> steps;  // End-terminated
    };
    // The definition a player plays: its cooked asset (copied out once, re-read on hot reload),
    // else the code-defined sequence with its name. Null when neither resolves (warned once).
    const Def* resolve(const SequencePlayer& p);

    // Factory steps for code-built sequences.
    static asset::SequenceStep wait(f32 seconds);
    static asset::SequenceStep wait_event(StringView id);
    static asset::SequenceStep move_to(math::vec3 target, f32 speed, bool relative = false, f32 tolerance = 0.01f);
    static asset::SequenceStep anim(StringView clip);
    static asset::SequenceStep sound(asset::Guid sfx, f32 gain_db = 0.0f);
    static asset::SequenceStep stop_sound();
    static asset::SequenceStep emit(StringView id);
    static asset::SequenceStep call_step(StringView id);
    static asset::SequenceStep spawn(asset::Guid prefab, math::vec3 at, bool relative = false);
    static asset::SequenceStep destroy();

private:
    asset::AssetSystem* assets_ = nullptr;
    Array<Def> defs_;  // code-defined (guid null) and loaded (guid set)
    struct Call {
        StringId id;
        SequenceCallFn fn;
    };
    Array<Call> calls_;
    Array<asset::Guid> warned_;
};

// Registers SequencePlayer and the SeqSignal / SeqAnimRequest events, sets the SequenceRegistry
// resource and adds "sequence_runner" to FixedUpdate_Post. Idempotent (re-points the assets).
void install_sequences(World& world, asset::AssetSystem* assets);

// The runner itself (exposed for tests and custom pipelines).
void sequence_runner(Frame& f);

}  // namespace mce::kit
