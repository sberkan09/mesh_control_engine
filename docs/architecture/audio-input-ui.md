# Architecture: Audio, Input & Runtime UI (`mce_audio`, `mce_input`, `mce_ui`)

**Subsystem:** `audio-input-ui` · **Doc class:** Architecture · **Date:** 2026-10-06
**Status:** Design — implements FR-AUD, FR-INP, FR-UI of `docs/REQUIREMENTS.md`. MVP-tier features get full design; v0.5/v1.0 features are designed to seam depth and sketched.
**Binding context:** SDL3 is the only device touchpoint (closed dependency list); the mixer/voices/DSP, the action mapping layer, and the runtime UI are hand-written (from-scratch list); the runtime never parses JSON/XML/PNG/WAV (all audio cooked to `.mce`); no exceptions/RTTI; ImGui is editor-target-only behind the CMake wall (FR-UI-048); fixed 60 Hz sim with per-tick input sampling (FR-INP-030).

This subsystem is **three engines sharing one doc because they share one host process**, not because they share code: `mce_input` (deterministic per-tick action sampling), `mce_audio` (a real-time audio thread + mixer), and `mce_ui` (v1.0 retained runtime UI). The one structural rule that binds them: **the simulation reads input from a snapshot and speaks to audio through a command FIFO; neither path ever reaches back into the ECS World from another thread.**

Naming note: `core.md` §2.1 shows `input::ActionSnapshot` in `TickContext`. That type and `InputState` are the same object; the canonical name is **`mce::input::InputState`** (FR-DX-010 fixes it as one of the 12 named gameplay types), with `using ActionSnapshot = InputState;` kept as an alias so the core doc stays valid.

---

## 1. Responsibilities & non-responsibilities

### 1.1 This subsystem owns

**Input (`mce_input`):**
- The SDL3 event → engine-event translation and device-state snapshot layer (keyboard, mouse, text input; gamepads at v0.5). The only place `SDL_Event` exists outside `mce_platform`'s pump (FR-INP-010's CI grep has exactly one consumer: this file).
- The hand-written **action mapping layer**: named actions (`Button`/`Axis1D`/`Axis2D`), bindings, composite axes, per-binding deadzones and response curves, swappable action maps — authored as data, cooked to a `.mce` input-map chunk, editable and rebindable in the editor.
- **Tick-quantized input sampling** (the load-bearing decision, §3.2): per-tick `InputState` snapshots whose contents are a pure function of event timestamps against the fixed tick grid — independent of render rate (FR-INP-030).
- Input stream record/replay hooks for the determinism harness (FR-RUN-070) and editor input for play-in-editor (delegated, §4.6).
- Focus-loss state zeroing, device hot-plug handling, rumble (v0.5 Could).

**Audio (`mce_audio`):**
- The SDL3 **audio device callback** and everything above it: 48 kHz stereo float32 mixer, fixed voice pool with priority stealing + unlimited virtual voices (FR-AUD-010/020), bus graph Master/Music/SFX/UI (v0.5, FR-AUD-030), in-memory SFX + ring-buffer streamed music from mapped `.mce` (FR-AUD-040), 2D/3D spatialization (FR-AUD-050), DSP chain (biquads, Freeverb-style reverb, look-ahead limiter — v0.5, FR-AUD-060), sample-accurate scheduling.
- The real-time audio thread and its lock-free SPSC command FIFO; the RT discipline (no allocs/locks/ECS in the callback) is owned here.
- The `AudioSource` / `AudioListener` starter-kit components and the `audio_bridge` system that turns ECS state into audio commands at the tick sync point (FR-AUD-055).
- Cook-facing *contracts* (clip format, bus layout, attenuation curves) — the cook tool itself belongs to `assets`.

**Runtime UI (`mce_ui`, designed now, built v1.0 per FR-UI-020):**
- The retained-mode widget system for shipped games: anchored-rect layout, widget tree, input routing/focus, data binding, rendered through the 2D sprite batch path. MVP/v0.5 games use scene-state menus + text sprites instead (A9 resolution).
- The shipped debug console shell over core's convar registry (FR-UI-045).

### 1.2 This subsystem explicitly does NOT own

- **The event pump, window, and clock** — `mce_platform` pumps SDL events; this subsystem *subscribes* to them (core.md §2.5). We never call `SDL_PollEvent` ourselves.
- **The audio device negotiation details** (WASAPI COM, device hot-swap plumbing) — SDL3 owns it; we own the callback body only.
- **Decoding WAV/PNG or resampling at runtime** — `assets` cooks; the runtime reads cooked float32/ADPCM chunks. Our IMA-ADPCM *decoder* (~30 lines of table math) lives in the mixer because it is a mixing concern, not an asset-parsing one (it never sees a file).
- **ImGui and the editor UI** — `editor-tools` owns ImGui entirely; this subsystem only defines the event-consumption seam the editor plugs into (§4.6).
- **Text rendering / font atlases** — `rhi-renderer` bakes stb_truetype atlases at cook (FR-R2D-060); `mce_ui` consumes text quads.
- **The frame loop, phases, and tick gates** — `core` + `ecs-gameplay`. Our systems are ordinary phase members with no scheduler privileges.
- **Gameplay logic of any kind** — jump buffering, coyote time, menu navigation logic are game/scene-state code consuming `InputState`; the action layer stops at "typed action value + edge".
- **Music authoring, adaptive music, HRTF/convolution reverb, runtime codecs** — FR-AUD-080 Won't.

### 1.3 Design stance in one paragraph

Input and audio are the two subsystems where the fixed-timestep discipline is *felt* rather than declared: input must be quantized to the tick grid or the sim's determinism claim is false, and audio must be driven off a sample clock or every one-shot lands ±16 ms late. Both are therefore designed as **asynchronous boundaries with one-way command/data flow**: the sim reads an immutable per-tick `InputState` and pushes POD `AudioCommand`s into a FIFO; the SDL audio thread consumes commands and mixes; the IO worker pre-fills stream pages. `mce_ui` is the deferred third member — committed now as a design contract (retained tree on the sprite batcher, ImGui never in the runtime) so nothing accidentally couples to ImGui, with its full design doc at M4 per the requirements' own sequencing.

---

## 2. Public interface

### 2.1 Input — action maps, sampling, and the per-tick snapshot

```cpp
// mce_input/actions.h  (L2 — the only header gameplay code includes)
namespace mce::input {

enum class ActionType : uint8_t { Button, Axis1D, Axis2D };

struct ActionState {                  // one action's value this tick
    float   x = 0.f;                  // Button: 0|1 (held). Axis1D: x. Axis2D: x,y
    float   y = 0.f;
    bool    pressed  = false;         // edge: became held THIS tick (was up at last tick)
    bool    released = false;         // edge: became released THIS tick
    float   held_time = 0.f;          // seconds held as of this tick (sim-time)
};

// The per-tick gameplay-facing snapshot. Registered as a World resource and also
// carried in app::TickContext so non-ECS samples use the identical object.
// Immutable after sampling; determinism harness replays a stream of these.
struct InputState {
    uint64_t           tick;                    // sim tick this snapshot is bound to
    // Fixed slots for the starter kit's 12-type budget; named actions below.
    float2             move;                    // "Move"   (WASD / left stick)
    float2             look;                    // "Look"   (mouse delta / right stick)
    float              trigger_l, trigger_r;    // v0.5 analog
    bool               jump_held, jump_pressed, jump_released;   // "Jump"
    bool               fire_pressed;            // "Fire"
    bool               pause_pressed;           // "Pause" (menu states consume this)
    bool               confirm_pressed, cancel_pressed;          // UI navigation (mce_ui)
    // Access by action NAME (editor-driven games, remap-proof): hash lookup over
    // the cooked ActionMap. The fixed fields above are fast paths, not a second system.
    const ActionState& action(StringId name) const;
    // Convenience readers over the same cooked ActionMap (finding 24) — what the
    // §2.7 sketches actually call. `held` == ActionState::held; `pressed` is the
    // per-tick edge; `axis` returns the mapped curve value.
    bool  pressed(StringId name) const;      // true only on the tick the action fired
    bool  held(StringId name) const;
    float axis(StringId name) const;         // 0..1 (or -1..1 for 2D-into-1D axes)
    // Text (per-tick accumulated UTF-8, NUL-terminated; emptied each tick)
    Span<const char>   text() const;            // FR-INP-010 text input
    float2             mouse_pos_px() const;    // window-space; UI + editor use
    float2             mouse_wheel() const;
    // Device introspection (debug/overlay only — gameplay binds actions, never devices)
    bool               gamepad_connected(uint32_t slot) const;   // v0.5
};

// The mapping service. One instance per Application; registered as a world resource.
class InputSystem {
public:
    // Load the cooked input-map chunk (assets provides the mmap'd chunk payload).
    Result<void, Error> load_map(Span<const byte> mce_chunk);
    void                push_map(StringId map_name);    // swap overlay map (UI vs Gameplay)
    void                pop_map();
    // Runtime rebinding (FR-INP-040, v0.5): rebind returns the captured binding
    // after the next matching device event (options-screen flow), or times out.
    void                begin_rebind(StringId action, RebindCallback cb, float timeout_s);
    Result<void, Error> serialize_bindings(SaveWriter&) const;    // reflection-serialized
    Result<void, Error> deserialize_bindings(SaveReader&);        // SaveWriter/SaveReader: core §2.9

    // --- engine-internal; called by the RUN loop, not by gameplay ---
    void                begin_frame(const platform::Event* events, size_t n, u64 now_us);
    const InputState&   sample_for_tick(uint64_t tick, u64 tick_close_us);  // §3.2
    void                record_tick(const InputState&);   // replay capture (dev)
    void                replay_tick(uint64_t tick, InputState& out);      // replay feed
};

} // namespace mce::input
```

Gameplay consumption (the whole contract — note there is no way to see a device):

```cpp
MCE_SYSTEM(player_control, mce::Phase::FixedUpdate_Pre, after("input_apply"))
void player_control(Frame& f) {
    const input::InputState& in = f.resource<input::InputState>();
    f.query<LocalTransform&, Velocity&, PlayerTag&>()
     .each([&](Entity, LocalTransform& t, Velocity& v, PlayerTag&) {
        v.v.x = in.move.x * RUN_SPEED;                      // curved, deadzoned axis
        if (in.jump_pressed) f.events().push(RequestJump{ .who = ??? });  // see §2.4
    });
}
```

### 2.2 Input — device/text surface for the editor and UI layers

```cpp
// mce_input/devices.h  (L1 — consumed by editor-tools and mce_ui only, not gameplay)
namespace mce::input {
    // Raw (unmapped) state for tools that genuinely need it: editor camera,
    // ImGui text fields' IME composition rects, shipped console text entry.
    bool  key_held(KeyCode k)  const;         // post-focus-loss-zeroing state
    bool  mouse_held(MouseButton b) const;
    // Per-render-frame text queue (NOT tick-quantized — typing is not sim state).
    // Editor's ImGui owns its own IME path via ImGui_ImplSDL3; this is for
    // mce::ui's console (FR-UI-045) and future runtime text entry.
    Span<const TextEvent> frame_text() const;
}
```

### 2.3 Audio — main-thread API (commands only, never blocks)

```cpp
// mce_audio/audio.h  (L2 — the gameplay-facing surface is AudioHandle, per FR-DX-010)
namespace mce::audio {

using ClipHandle  = Handle<class AudioClip>;   // generational; resolved by `assets`
using AudioHandle = Handle<class Voice>;       // gameplay-visible voice handle
using BusId      = uint8_t;                    // 0=Master 1=Music 2=SFX 3=UI 4..11=user

struct PlayParams {
    float    gain_db    = 0.f;                  // on top of clip + bus gain
    float    pitch      = 1.f;                  // resample-ratio pitch (v0.5 variance)
    float    pan        = 0.f;                  // -1..1 (2D); 3D spatialization overrides
    bool     loop       = false;
    uint8_t  priority   = 128;                  // 0=highest; steal threshold (§3.5)
    uint16_t attenuation_id = 0;                  // cooked curve id (3D, v0.5)
    BusId    bus        = Bus_SFX;
};

class AudioDevice {   // one per Application; created after `assets`, before rhi (core §4.4)
public:
    static Result<UniquePtr<AudioDevice>, Error> create(const AudioDesc& desc);

    // --- clip lifecycle (assets hot-reload calls these; clips are cooked chunks) ---
    Result<ClipHandle, Error> register_clip(const ClipPayload& cooked);   // in-memory SFX
    Result<ClipHandle, Error> register_stream(StringId chunk_id);         // streamed music
    void                      release_clip(ClipHandle h);

    // --- playback (all enqueue POD commands; sub-frame scheduling to the sample) ---
    AudioHandle play(ClipHandle c, const PlayParams& p = {});             // one-shot or loop
    AudioHandle play_at(ClipHandle c, u64 mix_sample, const PlayParams&);// sample-accurate
    void        stop(AudioHandle v, float fade_s = 0.f);                  // graceful, no click
    void        set_handle_gain(AudioHandle v, float db);                 // ramps internally

    // --- mixer surface (options UI + editor mixer panel wire to this) ---
    void        set_bus_gain(BusId bus, float db);      // smooth-ramped; convar-bound
    void        set_listener(const float3& pos, const float3& vel, const Quat& orient);
    // DSP (v0.5): nodes are plain structs registered on a bus chain at world init
    void        attach_dsp(BusId bus, const DspNode& node);   // Reverb{roomSize,damping,wet}, Biquad{...}, Limiter{...}

    // --- diagnostics (dev overlay + editor, FR-DBG-060) ---
    struct Stats { uint32_t voices_real, voices_virtual, steals_this_frame;
                   uint32_t device_recoveries;   // device-loss/default-change re-opens (FR-DBG-060)
                   float    master_peak_db; u64 mix_sample; };
    Stats       stats() const;                    // readback via SPSC reply queue, never locks
};

// Convars registered by the subsystem (FR-DBG-040: same-frame effect through the FIFO):
//   audio.master_db, audio.music_db, audio.sfx_db, audio.ui_db   (Saved flag → options)
//   audio.device           (device selection; requires restart)
//   audio.debug_voices     (dev overlay voice list)

} // namespace mce::audio
```

### 2.4 Audio + input — the ECS surface (starter kit additions)

```cpp
// mce_audio/components.h — reflected plain structs (inspector + serialization free)
MCE_COMPONENT(AudioSource,
    assets::Guid      clip;            // resolves to ClipHandle at scene load (hook)
    audio::BusId      bus   = audio::Bus_SFX;
    float             gain_db = 0.f;
    float             pitch   = 1.f;
    bool              loop    = false;
    bool              play_on_start = false;
    uint16_t          attenuation_id = 0;   // 0 = 2D (pan only)
    // engine-written, not serialized:
    audio::AudioHandle handle{};            // live voice (looping ambient sources)
)

MCE_COMPONENT(AudioListener,   // exactly one per World; camera hook asserts it
    float   gain_db = 0.f;
)

// The bridge system: the ONLY code that turns ECS state into AudioCommands.
// Runs FixedUpdate_Post (after physics/transform propagation — positions are final),
// drains event queues for one-shots, emits into the FIFO at the sync point.
MCE_SYSTEM(audio_bridge, mce::Phase::FixedUpdate_Post, after("transform_propagation"))
void audio_bridge(Frame& f) {
    AudioDevice& dev = f.resource<AudioDevice>();
    for (const AnimFrameEvent& ev : f.events().read<AnimFrameEvent>())   // FR-AUD-055
        if (ev.sound) dev.play(ev.sound, {.gain_db = ev.gain_db});       // footsteps ±1 tick
    // ... looping AudioSource start/stop/retarget from Changed<LocalTransform> ...
}
```

Trigger/one-shot vocabulary (producer systems → audio): `AnimFrameEvent` (rhi-renderer's animated-sprite system), `DamageDealt`, `EntityDestroyed`, `TriggerEnter` (physics §2.3 — the committed event name), and the presentation-side pairs `PlaySfx` and `SpawnParticles` (module systems → audio bridge / ecs-gameplay particles system — see scripting §2.7's `player_react`). Audio never subscribes to gameplay internals — it consumes registered events, which keeps FR-UI-030's "gameplay never reaches into [presentation]" symmetric in the other direction.

### 2.5 Runtime UI (`mce_ui`, v1.0 — design-committed, sketch depth per FR-UI-020)

> **M4:** the full design is [ui.md](ui.md); this section is its seed and stays for the decisions' history.

```cpp
// mce_ui/ui.h — retained widget tree; rendered through rhi-renderer's 2D MDI batch.
namespace mce::ui {

struct RectAnchor { Vec2 anchor_min, anchor_max, offset_min, offset_max; }; // uGUI model

class Widget {  // polymorphic by small enum + params union, not virtual-per-widget hot path
public:
    WidgetId id; Entity owner;                 // optional ECS bind target
    RectAnchor anchor; float2 pivot, size;
    uint8_t    layer;                          // draw + input layer (painter's order)
    bool       interactable, focused;
    virtual void layout(const Rect& parent) = 0;          // anchored-rect resolve
    virtual void submit(DrawList2D&) const = 0;           // -> sprite batch quads
    virtual Widget* hit_test(float2 px) = 0;              // tree traversal, topmost
};

// Builder API (v1.0; widgets defined in C++, no cooked UI format until an artist needs it)
UiBuilder ui(root);
ui.panel("pause_bg").anchor_fill().color({0,0,0,0.6f});
ui.button("Resume").anchor(.5f,.4f).text("RESUME").on_confirm([](Frame& f){ f.app().states().pop(); });
ui.slider("Volume", bind_convar("audio.master_db")).range(-60.f, 0.f);

// Input: actions only, with focus navigation (FR-UI-020 acceptance: gamepad menus work).
// mce_ui reads InputState::confirm_pressed / cancel_pressed / "UI_Navigate" from a
// pushed "UI" action map; focus moves along an authored/implicit navigation graph.
// Data binding (FR-UI-030): read-only property paths via reflection + events.
ui.health_bar().bind_path("player/Health/hp", /*max*/ 100.f);
}
```

The binding that this sketch exists to honor: **no ImGui symbol reaches a shipped runtime** (CMake wall + CI symbol scan, FR-UI-048), and **UI observes gameplay; gameplay never imports `mce_ui`** (FR-UI-030). Full widget set and design doc land at M4; the *decision* (retained tree, sprite-batch path, action-driven focus, events-only bridge) is recorded now so nothing pre-couples.

---

## 3. Internal design: data structures & algorithms

### 3.1 Input layering (three layers, one direction)

```
SDL3 events ──▶ [L0 platform::pump_events] ──▶ [L1 mce_input: DeviceState + event ring]
                                                       │ begin_frame(events, now_us)
                                                       ▼
                                     tick-quantized fold (§3.2, the algorithm)
                                                       │ sample_for_tick(tick)
                                                       ▼
                                     [L2 action mapping] ──▶ immutable InputState
                                                       │                     ▲
                                                       └── record/replay ────┘
```

- **L1 device layer:** per-device state structs (`KeyboardState` — 32-byte bitset per key block + modifiers; `MouseState` — pos, wheel, button bitset, per-frame accumulated *delta* (pointer motion is inherently frame-shaped and non-deterministic; it is consumed only by camera/UI code that is explicitly out of the determinism contract — see §3.2's note); `GamepadState` — SDL standard layout axes/buttons, v0.5). Text input accumulates SDL3 `SDL_EVENT_TEXT_INPUT` strings (IME composition is delegated: ImGui handles its own in the editor; `mce_ui` console gets composed strings, not keycodes).
- **L2 action layer:** the cooked `InputMapChunk` is a flat table (POD, mmap'd straight from `.mce`):

```cpp
struct ActionDef   { StringId name; ActionType type; u16 first_binding, binding_count;
                     uint8_t compose; /* Or|Average|MaxMagnitude */ };
struct Binding     { DeviceType device; u16 code; float scale; uint8_t curve_id;
                     Deadzone dz; bool invert; };    // 12 bytes, SoA-friendly
struct ActionMapDef{ StringId name; u16 first_action, action_count; };
```

  At load, `InputState`'s fixed fields (`move`, `jump_held`, …) are **bound by name at init** (`bind_fixed("Move", &InputState::move)`), so the default map's hot path is struct-field writes, not hash lookups; named `action()` lookup is the fallback for game-defined actions. This keeps the 12-type budget honest: gameplay uses fields for the universal six, names for the rest.

### 3.2 Input sampling — the tick-quantization algorithm (FR-INP-030)

The requirement: synthetic events at 30/90/240 Hz render loops produce **identical sim-visible sequences**. Event-to-frame assignment can never satisfy this — it varies with render pacing. The fix is to make a tick's input a function of **event timestamps quantized to the fixed tick grid**, not of frame boundaries:

1. The RUN loop's accumulator and the input layer share one clock source (`platform::now_us()`). Tick `N` owns the half-open time window `[N·T, (N+1)·T)`, `T = 1/60 s`. There is **no integer `T_us`** — 1/60 s is 16666.67 µs — so window boundaries are computed in the rational form `tick_window_end_us(N) = (N * 1'000'000) / 60` (u64 multiply-then-divide; exact per-tick boundaries, no float drift, no accumulation error — the same arithmetic core's accumulator uses, shared by construction).
2. `begin_frame` appends each SDL event with `sort_key = now_us()` into a small ring (`FixedArray<InputEvent, 1024>`, frame-allocated; overflow → oldest-dropped counter + one console error, never a stall).
3. `sample_for_tick(N)` folds, **in timestamp order**, every queued event with `ts < tick_window_end_us(N)` — including events that arrived while earlier catch-up ticks were running (they carry timestamps inside tick `N`'s window and are correctly attributed). Events with later timestamps stay queued for tick `N+1`. Edge state (`jump_pressed`) is derived by comparing the folded device state to the *previous tick's* folded state — never from raw event arrival.
4. Catch-up clamp interaction: if the 5-tick spiral clamp drops ticks (it drops *time*, not ticks — core.md §2.1 clamps the accumulator), every surviving tick still folds its full timestamp window, so no press is ever lost; two presses inside one dropped window appear as one held tick, which is the documented, replay-consistent behavior (the recorded stream is the artifact of record).
5. **Replay (FR-RUN-070):** recording stores the *post-quantization* `InputState` stream (fixed fields + name-bound action values + text, ~40–80 B/tick at MCE scale; a 64 MiB ring ⇒ ~3.9–7.8 hours at 60 Hz, inside NFR-M-04's replay ring budget). Replay feeds `sample_for_tick` from the recording, so the sim cannot tell live from replayed input. Live timestamp quantization is what makes the FR-INP-030 test pass; the recorded stream is what makes FR-RUN-040's hashes hold across a replay.

Non-determinism note, stated honestly: raw mouse *motion* deltas are render-rate-dependent no matter what. They are excluded from the determinism contract by construction — the determinism harness runs headless with a scripted `InputState` stream (FR-RUN-025/040), and gameplay systems that consume `look`/mouse deltas (editor camera, orbit camera) are render-phase or explicitly replay-approximated (deltas zeroed in replay mode, actions-only). Keyboard/button actions — the things that matter for FR-RUN-040 — are fully covered.

### 3.3 Audio — the mixer graph and the RT contract

```
  gameplay/main thread                          SDL audio thread (RT)
┌──────────────────────────┐      SPSC FIFO   ┌───────────────────────────────────┐
│ AudioSource / events     │ ──AudioCommand[]──▶ drain FIFO (bounded, fixed count/cb)  │
│ audio_bridge (FixedUpdate│                   │ for each active real voice:       │
│  _Post, at sync point)   │                   │   start/stop/param-ramp (sample-  │
│ AudioDevice::play(...)   │                   │   accurate via mixSample clock)   │
└──────────┬───────────────┘                   │   mix clip → (gain,pan) → bus     │
           │ ClipHandle from assets            ├───────────────────────────────────┤
           ▼                                   │ per-bus DSP chain (v0.5)          │
   ClipRegistry (main)                         │ master limiter → float mixdown    │
   ClipHandle → {in-mem float32 | stream page}─▶ read clip data (no locks: pages are │
                                                │ pre-published, see §3.6)          │
                                                └───────────────────────────────────┘
```

- **Format contract:** internal mix bus is 48 kHz stereo float32. `assets` cooks every clip to 48 kHz float32 (mono clips stored mono + upmixed at mix — one `L+R = v` step); IMA-ADPCM is a cook-time *encoding* whose 30-line table-math decoder runs (a) at register time for in-memory SFX, or (b) in the IO worker's page-filler for streams — **never in the callback** and never on the main thread's hot path. Voices never resample at runtime (research §2.3's rationale: cook normalization deletes the problem).
- **Voice pool:** `Pool<Voice, 48>` (8 at MVP — a constexpr, tiered per A7) with an intrusive free list; a Voice is a fixed 96-byte POD: `{ClipHandle clip, u64 start_sample, u64 cursor_frames, f32 gain_lin, target_gain, pan, pitch_ratio, u8 priority, u8 flags(loop/stream/stealing), BusId bus, VoiceHandle user_handle}`. Virtual voices are an intrusive singly-linked list of `{AudioHandle, clip, params}` with no mixing cost — when a real voice frees, the highest-priority *audible* virtual voice is promoted.
- **Command FIFO:** single-producer (main), single-consumer (audio cb), bounded `RingBuffer<AudioCommand, 1024>` (core's SPSC container, unit-tested with sequence numbers — core.md §6.3). `AudioCommand` is a ≤ 32-byte POD tagged union (`Play{clip, params, handle, start_sample}`, `Stop{handle, fade}`, `Gain{handle, db}`, `BusGain{bus, db}`, `Listener{pos,vel,orient}`, `AttachDsp{...}`). A 1024-deep FIFO at 60 Hz tolerates ~17 commands per tick before overflow; overflow policy: play commands are *never* dropped silently — `play()` returns a null handle + one-shot `MCE_LOG_WARN` (per-frame summary), stop/gain commands are best-effort. Reply path (stats, underrun counters) is a second tiny SPSC back-channel.
- **Click-free parameter changes:** all gain/pan writes are **per-callback linear ramps** toward the target (ramp length = callback block, 480 frames ≈ 10 ms). No atomics needed for per-voice params — they arrive only via the FIFO, which is the synchronization. Bus gains and the listener are the only cross-thread atomics (`std::atomic<float>`/`atomic<u32>` snapshot pair), read once per callback into the block's local frame.
- **The mix loop:** per callback (target 480 frames, SDL may deliver more/less — process whatever arrives, the algorithm is frame-count-generic): drain up to 64 commands → for each real voice compute its window into the clip (start-offset math against the authoritative `mixSample` counter, §3.4) → accumulate into a stereo block with constant-power pan → (v0.5) per-bus DSP chains → master look-ahead limiter → write the device buffer. Zero heap, zero ECS, zero locks — asserted by a debug-build allocation hook installed on the audio thread (§6.2).

### 3.4 Sample-accurate scheduling (Gaffer On Games' audio-scheduling point)

The mixer owns the one authoritative clock: `mixSample` (u64, frames since device open, advanced by exactly the frames mixed each callback). A `Play` command carries `start_sample = dev.stats().mix_sample + lead_frames` where the main thread computes `lead` from its own estimate of callback latency (~1 block). The callback, on voice start, computes `offset = start_sample - mix_sample_at_block_start`; positive offset ⇒ skip into the clip (or silence-wait); negative ⇒ start mid-clip (late request, documented degradation). Sub-frame remainders carry into the next callback via the voice's fractional cursor, so 60 Hz command granularity still honors sample timestamps — a footstep triggered on tick N lands within one 480-frame block, not ±16 ms. Music/beat sync reads the same counter. The FR-AUD-020 "sub-frame scheduling honors timestamps to the sample" test drives this path with a synthetic clock.

### 3.5 Voice stealing (FR-AUD-020)

On `play()` with no free real voice: candidate set = all real voices, scored by `{priority (higher steals first), then current linear gain (quieter steals first), then age (older steals first)}` — a linear scan of ≤ 48 entries, trivially deterministic. A stolen voice gets a 5 ms fade-out and its handle's generation is bumped (the caller's `AudioHandle` fails `is_alive` cleanly — generational-handle semantics, never aliasing). The stolen voice's user-visible fact surfaces as an `AudioVoiceStolen` counter in `Stats`, not an exception. Looping music on `Bus_Music` carries `priority 0` + a "never steal below priority 32" flag so UI/SFX storms cannot kill the soundtrack.

### 3.6 Streaming music (FR-AUD-040, gapless requirement)

- A streamed `ClipHandle` maps to an `.mce` audio chunk already mmap'd by `assets` (the runtime never parses — the chunk is raw PCM frames + a header the mixer reads as POD).
- **Page ring:** 4 pages × 4,800 frames (100 ms each) of float32 stereo (~38 KB/page, ~154 KB per stream's ring; 2 concurrent streams max at MVP — streaming memory is trivially inside the audio-arena budget in ARCHITECTURE §6.2). The **IO worker** (core.md's thread roster, one thread, no GL/ECS) fills pages: on `register_stream` it primes page 0–1, then keeps `fill_cursor` ahead of the play cursor by ≥ 2 pages; page completion is published with a `std::atomic<u32> filled_upto` sequence — the callback reads only pages below `filled_upto` (single-writer/lock-free-read; no mutex on the RT side).
- **Gapless loops:** the page-filler knows the clip's `loop_point_frames`; when the play cursor crosses it, the next fetch continues from `loop_start` *within the same fill stream* — no re-seek, no device-side seam. The callback's voice cursor math is pure sample arithmetic, so the loop is bit-exact across the boundary by construction (verified by the waveform-comparison test, §6.3).
- **Underrun policy:** if the callback needs a page the IO worker hasn't filled (disk stall), it mixes silence for that page, bumps an underrun counter, and *keeps the voice* (re-syncs at the next filled page) — a one-page glitch beats a stop-click loop. A sustained-underrun convar-triggered warning surfaces in the dev overlay. The 10-minute soak (FR-AUD-040 acceptance) asserts zero underruns at sample-project sizes (music chunk fully resident-in-page-cache in practice).

### 3.7 DSP chain (v0.5, FR-AUD-060 — sketch depth per tier)

All nodes are POD param structs + one process function, scalar-first, SIMD pair only if Tracy shows cost (math library's hotspot pattern):

- **Biquad EQ** per bus — RBJ Audio-EQ-Cookbook coefficient formulas (low-shelf/high-shelf/peaking); per-voice low-pass for occlusion reuses the same biquad.
- **Freeverb-style reverb** — 8 comb + 4 allpass per channel, the Schroeder-Moorer topology; as a *send* from SFX/Music buses to a shared wet bus (stereo spread via the classic tuned delay set).
- **Look-ahead limiter** on master — 5 ms lookahead delay line + peak/rms detector + gain computer; the click-prevention workhorse that makes cut-ladder rung 4 ("voices → master limiter → device") a *degradation*, not a quality cliff.
- Each node: `void process(float* L, float* R, size_t frames, const Params&) noexcept` over the bus's block buffer; chains are fixed at world init (no runtime graph mutation → no RT reallocation).

### 3.8 Spatialization (FR-AUD-050)

Computed **once at command time on the main thread** (not per sample): given listener + source world positions, produce `{gain_lin (attenuation curve id), pan (constant-power, listener-relative angle)}` and ship them inside the `Play` command; a moving source re-sends gain/pan at most every tick via `Gain` commands from the bridge (ramped — no zippers). 2D sounds skip the listener entirely (`attenuation_id == 0` → pan-only). Velocity-based Doppler is out of scope (FR-AUD-080 adjacent; recorded, not built).

### 3.9 Runtime UI internals (v1.0 sketch — full doc at M4)

> Superseded in detail by [ui.md](ui.md) §3–§5.

Retained tree of `Widget` nodes in a flat pool (stable `WidgetId`, parent/first-child/next-sibling indices — same shape as the ECS hierarchy index). Per render frame in `Render_Pre`: (1) resolve anchors parent→child (one linear pass in depth order); (2) dirty-flag re-layout of changed subtrees (Nystrom); (3) hit-test input actions → focus set → widget state machines (button hover/press/confirm); (4) `DrawList2D` append: quads into the same atlas/9-slice/text pipelines the sprite batch uses — **no second renderer, one extra MDI batch at most** (FR-UI-020 acceptance is "renders through the sprite batcher"). Gamepad focus = an explicit navigation graph (authored edges, auto-generated fallback by geometry). Data binding = reflection property path + poll in `Render_Pre` (read-only; gameplay never imports `mce_ui`, FR-UI-030).

---

## 4. Threading & lifecycle

### 4.1 Threads (positions in core.md's roster, restated as contracts)

| Thread | Role in this subsystem | Hard rules |
|---|---|---|
| **Main** | input L1 accumulation + tick quantization + action eval; all `AudioDevice` main-thread API; ECS bridge system; (v1.0) `mce_ui` layout/submit | Never touches SDL audio state directly; never blocks on the audio thread |
| **Audio (SDL cb)** | drain FIFO, mix voices/buses/DSP, limiter, deliver | No heap, no locks, no ECS, no syscalls beyond the device write (Doumler RT rules); param changes only via FIFO/atomics |
| **IO worker** (platform-owned) | stream page fills from mmap'd `.mce`; ADPCM decode; text-atlas uploads are *not* here (renderer owns those) | Never touches mixer state directly — publishes pages via `filled_upto` atomic only |

Steady-state thread count stays ≤ 5 (core.md §4.1); this subsystem adds **zero** threads beyond SDL's own audio callback and its share of the existing IO worker.

### 4.2 Input lifecycle through the frame

```
render frame begin
  platform::pump_events()  → Window::event_handler → input.begin_frame(events, now_us)
      (editor builds: ImGui_ImplSDL3 processes the SAME events first and may mark
       keyboard/text consumed — see §4.6)
sim catch-up loop (0..5):
  input.sample_for_tick(tick) → InputState (frame-allocated, immutable, hashed in dev)
  TickContext{... .actions} → FixedUpdate_Pre systems read f.resource<InputState>()
render phase:
  mce_ui (v1.0) reads frame_text()/mouse (NOT sim-quantized — UI is presentation)
  editor reads raw device state for its own camera/tooling shortcuts
```

`InputState` is a per-tick value object (POD, frame-arena): systems hold a `const&` valid for the tick only. The name-binding of fixed fields happens at map load; a map reload (editor hot-reload, FR-AST-090) re-binds and resets held state (one-frame input grace is acceptable and asserted in the reload test).

### 4.3 Audio lifecycle

```
init  (after assets — clips register from cooked chunks; before rhi per core §4.4,
       because device-open failure must degrade to silence, not block the editor):
  SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, 48000, float32,
                            stereo, cb=MixerCallback)
  → cb failure = Result error + `audio.enabled=false` convar path (game runs silent —
    FR-DX-050 failure taxonomy: silence + one error, never a crash)
device policy: init opens SDL3's default playback device (no explicit device
  enumeration at MVP). SDL3 posts SDL_EVENT_AUDIO_DEVICE_REMOVED and
  SDL_EVENT_AUDIO_DEVICE_DEFAULT_CHANGED; when either affects our stream, the mixer
  re-opens on SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK — the one re-open policy for both
  unplug and default-switch (v0.5: drain current commands first, then swap — voices
  on the old device fade 50 ms and restart on the new one; until v0.5 the re-open is
  immediate with the same one-shot drain). A failed re-open falls back to the silence
  path above, never a crash; every successful re-open bumps Stats.device_recoveries,
  surfaced in the FR-DBG-060 overlay so the recovery is visible, not silent.
runtime: main thread enqueues; cb consumes; IO worker prefetches pages
scene load:  register_clip per referenced clip (assets resolves GUID → chunk → payload)
scene unload: on_remove(AudioSource) hook → stop(handle) (graceful 50 ms fade);
              stream pages released, ClipHandles generational — stale handle = silent no-op
shutdown: stop-all command → 100 ms drain → SDL_CloseDevice → verify cb exited
          (SDL guarantees the callback is not active once the stream is closed; we
          assert the FIFO drain count matches) → clip registry teardown
```

Voice-leak assertion (mirrors FR-RUN-030's entity-leak assert): at world unload, debug builds assert `voices_real + voices_virtual` returned to baseline; a leaked `AudioHandle` is a bug caught here, not a stuck sound in a shipped build.

### 4.4 Pause / focus / time-scale interplay

- **Focus loss (FR-RUN-060):** platform emits `FocusOut` → input zeroes all held state and drops the pending event ring (stuck-run-key fix, research §3.2.6); the RUN layer pauses the accumulator. Audio *keeps playing* menu music but the bridge stops issuing new one-shots (no sim ticks run); on focus regain the mix resumes from a clean state — no stuck voices because commands were the only side channel.
- **Pause (time_scale = 0):** sim ticks stop → bridge issues no commands → SFX stop naturally at loop boundaries; music bus keeps streaming (a paused game with silent music is a worse product; this is the Unity/Godot convention and is a per-game choice via a `pause_behavior` flag on the music clip).
- **Slow-mo (FR-RUN-050):** no special audio path — pitch/gain sweetness is a v0.5 Could (FR-AUD-070); correctness-wise the FIFO simply sees fewer commands.

### 4.5 Editor vs shipped lifecycle differences

| | Shipped runtime | Editor / devhost |
|---|---|---|
| Input | full L1+L2; action maps from cooked chunk | same layer; **editor input first**: ImGui backend consumes by default; game-view widget focus routes actions to the play world (§4.6) |
| Audio | full mixer | full mixer + mixer-panel convar surface; play-mode preview world gets its own `AudioDevice` instance? **No — one device, two worlds never mix simultaneously** (editor preview world is paused while play world runs; A11 model) |
| Text input | none at MVP (console at v1.0 via `mce_ui`) | SDL text events → ImGui fields directly |
| Dev overlay | FR-DBG-060 (F1: voices, master peak) | editor audio panel: live bus gains, voice list, steal counters (Tracy-backed) |

### 4.6 The editor input seam (the one genuinely tricky interaction)

ImGui_ImplSDL3 must see events before the engine's action layer, or editor typing leaks into the game (pressing `W` in a rename field moves the player). Contract: in editor targets, `platform::event_handler` first calls the ImGui backend (`ImGui_ImplSDL3_ProcessEvent`), then consults `ImGui::GetIO().WantCaptureKeyboard/WantCaptureMouse`, and tags events `consumed_by_editor` before they reach `input.begin_frame`. When the **game view widget has focus** (clicked into, play mode active), the editor releases capture for gameplay-relevant events (keyboard + gamepad; mouse stays captured by the viewport widget which forwards position). Shipped runtimes have no ImGui and no such step — one code path, one optional consumer in front (P6: one path per concern; the editor is a *client* of the pump, not a second pump). This seam is unit-tested with synthetic WantCapture toggles (§6.1).

### 4.7 Headless mode

`AppDesc::headless` (FR-RUN-025): no window, no GL — and **no audio device** (the mixer is presentation). The determinism harness instead runs `input.record/replay` against a scripted `InputState` stream and asserts the FIFO command *sequence* is identical between headless and live runs (audio is excluded from state hashes — it is a side effect, not sim state — but the *commands* are sim-deterministic, so they are hashed; this is the cheap way to keep audio honest inside the determinism harness).

---

## 5. Upstream/downstream dependencies

### 5.1 What this subsystem consumes (upstream)

| Provider | What | Notes |
|---|---|---|
| `core` | `RingBuffer` (command FIFO, unit-tested SPSC), `Pool<Voice>`, `Handle<T>`/`HandleTable`, `Result<T,Error>` (`Errc` range 0x0400 audio), convar registry (bus gains as Saved convars), frame allocator (InputState, event ring, DSP block buffers), `StringId`, Tracy zones | No `mem::heap()` reachable from mixer code — enforced by the no-heap audit hook |
| `platform` | SDL event pump subscription (we never pump), SDL audio device open/close, `spawn_thread` (page prefetch rides the existing IO worker), `now_us` clock (shared with the accumulator — the tick-quantization contract), mmap (`assets` maps; we read the payload) | SDL3 symbols confined to `mce_platform` + our single device-open call site (agreed seam, CI-greppable like the Window allowlist) |
| `assets` | cooked `.mce` chunks: audio clips (48 kHz float32 / IMA-ADPCM), input-map chunk, (v1.0) UI layout chunks; GUID → chunk resolution; hot-reload notifications (input map + clips re-register, handles stay generational-stable) | We never see a WAV file. The `.mce` audio chunk layout is co-owned with `assets` (FR-AST-100) |
| `ecs-gameplay` | `Frame` (resources, events, phases), component registration (`AudioSource`/`AudioListener` in the starter kit, FR-ENT-080), `MCE_SYSTEM` phase registration, world lifecycle hooks (`on_add`/`on_remove` → register/stop voices), event queues (`AnimFrameEvent`, `DamageDealt`, `TriggerEnter`) | The bridge system is an ordinary phase member — no scheduler privileges (ecs-gameplay §5.2 rule) |
| `mce_refl` (core subsystem, L1 — ARCHITECTURE F4) | component field registration (inspector edits `AudioSource` fields live), save-game serialization of rebinding data (FR-INP-040) and (v1.0) UI bind paths | All scene/inspector/save flows route through reflection (binding rule); `mce_refl` is the sole metadata source |
| `rhi-renderer` | 2D draw-list submission path (v1.0: `mce_ui` submits quads through the same MDI batch; MVP text HUD uses `TextLabel` components → renderer's font pipeline) | No direct calls; we produce draw data, renderer owns GL — the seam rule applies to us like everyone else |

### 5.2 What consumes this subsystem (downstream)

| Consumer | What it takes | Rule |
|---|---|---|
| `ecs-gameplay` | `InputState` as a world resource (sampled per tick, FR-INP-030); `AudioDevice` as a resource; the `audio_bridge` system registered in `FixedUpdate_Post`; `AudioSource`/`AudioListener` starter-kit components | Gameplay polls actions; it never sees `SDL_Event` (CI grep, FR-INP-010) and never calls the device directly |
| `editor-tools` | action layer for editor shortcuts (its own action map, pushed as an overlay map); raw device state for viewport navigation; `InputState` inspection in play mode; audio mixer panel (bus gain convars + `Stats`); input rebinding UI (v0.5) | Editor consumes the same public API a game would — no editor-only back doors into device state beyond `input::key_held` (§2.2), keeping the seam honest |
| `scripting` | `AudioHandle` for sequence steps (`play_sound`, `stop_sound` actions in the action-list runner, FR-ENT-100); timeline audio tracks (FR-ENT-110, v1.0) issue `play_at` with the sample clock for sync | Sequencing goes through the same command API — no second audio path |
| `physics` | nothing (physics emits trigger events; the bridge listens) | Direction enforced: physics never imports audio/input |
| `core` (RUN shell) | `TickContext.actions` (the alias, §0 note); app-state pause/focus behavior | RUN owns the loop; input owns what a tick's input *is* |

**Dependency-arrows invariant:** input and audio are *leaves* in the runtime DAG (like physics) — nothing above them depends on their internals, and they depend on no gameplay concepts. `mce_ui` (v1.0) additionally reads `ecs-gameplay` read-only (queries + events) and `mce_refl` (bind paths) but is never imported by gameplay (FR-UI-030, CI-greppable). Logical-name mapping: `platform` and `mce_refl` are L1 modules inside the `core` subsystem (ARCHITECTURE §2.1 layer table is the module authority; this doc's tables use the 8-subsystem logical names with module qualifiers where the contract is finer-grained).

---

## 6. Test strategy

Per the binding test policy; every item names its FR. Input and audio are the two subsystems where unit tests beat playtesting, because both are driven by synthetic streams.

### 6.1 Input (doctest, deterministic, no SDL needed)

1. **Tick-quantization determinism (FR-INP-030, the load-bearing test):** feed the same synthetic event stream (presses at varying offsets) through three virtual render loops pacing 30/90/240 Hz against a 60 Hz tick grid; assert byte-identical `InputState` sequences per tick. Runs in the determinism build; failures here are determinism regressions by definition.
2. **Edge semantics:** press/release/press within one tick window → `held=true, pressed=true` (final state wins, edges folded in timestamp order); press+release between ticks → visible as `pressed` then `released` on consecutive ticks (the 1-tick visibility floor, documented behavior); release with no prior press is a no-op.
3. **Action mapping unit tests (FR-INP-020):** composite WASD→Axis2D with circular deadzone (scaled-radial per XInput guidelines), per-binding response curves (linear/ease/spline) against hand-computed values; map push/pop precedence (UI map shadows Gameplay map for `confirm`); unknown action name → `Result`-free safe default + one error (FR-DX-050 taxonomy).
4. **Rebinding round-trip (FR-INP-040):** rebind via synthetic capture → serialize through reflection → fresh `InputMap` → deserialize → same behavior on the synthetic stream; editor-restart persistence is the same serialization test pointed at the save mount.
5. **Focus loss:** `FocusOut` zeroes all held state and clears the ring; a held key's `released` edge fires exactly once on the next tick.
6. **Editor capture seam (§4.6):** with `WantCaptureKeyboard` toggled, key events are tagged consumed and `InputState` is unchanged; game-view focus restores action flow. Synthetic toggle, no real ImGui needed (the seam is a plain flag).
7. **Replay equivalence (FR-RUN-070):** record 60 s of synthetic input → replay → recorded `InputState` stream is byte-identical (this is the harness `ecs-gameplay`'s FR-RUN-040 test consumes).
8. **CI grep:** no `SDL_Event` outside `mce_platform` and the single audio/input seam files (FR-INP-010 acceptance).

### 6.2 Audio — unit and RT-discipline tests

1. **FIFO correctness:** SPSC ring with a producer/consumer thread pair + sequence numbers (extends core's §6.3 test with `AudioCommand` payloads); overflow → `play()` returns null handle, no corruption; every command consumed exactly once.
2. **RT-safety by construction (FR-AUD-010):** debug builds install the fail-allocator on the audio thread — any callback allocation asserts instantly; a static review hook (clang-tidy custom check, shared with the no-heap rule) bans `malloc`/`new`/mutex/`World` includes in the mixer TU. The callback also runs against a **fake clock + fake device** in tests: deterministic frame counts, no WASAPI in CI.
3. **Sample-accurate scheduling (FR-AUD-020):** with the fake device, schedule 1,000 one-shots at random sub-frame offsets; assert each voice's first output sample index matches the requested `mix_sample` exactly (integer comparison — the whole point of the sample clock).
4. **Voice stealing:** play 49 clips (v0.5 pool size) → lowest-priority/quietest/oldest stolen; stolen handle fails `is_alive`, never aliases a new voice; music-bus priority flag protects the loop.
5. **Bus mix reference (FR-AUD-030):** render a scripted mix (known clips, known gains, one bus chain) through the mixer's *offline* path (same `mix_block` code, pumped to completion) and compare to a checked-in reference render within −60 dB — this also regression-tests gain ramps (no click = no discontinuity above threshold).
6. **DSP reference tests (FR-AUD-060):** impulse + swept-sine responses for biquad (compare against the RBJ coefficient model), Freeverb (impulse response envelope + decay time bounds), limiter (no sample above 0 dBFS over a +6 dB hot input; attack/release within tolerance).
7. **One-shot latency (FR-AST-100, finding F13 — note the gate lives in FR-AST-100/FR-AUD's cook + NFR-T-05 rows; there is no FR-AUD-100):** two-part gate. (a) **CI-capable primary:** with the fake device + fake clock, measure command-enqueue-to-callback-mix for a one-shot — assert ≤ 25 ms worst-case at the documented ring/block sizes (this is the engine-controllable segment and runs on every dev-machine perf job). (b) **Informational:** device loopback on the devhost (WASAPI loopback via SDL) records trigger-to-output with a ≤ 40 ms target at the 480-frame period request; driver-granted periods are outside the engine's control, so `audio.device_latency_ms` is the documented tuning lever and the measured number is pinned per dev machine, not asserted in CI.
8. **Gapless stream loop (FR-AUD-040):** render 10 minutes of a looped streamed clip through the fake device; assert zero underruns and waveform continuity across every loop boundary (cross-correlation at the seam, tolerance 0).
9. **Soak:** 10-minute mixed load (SFX storms + streamed music + param ramps) under ASan: no allocs, no underruns, voice count returns to baseline (leak assert).

### 6.3 UI (v1.0 tests land with the build; contracts pinned now)

- Prototype panel+button+text renders through the sprite batch → golden image (FR-UI-020 acceptance; rhi-renderer's golden-image harness, dev-machine perceptual threshold per R9/A8).
- Gamepad focus navigation: synthetic `InputState` walk of a 3×3 menu grid; focus reaches every widget; `confirm` fires exactly once per press.
- Binding: health-bar bound to a reflected property path updates when the property changes; gameplay target compiles with zero `mce_ui` includes (CI grep, FR-UI-030).
- FR-UI-048 wall: CI symbol scan of `mce_engine.dll` + the shipped `game.exe` for `ImGui*` symbols — negative test ships from M0 (the wall is enforced before the system exists, so nothing can grow on the wrong side; scan target per ARCHITECTURE F8).

### 6.4 Determinism integration

The subsystem's contribution to FR-RUN-040: input is quantized before it reaches the sim (§3.2), and audio *commands* (not audio output) are hashed alongside sim state in the harness. Explicitly out of the determinism contract: rendered audio (driver/device timing), mouse-delta-consuming presentation systems (§3.2 note), and device hot-plug timing.

---

## 7. Risks & mitigations

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| R1 | **RT audio discipline broken subtly** (an allocation in a DSP node, a mutex taken under contention, a click from a non-atomic param) — the classic solo-engine audio failure | Medium | High (clicks/underruns ship to players) | Fail-allocator + no-lock static checks on the mixer TU (§6.2.2); all param changes are FIFO-carried ramps by construction (no atomics in the hot path except two snapshot pairs); fake-device tests run the exact callback body in CI; Tracy audio-thread track from M1 |
| R2 | **Tick quantization ambiguity** — an event's timestamp straddling the accumulator's catch-up clamp produces different tick attribution between live play and replay | Medium | High (breaks FR-RUN-040's replay promise) | The recorded *post-quantization* `InputState` stream is the artifact of record for replay (§3.2.5); live quantization is deterministic w.r.t. the shared `now_us` clock; the odd-rate test (§6.1.1) plus the replay-equivalence test (§6.1.7) pin both halves; the mouse-delta exclusion is documented, not hidden |
| R3 | **Action layer over-growth** (per-device profiles, response-curve editors, controller remap UI) eating v0.5 weeks | High (it is the most seductive scope creep in this subsystem) | Medium | MVP ships the fixed-field + named-action core with one default map; rebinding UI and gamepad-first-class land at v0.5 per FR tiers; per-device profiles and remap UX are explicitly Won't-adjacent (N15); cut ladder rung 4 applies if audio/input threatens Artifact A's date |
| R4 | **SDL3 audio API churn** (SDL3 is young; `SDL_OpenAudioDeviceStream` semantics may shift across releases) | Medium | Medium | Pin the vcpkg version (binding build policy); the callback body is ~150 lines against a stable subset (`SDL_AudioDeviceCallback`-shaped); the device-open seam is one function — a patch release is absorbed in an afternoon; fallback ADR pre-drafted mirroring core.md R1 (vendor SDL3) |
| R5 | **Audio thread and main thread race at shutdown** (callback fires during registry teardown → UAF in ASan, heisenbug in release) | Low | High | Shutdown order is explicit (§4.3): stop-all → bounded drain → `SDL_CloseAudioDevice` (SDL guarantees no callback during close) → assert cb-exited flag → teardown; the 10-min soak + ASan per PR covers the path |
| R6 | **`mce_ui` eats a tier** — the retained-UI system is a real system and v1.0's schedule is the tightest | Medium | High | The requirements already fence it: design doc at M4, build at v1.0, MVP/v0.5 ship scene-state menus + text sprites (A9); the **RmlUi fallback is named and pre-approved** (research §4.2) — a schedule slip converts to a one-ADR dependency, not a slipped milestone; FR-UI-040's extra widgets are Could and cut first |
| R7 | **ImGui/game input contention** (typing in a text field fires gameplay actions, or game view steals editor keys) — an annoyance that erodes UC-C trust daily | High if unhandled | Medium | The event-tagging seam (§4.6) is one function with a unit test (§6.1.6); game-view-focus click routing is part of FR-EDT-010's shell, tested with the editor open in CI's smoke sample |
| R8 | **Streaming underruns on cold cache** (first music start after scene load, disk spin-up) | Medium | Low | Prime-first-page synchronous fill at `register_stream` (≤ 100 ms, off the critical path of load — acceptable inside NFR-M-01's 5 s boot); 4-page ring gives 300 ms of slack; underrun keeps the voice alive (§3.6) |
| R9 | **One-shot latency misses 25 ms** (finding F13 split: the CI-gated segment is command-enqueue-to-callback-mix on the fake device; a miss there means engine-side ring/block sizing is wrong and is a real defect) | Low | Medium | Request 480-frame periods at open and assert the granted spec in debug; the enqueue-to-mix gate (§6.2.7a) is the binding test. Device loopback overruns are a different, informational failure — drivers may grant 20 ms+ periods the engine cannot control; document the granted latency via `audio.device_latency_ms` in the dev overlay and re-request with the convar, with the loopback run (§6.2.7b) pinning the real number per dev machine rather than failing CI |
| R10 | **Determinism boundary confusion** — someone hashes audio output or depends on mouse deltas in sim code | Medium | Medium | The contract is written once here (§4.7, §6.4): commands are hashed, output is not; the FR-RUN-040 harness's input stream is by-construction action-only; a code-review rule + the headless GL/SDL-leak greps keep device access out of sim paths |

---

## 8. Milestone mapping (binding tiers)

Tier names with ROADMAP milestone IDs (ARCHITECTURE §10.1 is the mapping authority — finding F8).

- **ROADMAP M0 (MVP):** nothing of this subsystem builds yet; the ImGui event seam ships with the editor shell.
- **MVP (ROADMAP M3):** input wrapper + action layer + fixed-field binding + tick quantization + determinism/replay tests (FR-INP-010/020/030); audio device callback + SPSC FIFO + voice pool (8) + one-shot/loop + gain/pan + streamed music loop + Master bus only (FR-AUD-010/020@8/040, FR-AST-100 contract) + `AudioSource`/`AudioListener` starter kit + bridge system; **60 Hz command granularity — sample-accurate `play_at` scheduling deferred to v0.5 (finding F2)**; Artifact A's SFX + music loop run through it.
- **v0.5 (ROADMAP M6):** gamepads as binding targets + rumble (Could), runtime rebinding + persistence (FR-INP-040), bus graph Music/SFX/UI + per-bus biquad EQ + Freeverb send + master limiter (FR-AUD-030/060), 48 voices (FR-AUD-020), **sample-accurate `play_at` scheduling on the `mixSample` clock (§3.4, deferred from MVP)**, 2D/3D spatialization with attenuation curves (FR-AUD-050), event-driven one-shots from animation frames (FR-AUD-055), editor mixer + voice debug panels, input rebinding UI.
- **v1.0 (ROADMAP M8):** `mce::ui` build (design finalized at M4 in [ui.md](ui.md), per CF21): anchored-rect tree, panel/image/text/button/slider primitives through the sprite batch, action-driven focus navigation, read-only reflection data binding, shipped debug console over convars (FR-UI-010/020/030/045); RmlUi fallback ADR only if the prototype slips (the pre-approved trigger — ROADMAP M8 holds `mce_ui` to it).

## References

- Jason Gregory, *Game Engine Architecture*, 3rd ed., ch. 13 — mixer/voice/bus architecture, virtual voices, the runtime-audio shape adopted here.
- Glenn Fiedler, "Fix Your Timestep," Gaffer On Games — tick grid; the audio-scheduling note behind §3.4's sample clock.
- Timur Doumler, "C++ in the Audio Context," CppCon 2015 — the RT rules the FIFO/callback design enforces.
- RBJ, *Audio EQ Cookbook* — biquad coefficient formulas (§3.7).
- Schroeder & Moorer Freeverb design — the reverb topology (§3.7).
- Unity Input System / Unreal Enhanced Input / Godot InputMap docs — the action/binding/action-map layer shape (research §3).
- SDL3 wiki — `SDL_OpenAudioDeviceStream`, `SDL_Gamepad`, text-input APIs (device seams only).
- Nystrom, *Game Programming Patterns* — Event Queue (the FIFO pattern), Dirty Flag (UI layout), Object Pool (voices).
- XInput deadzone guidelines — scaled-radial deadzone math (§3.1).
- Repo-internal: `docs/research/audio-input-ui.md` (committed decisions), `docs/architecture/core.md` (thread roster, allocators, loop), `docs/architecture/ecs-gameplay.md` (phases, events, starter kit), `docs/REQUIREMENTS.md` (FR-AUD/INP/UI).
