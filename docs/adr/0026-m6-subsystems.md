# ADR-0026 — M6 subsystems as built: audio v0.5, input persistence + gamepads, physics2d v0.5

**Status:** Accepted (2026-10-08). Sections are added as each subsystem merges.

**Context.** ROADMAP M6 carries the v0.5 depth of three subsystems whose MVP shape shipped in M3/M3.5:
`audio-input-ui.md` §3.3–§3.8 sketch the bus graph, DSP chain, `play_at` and spatialization; §2.1/§6.1 the
gamepad bindings and rebinding persistence (FR-INP-040); `physics.md` the joints, sleeping and the 2,000-body
budget (FR-PHY-050/060). Building them settled the points below. Each subsystem landed as its own PR.

## 1. Audio v0.5 (FR-AUD-020/030/050/060)

1. **48 real voices is a constant change.** `kMaxRealVoices = 48`, `kMaxVirtualVoices = 208` (handles stay 256).
   Stealing, virtual voices and promotion are the M3 code unchanged.
2. **The bus graph is a parent array with `parent < bus`.** Master/Music/SFX/UI plus user buses 4..11; every bus
   but Master feeds exactly one lower-id parent (default Master). That makes the graph acyclic by construction and
   lets the callback mix it in one pass: user buses fold into their parents in descending id order, then
   Music/SFX/UI fold into Master in ascending order — the MVP's exact summation order, so a default graph renders
   bit-identically to M3. No runtime topology sort, no allocation.
3. **EQ = 4 RBJ biquad bands per bus, coefficients computed on the main thread.** `set_bus_eq` turns a
   `BiquadDesc` (low/high-pass, peaking, low/high shelf) into normalized coefficients and ships them in one
   ≤ 32-byte command; the callback runs transposed direct form II per channel, applied after a bus's children are
   summed and before its gain. `Off` bypasses and resets the band.
4. **The master limiter is opt-in and overshoot-free by construction.** 5 ms look-ahead (240 frames): sliding-window
   minimum of the required gain (monotonic deque), exponential release, then a 240-tap box average applied to the
   input delayed 239 frames. Every value in the box window is a minimum over a window that contains the delayed
   sample, so the applied gain never exceeds what that sample needs, and gain reduction ramps across the
   look-ahead instead of stepping. It is off by default because it adds 239 frames (5 ms) of latency; games enable
   it once at init (`set_limiter`). The final hard clip stays as the last safety.
5. **`play_at` schedules on the mixer clock.** `Play` carries `start_sample` (the command grew to exactly
   32 bytes). A future start parks the voice in its slot and the callback starts it mid-block at the exact frame;
   a past start skips that far into an in-memory clip (streams start at once — their ring is primed from frame 0);
   stopping a voice before it starts ends it silently. `start_sample` enters the command hash only when non-zero,
   so immediate plays hash exactly as before and existing determinism hashes are unchanged.
6. **Spatialization is main-thread math at command time (§3.8).** `Listener` (position/forward/up/gain) and
   `AttenuationCurve` (Inverse/Linear/Exponential, clamped to [min, max] distance — the OpenAL clamped models,
   32 ids, 0 = 2D). Gain = curve × listener gain; pan = sin(azimuth) on the listener's right axis, faded toward
   centre inside `min_distance` so a source passing through the listener does not flip sides. The device keeps
   each 3D handle's base gain and position and re-sends one `VoiceGainPan` only when the result changes;
   `set_listener` re-spatializes every live 3D voice. No Doppler (recorded out of scope in §3.8).
7. **ECS surface.** `AudioListener` (first one in query order wins; orientation from its `LocalTransform`),
   `AudioSpatial { attenuation_id }` beside an `AudioSource` to make it 3D, and
   `PlaySfx::{attenuation_id, position}` for 3D one-shots. 3D is a separate component rather than a new
   `AudioSource` field so 2D sources keep their M3 byte layout — `World::state_hash` folds component bytes, and
   the 03_platformer determinism hash stays exactly what it was. The bridge reads `LocalTransform` positions in
   `FixedUpdate_Post`, so 3D commands are a deterministic function of sim state.

**Not built (recorded):** the Freeverb send of FR-AUD-060 (timeboxed out — R-cut), per-voice pitch, device
re-open on default change.

## 2. Input: gamepads and rebinding persistence (FR-INP-020/040)

1. **SDL gamepads are on, through the platform layer only.** `cmake/Dependencies.cmake` turns on
   `SDL_JOYSTICK`, `SDL_HIDAPI`, `SDL_HAPTIC` and `SDL_VIRTUAL_JOYSTICK`; `mce_platform` opens up to
   `kMaxGamepads = 4` pads into stable slots and turns SDL gamepad events into `GamepadAdded/Removed`,
   `GamepadButtonDown/Up` and `GamepadAxis` events (slot in `flags`, value in `x`, axes normalized to
   [-1, 1], triggers [0, 1]). Nothing above `mce_platform` includes SDL. A failed gamepad subsystem init only
   warns: keyboard and mouse keep working.
2. **Gamepad bindings read "any pad".** `Device::GamepadButton` / `GamepadAxis` join the binding table. A
   button is held while any connected pad holds it; an axis takes the largest deflection across pads. Per-player
   pad assignment waits for local multiplayer. Pads go through the same tick quantization as the keyboard
   (a tap inside one tick still shows for one tick), so the sim-visible stream is identical at any render rate.
3. **The default maps gained pad bindings without new actions.** Move = left stick (y flipped) + d-pad,
   Look = right stick, Jump = South, Fire = West, Pause = Start; UI navigation = d-pad + stick (deadzone 0.5),
   Confirm = South, Cancel = East. Trigger actions were left out on purpose: a new action changes the
   `InputState` layout and with it the 03_platformer determinism hash.
4. **Rebinding is a capture mode on the input system.** `begin_rebind(map, action, index, accept)` arms
   capture for one binding; the next press of an accepted device becomes that binding. The capturing press is
   swallowed (it never reaches the action that tick), Escape cancels, a stick needs 0.6 deflection, an axis
   binding keeps its direction sign, and the timeout counts sim time so replays behave the same.
5. **Overrides persist as a reflected `BindingOverride` list, not as a re-cooked map.** The cooked map stays
   the source of defaults; overrides re-apply on every `load_map`. The file is text
   (`# mce input bindings v1`, then `[BindingOverride]` blocks of `field = value` lines, enums by name),
   written by the reflection walker so new fields need no serializer change. `save_bindings` writes a temp file
   and renames it; unknown fields are skipped and overrides naming an unknown map or action are dropped with a
   warning, so a stale file never blocks boot. `reset_bindings` returns to the cooked defaults.

**Not built (recorded):** per-player pad ownership; the rebinding UI screen (lands with the UI work).
Rumble is a platform passthrough (`gamepad_rumble`) with no input-map surface yet.

## 3. Physics2d v0.5: joints, sleeping, stacking, 2,000 bodies (FR-PHY-050/060)

1. **Joints are the Box2D 2.x formulations.** `DistanceJointDesc` is a rigid rod by default, a rope with
   `min_length < max_length`, or a soft spring toward `length` with `hertz > 0` (stiffness and damping from the
   pair's effective mass, `b2LinearStiffness`). `RevoluteJointDesc` is the 2x2 point constraint with an optional
   angle limit and a motor capped by `max_motor_torque`. Joints solve before contacts in both the velocity and
   position passes, are warm started, and are addressed by a generational `JointId`; destroying a body destroys
   its joints. `collide_connected = false` (the default) filters the pair out of collision.
2. **Islands link through joints, and islands sleep (Box2D tolerances).** An island whose bodies all stayed
   under 0.01 m/s and 2 deg/s for `time_to_sleep` (0.5 s) stops being integrated, collided and solved. Its
   contacts and sensor overlaps carry over unchanged, so a sleeping stack keeps its warm-start impulses and fires
   no spurious exit events. Anything that touches it, a joint to an awake body, a moving kinematic body, or an
   API call (velocity, impulse, force, teleport, config change, destroy of a neighbour) wakes the whole island.
3. **Two-point manifolds get the Box2D 2.x block solver.** `block_solver` solves both normal impulses as one
   2x2 LCP by total enumeration, and drops the second point when the effective-mass matrix is ill-conditioned
   (condition number > 1000). Without it a 20-box column sinks half a metre and leans even at 30 iterations.
   With it, 8 velocity iterations still lean over within seconds (Gauss-Seidel does not converge down 20 bodies
   in 8 sweeps); 16 holds the column, settles it within 1 s and sleeps it. That pair (`block_solver = true`,
   `velocity_iterations = 16`) is the documented **stacking profile**.
4. **Everything new is opt-in, for the hashes.** `enable_sleep` and `block_solver` default to `false`, and the
   state hash folds sleep flags only with sleeping on and joints only when one exists. The M3/M3.5 solver path is
   therefore bit-identical: the 03_platformer and physics2d_headless hashes and the Box2D v3 oracle diffs are
   unchanged. New games should turn both on.
5. **The 2,000-body budget needed no new broadphase.** The M3 dynamic AABB tree with fat proxies already meets
   FR-PHY-050: 2,000 free-falling dynamic bodies step (broadphase + integrate) in 0.83 ms in Release, against the
   3 ms budget. Sleeping skips sleeping bodies in the pair query and the proxy refresh, so settled piles cost
   less still. A fully colliding 2,000-body pile (3,500 contacts) is a solver load, not a broadphase one, and is
   reported but not budgeted.

**Not built (recorded):** prismatic, wheel, weld and mouse joints; joint break forces; continuous collision
between dynamic bodies; a persistent contact graph (contacts are rebuilt each step from the pair list).
