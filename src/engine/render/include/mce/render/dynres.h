#pragma once
// M-DYNRES 46b: the pure CPU controller of dynamic resolution. A frame costs ms = c * ((1 - v) + v * s^2) at render scale s
// (c = the cost at scale 1, v = the share that follows the pixel count), so a measured time converts to a scale independent
// cost, is smoothed, and picks the scale of a later frame: down at once on an overrun, up one 1/16 step at a time after a
// cooldown and with headroom. Also the settings resolve and the test injection schedule (`r.dynres.inject`, "22@0,9@200").
// f32 / u32 only, fixed evaluation order, no rand, no clock, no global state; a non-finite input is a rejected sample (the
// scale holds). Included by dynres.cpp, forward3d.cpp (46c), quality.cpp, the kit extract and install (46d) and the tests only.
//
// This header must stay narrow (it does not include scene3d.h, a wide header), so the render types are forward-declared.

#include "mce/types.h"

namespace mce::render {

struct DynamicResolution;

// The default share of a frame's cost that follows the pixel count (shadows, the 2D/UI and the upsample do not).
inline constexpr f32 kDynResVariableShare = 0.7f;
// The scale grid: k / kDynResGrid.
inline constexpr u32 kDynResGrid = 16;
// The most entries a test injection schedule holds.
inline constexpr u32 kDynResMaxInject = 8;

// The cost the same frame would have at scale 1, from `ms` measured at `scale`. ms / ((1 - v) + v * scale^2).
f32 dynres_cost_full(f32 ms, f32 scale, f32 variable_share);
// The forward direction: c * ((1 - v) + v * scale^2).
f32 dynres_ms_at(f32 cost_full, f32 scale, f32 variable_share);
// The largest continuous scale in 0..1 whose cost fits `target_ms`: sqrt(max((target / c - (1 - v)) / v, 0)); 1 when c <= 0.
f32 dynres_scale_for(f32 cost_full, f32 target_ms, f32 variable_share);

// Rounds `scale` down to the grid: k / 16 for the integers in [floor, ceiling) plus the ceiling itself exactly (a ceiling
// off the grid, the tier's 0.67, is returned bit for bit). `floor` is snapped up to the grid; a floor above the ceiling (or
// a NaN input) gives the ceiling.
f32 dynres_quantize(f32 scale, f32 floor, f32 ceiling);

struct DynResParams {
    f32 target_ms = 14.0f;
    f32 floor = 0.6f;
    f32 ceiling = 1.0f;
    f32 headroom = 0.15f;
    u32 max_down_steps = 4;   // grid steps one drop moves at most
    u32 down_cooldown = 8;    // frames before the next drop after a drop
    u32 up_cooldown = 60;     // frames before the next rise after any move (and the first after a rise)
    f32 ema = 0.25f;          // the smoothing weight of a new sample
    f32 spike = 2.0f;         // a sample is clamped to this times the smoothed cost
    f32 variable_share = kDynResVariableShare;
};

struct DynResState {
    f32 scale = 1.0f;   // the scale in force (starts at the ceiling)
    f32 cost = 0.0f;    // the smoothed cost at scale 1 (0 = no sample yet)
    u32 cooldown = 0;   // frames until the next rise (or the next drop after a drop)
    u32 samples = 0;    // valid samples consumed
};

// One controller step. `has_sample` false (or a non-finite or negative sample, or a scale outside (0, 1]) holds the scale and
// only counts the cooldown down to 0. A drop is allowed when the cooldown is 0 or was set by a rise (an overrun right after
// a rise is not made to wait); a rise needs cooldown 0 and the estimate at the next grid step up to be within
// target * (1 - headroom). Returns the new scale, which is also stored in `state.scale`.
f32 dynres_step(const DynResParams& p, DynResState& state, bool has_sample, f32 sample_ms, f32 sample_scale);

// The controller's parameters from the setting, `r.dynres`, `r.dynres.target_ms` and the tier (floor and ceiling =
// QualitySettings::render_scale). False (the controller does nothing, the frame is today's) when the setting is off, the cvar
// is off or any input is not finite. target_ms = the cvar when > 0 else the setting's (4..100); floor = max(setting, tier
// floor, 0.5); ceiling = min(setting max_scale, tier ceiling, 1) and at least 0.5.
bool dynres_resolve(const DynamicResolution& setting, bool cvar_on, f32 cvar_target_ms, f32 tier_floor, f32 tier_ceiling,
                    DynResParams& out);

// The test injection: "ms@frame,ms@frame,..." = the frame costs `ms` at scale 1 from `frame` on (up to 8 entries, frames
// strictly ascending, ms in (0, 1000], frames are non-negative integers; spaces around a token are allowed).
struct DynResInject {
    u32 count = 0;
    f32 ms[kDynResMaxInject] = {};
    u64 frame[kDynResMaxInject] = {};
};
// False and `out` empty for an empty or malformed string.
bool dynres_parse_inject(StringView text, DynResInject& out);
// The injected cost at scale 1 of frame `frame`: the last entry whose frame is <= it (the first entry before it); 0 for an empty schedule.
f32 dynres_inject_ms(const DynResInject& inject, u64 frame);

}  // namespace mce::render
