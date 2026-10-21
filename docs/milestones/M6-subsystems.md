# M6 — Subsystems: audio v0.5, input, physics2d v0.5 (milestone record)

**Scope:** the subsystem half of ROADMAP M6 (the ECS, editor, 2D-extras and content halves are tracked
separately). Design decisions: ADR-0026. **Measured on:** the Linux CI container (4 cores).

## Audio v0.5

| ROADMAP / FR item | Where | Evidence |
|---|---|---|
| 48 voices, steal without a click (FR-AUD-020) | `kMaxRealVoices`, `mixer.cpp` | `test_mixer_v05`: 49th voice steals the least important; max output step ≤ one voice |
| Sample-accurate `play_at` (FR-AUD-020, deferred from M3) | `AudioDevice::play_at`, `Mixer::mix_voice` | first output frame == requested frame for 480/37/1024-frame callbacks; late request starts mid-clip |
| Bus graph Master/Music/SFX/UI + user buses (FR-AUD-030) | `set_bus_parent`, `Mixer::fold_bus` | 3-level graph matches an offline reference mix within −60 dB (max error < 1e-3) |
| Per-bus biquad EQ (FR-AUD-060) | `biquad_coeffs`, `dsp.h` `BiquadRT` | cookbook magnitudes (−3 dB at fc, peak/shelf gains); impulse response == difference equation; swept sines == \|H(f)\| within 1 % |
| Look-ahead master limiter (FR-AUD-060) | `dsp.h` `LimiterRT`, `set_limiter` | +6 dB hot input never exceeds the −1 dBFS ceiling; quiet input bit-transparent; releases to unity; no click |
| 3D spatialization, listener = camera (FR-AUD-050) | `spatialize`, `set_listener`, `AudioListener` | gain follows the authored curve within 0.1 % as the listener retreats 0–150 units; bridge follows transforms |

The Freeverb send is not built (ADR-0026 §1).

## Input: gamepads and rebinding persistence

| ROADMAP / FR item | Where | Evidence |
|---|---|---|
| SDL gamepads enabled, hot-plug (FR-INP-020) | `platform/src/gamepad.cpp`, `Dependencies.cmake` | `test_platform`: a virtual pad hot-plugs into slot 0 and streams standard-layout button/axis events |
| Gamepad action maps (FR-INP-020) | `Device::GamepadButton/GamepadAxis`, `default_map.cpp` | stick + d-pad drive Move with deadzone and y-flip; South is Jump; any pad drives the action |
| Tick-quantized pad input (FR-INP-010) | `InputSystem` pad state | identical sim-visible streams at 30 Hz and 144 Hz render rates; one-tick visibility floor for taps |
| Capture rebinding (FR-INP-040) | `begin_rebind`, `rebind_result` | capturing press swallowed, applied next tick; Escape cancels; stick captured onto an axis keeps its sign |
| Rebinding persistence (FR-INP-040) | `bindings.h`, `save_bindings`/`load_bindings` | overrides round-trip through reflection and survive a restart; bad and stale files load as defaults |

## Physics2d v0.5

| ROADMAP / FR item | Where | Evidence |
|---|---|---|
| Distance joint: rod, rope, spring (FR-PHY-060) | `create_distance_joint` | `test_joints_sleep`: one solve gives the hand-computed common axial velocity with momentum conserved; pendulum keeps length and period 2π√(L/g); spring oscillates at its hertz; rope only pulls |
| Revolute joint: limit, motor (FR-PHY-060) | `create_revolute_joint` | pinned points share a velocity after the solve (hand-computed); limit holds; motor reaches its speed or its torque cap |
| Island sleeping | `enable_sleep`, `update_sleep` | sleeps after `time_to_sleep`, freezes, wakes on contact and API; removing the support wakes the island; sensors keep sleeping bodies inside |
| 20-body stack settles < 1 s, no jitter (FR-PHY-060) | `block_solver` + 16 iterations (stacking profile) | top box at 19.5 m ± 1 %, max speed < 0.05 m/s after 1 s, creep < 5 mm over the next 2 s, all 20 asleep |
| 2,000 bodies broadphase + integrate < 3 ms (FR-PHY-050) | M3 dynamic AABB tree | 0.83 ms per step in Release (budget asserted in optimized builds); a 2,000-body pile stays valid and deterministic |
| Existing behaviour untouched | defaults off | sleeping-off test; 03_platformer and physics2d_headless hashes unchanged; Box2D v3 oracle suite green |
