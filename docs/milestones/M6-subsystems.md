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
