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
