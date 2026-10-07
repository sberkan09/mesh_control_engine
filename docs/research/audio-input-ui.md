# Research: Audio, Input, and UI (Editor + Runtime) for MCE

**Domain:** audio-input-ui
**Date:** 2026-10-06
**Status:** Recommendation committed; feeds ADRs at M1 (audio/input), M0 (editor shell), M4 (runtime UI design).
**Binding context:** Per the chosen stack (C++20/MSVC, GL 4.5 Core RHI, SDL3, Dear ImGui docking), the approved runtime dependency list is **closed**: SDL3, glad2, Dear ImGui (docking), stb_image/stb_truetype, meshoptimizer, Tracy. Anything else needs an ADR before first `#include`. The from-scratch list explicitly includes the **audio mixer/voices/DSP on top of SDL3's device callback** and the runtime never parses JSON/XML/PNG. This document recommends within those constraints.

---

## 1. State of the art (grounding)

Three sub-domains, three different levels of industry consensus:

- **Audio** splits cleanly at one seam: *device I/O* (talking to WASAPI/CoreAudio/ALSA with correct callback timing) is solved, boring, and platform-specific — everyone delegates it. *Everything above the callback* (mixer, voice management, buses, DSP, streaming, sample-accurate scheduling) is where engine audio actually lives and what *Game Engine Architecture* (Gregory, 3rd ed., ch. 13 "Audio") devotes a full chapter to. Commercial middleware (FMOD Studio, Wwise) dominates AAA, but both are engines-in-themselves — disqualified by the single-purpose-library rule and by the learning goal.
- **Input** has converged on a three-layer model across every modern engine (Unity's new Input System, Unreal's Enhanced Input, Godot 4's InputMap): a thin *platform event layer*, a *device-state snapshot* layer, and a *semantic action mapping* layer that decouples gameplay code from hardware. Raw Win32 `WM_INPUT`/XInput is what SDL3 already wraps; the design work worth doing ourselves is the action layer and the fixed-timestep sampling discipline.
- **UI** is two genuinely different problems wearing one word. *Editor UI* was settled industry-wide around 2014 when Omar Cornut's Dear ImGui became the de facto tool UI of engine teams; the only live question is integration discipline. *Runtime game UI* is genuinely unsettled — the industry is split between retained scene-graph UI (Unity uGUI, Unreal UMG/ Slate), HTML/CSS-derived stacks (RmlUi, Noesis, Coherent/Ultralight, the deceased Scaleform/Flash lineage), and immediate-mode overlays (ImGui itself, as in many PC tools and mods). This is the one place where "decide later but design now" is defensible, and Section 4.2 commits accordingly.

Canonical references used throughout: *Game Engine Architecture* ch. 13 (audio architecture, voice/busi model); Gaffer On Games — "Fix Your Timestep" (fixed-step sim + interpolated render) and the audio scheduling note (sample-accurate vs frame-accurate playback); Erin Catto's Box2D GDC slide decks (for the general "write the small version of a famous system" method); SDL3 wiki (`SDL_OpenAudioDeviceStream`, `SDL_AudioStream`, gamepad/text-input APIs); Dear ImGui's README and the docking-branch examples (docking, multi-viewport); Casey Muratori, "Immediate-Mode Graphical User Interfaces" (2005) and Sean Middleditch's rebuttal "DeImmediate Mode GUIs" for the IMGUI-vs-retained debate; the RmlUi and Noesis project docs for the HTML/CSS-in-games lineage.

---

## 2. Audio

### 2.1 Surveyed options for the device layer

| Option | What it is | Tradeoffs for MCE |
|---|---|---|
| **SDL3 audio** (`SDL_OpenAudioDeviceStream` + callback, `SDL_AudioStream`) | Part of the already-approved SDL3; C API, device enumeration, format conversion, per-device streams | Zero new dependencies, one integration for window/input/audio/threads, callback model is exactly the seam the mixer needs. Its `SDL_AudioStream` resampler exists but we will treat it as an oracle/reference, not a crutch (see 2.3). |
| **miniaudio** | Single-header C audio lib: device abstraction + built-in mixer/graph + resampling | The strongest competitor and the panel's losing candidate used it. Its device layer is excellent, but its built-in mixing graph would silently absorb the exact subsystem we committed to hand-write, and it is **not on the approved dependency list** — adding it requires an ADR for a capability SDL3 already provides. |
| **OpenAL Soft** | Cross-platform 3D-audio API (source/listener model, HRTF) | Wrong abstraction level for us: a whole spatialization model we didn't design, C API with global-ish state, and we would still write our own mixer under it. Its 3D spatialization ideas (distance models, HRTF) are worth reading, not linking. |
| **XAudio2 / WASAPI directly** | Windows-native | WASAPI is what SDL3 wraps anyway; XAudio2 is Windows-only (kills the Linux-plausibility SDL3 buys) and its voice model would *be* our mixer, designed by Microsoft. |
| **FMOD / Wwise** | Full commercial middleware | Disqualified: engines-in-themselves, license cost, and they would outsource the entire learning objective. |

### 2.2 Surveyed options for everything above the callback

- **FMOD/Wwise model (DSP graph + authoring tool):** the industry ceiling. buses, effects racks, sample banks, event system. Far too large to build, and its value is mostly the *authoring tool*, which no solo from-scratch project should attempt. We adopt only its *architecture*: buses, per-bus effect chains, and a simple event/desc model (Gregory ch. 13 describes precisely this).
- **SDL_mixer:** rejected outright — it is SDL's mixer and importing it would outsource the mixer. Never a candidate.
- **Roll your own on the callback:** what the from-scratch list mandates and what this document designs (2.3).

### 2.3 Recommendation for MCE — audio

**Committed decision: SDL3 owns the device callback only; everything else is hand-written `mce::audio`.**

Concrete design, all decisions:

- **Format contract (cook-time normalization).** The mixer runs at **48 kHz stereo float32** internally, full stop. The cook pipeline decodes source WAV (and, if needed later, OGG via a cook-time-only stb_vorbis — that would need an ADR) and writes **PCM float32, resampled to 48 kHz at cook** using our own resampler (linear for drafts, Catmull-Rom/Hermite for final). Rationale: a single internal format deletes the entire runtime format-conversion problem — voices never resample at runtime, mixing is one `float` MAC loop, and SIMD is trivial. Cooked audio lives in `.mce` chunks; the runtime never sees WAV or any decoder. This mirrors the "runtime never parses PNG/JSON" asset rule: WAV is interchange, `.mce`-audio is runtime.
- **Compression/size policy.** SFX that fit (the vast majority, target budget: < 32 MB resident) are **in-memory**; music/ambience/dialog are **streamed** from the memory-mapped `.mce` container via a ring buffer of decoded frames (double-buffered, ~100 ms pages) filled on a worker thread. If size pressure appears at M3, add **IMA-ADPCM encode at cook** (decode is ~10 lines of table math, 4:1 ratio, quality fine for SFX — the classic tradeoff id Software, Unreal, and Box2D-era engines all shipped). Do *not* build MP3/Vorbis runtime decoding; that is a runtime dependency hole with no learning payoff.
- **Mixer graph.** Hand-rolled per *Game Engine Architecture* ch. 13: N **voices** (a fixed pool, e.g. 48) → optional per-voice low-pass/gain → **buses** (Master, Music, SFX, UI, plus up to 8 user buses) → per-bus chain of DSP effects → master limiter → float mixdown → SDL3 callback. Each bus is a simple node; the graph is data (cooked or editor-authored), not a scene-graph-style free topology — fixed fan-in from voices to buses, linear chain on buses. This is the FMOD shape minus the authoring GUI.
- **Voice model.** A `VoicePool` with priority stealing: on exhaustion, steal the lowest-priority, quietest, or oldest virtual voice. **Virtual voices** (audible logic, no mixing) are unlimited; **real voices** are the pool. Distance attenuation follows an inverse/linear rolloff curve choice per sound; occlusion/low-pass is just the per-voice DSP slot. This is the voice/virtual-voice split every serious engine uses (Gregory ch. 13.4).
- **DSP (all hand-written, all scalar-first):** gain/pan (constant-power), 2-band EQ and one-pole/State-Variable low-pass per the **RBJ Audio-EQ-Cookbook** (the standard biquad reference), a **Freeverb-style Schroeder-Moorer reverb** (comb + allpass, the classic 1990s design still behind most small-engine reverbs), and a look-ahead **compressor/limiter** on the master bus. Each DSP node is `process(float* L, float* R, size_t frames, const Params&)`. Nothing exotic: the learning is in the structure, not exotic algorithms.
- **Scheduling.** Playback requests are timestamped; the mixer honors them to the *sample*, not the frame, by carrying a sub-frame offset into the next callback (Gaffer On Games' audio-scheduling point: fire at 60 Hz means up to 16 ms of jitter if you naively start at frame boundaries). Music/beat sync uses the same sample clock — one authoritative `mixSample` counter.
- **Threading.** The SDL3 callback runs on SDL's audio thread and **only** pulls from a lock-free FIFO of control messages (play/stop/param-set) and reads pre-resolved voice data; it never allocates, never locks a mutex the game thread holds, and never touches the ECS. The game thread submits `AudioCommand`s (a small POD command buffer, like a render command buffer). This is the standard "audio thread is a RT thread" rule every pro source repeats (Timur Gagiev's miniaudio docs, FMOD's threading docs).
- **ECS integration.** `AudioSourceComponent` (clip handle, bus, gain, pitch, loop, attenuation) + an `AudioListenerComponent` on the camera; an `audio_playback_system` runs after physics at the end of the fixed step and emits commands. Clip references are generational handles into the audio resource cache, like every other asset.
- **API sketch.**

```cpp
namespace mce::audio {
    using ClipHandle = Handle<AudioClip>;            // generational index
    using VoiceHandle = Handle<Voice>;

    struct ClipDesc { const char* path; BusId bus; bool stream; float gainDb; };

    // main thread API — enqueue commands, never block
    VoiceHandle play(ClipHandle clip, const PlayParams& p = {});   // position, loop, pitch, priority
    void        stop(VoiceHandle v);                               // graceful release, not click
    void        setBusGain(BusId bus, float db);                   // volume mixer UI reads this
    void        setListener(const float3& pos, const float3& vel, const Quat& orient);

    // DSP nodes are plain structs registered on a bus chain at cook or runtime-init
    struct Reverb { float roomSize, damping, wet; };               // Freeverb-style
}
```

**Rationale in one line:** SDL3's callback is the boring 5% we'd get wrong by hand (WASAPI COM, device hot-swap, underrun handling); the 95% that teaches (mixer, voices, DSP, streaming, scheduling) is ours, exactly as the from-scratch list requires.

---

## 3. Input

### 3.1 Surveyed options

- **Raw Win32 (`WM_INPUT`, XInput, `WM_KILLFOCUS`, IME messages):** maximum fidelity, ~2–3 weeks of yak-shaving (gamepad driver database, IME composition, DPI-adjacent focus quirks) for near-zero architectural learning — SDL3 is already an approved dependency that wraps all of it. The panel doc already committed SDL3 for windowing/input; raw Win32 stays a documented escape hatch (SDL3 lets you pump and inspect `SDL_Event` but also passes through window-proc access if ever needed).
- **SDL3 input:** keyboard/mouse events, `SDL_Gamepad` with the community **GameControllerDB** mappings compiled in, hot-plug events, sensor support, `SDL_StartTextInput`/`SDL_TextInputEvent` for IME-aware text. This is the platform layer, decided.
- **Unity's new Input System / Unreal Enhanced Input:** the semantic layer to imitate — **actions** (named gameplay intents: `Jump`, `Fire`, `Move`), **bindings** (device-specific: `Space`, `GamepadSouth`, key-combos), **composite axes** (`WASD` → 2D axis with deadzone and response curve), and **action maps** (swappable sets: `OnFoot` vs `InVehicle`). This is the layer worth hand-writing; it is small, pure logic, and it is what makes shipped games configurable/remappable.
- **Steam Input:** wraps remapping for controllers at the platform level; useful knowledge, but SDL's gamepad layer plus our action layer covers the need without a Steam dependency. Revisit only if we ship on Steam with controller-heavy games.
- **GLFW input:** fine, but SDL3 is already the windowing decision — one platform library, not two.

### 3.2 Recommendation for MCE — input

**Committed decisions:**

1. **Platform layer = SDL3 events, wrapped once.** `mce::platform::InputSystem` pumps SDL, translates to engine-internal events, and owns device state. No gameplay code ever sees an `SDL_Event`. The editor's ImGui backend (`ImGui_ImplSDL3`) receives SDL events directly *before* the engine layer and can mark input as consumed — this is the seam that keeps editor and game input from fighting (Section 4.3).
2. **Action mapping layer is hand-written** (`mce::input`), modeled on Unity's Input System/Unreal Enhanced Input because it is the converged industry design: `Action` (typed: `Button`, `Axis1D`, `Axis2D`), `Binding` (device + control, with modifiers), `ActionMap` (swappable sets), deadzones (per-axis + circular, standard and the better "scaled radial" from the XInput guidelines) and response curves (linear, ease, custom spline for trigger feel).
3. **Fixed-timestep sampling discipline (the load-bearing decision).** Input is **sampled per simulation tick, not per render frame**. The platform layer accumulates SDL events into per-frame device snapshots + a press/release edge queue; at each 60 Hz tick the sim consumes the snapshot since the last tick and computes edges from *state comparisons* (held-down transitions), never from raw event timing — so a 240 Hz render frame and a 60 Hz sim agree, and a dropped frame can't eat a button press. This is the input-side corollary of Gaffer On Games' "Fix Your Timestep" and it must be a unit test (feed synthetic events at odd rates, assert identical sim-visible sequences → determinism requirement).
4. **Gamepad:** SDL3 `SDL_Gamepad` (its database is maintained upstream — mapping drift is their bug, not ours). Expose standard gamepad layout (`SDL_GamepadAxis`/`SDL_GamepadButton`) as first-class binding targets alongside keyboard/mouse. Rumble later via `SDL_RumbleGamepad` — one function call, defer.
5. **Text input:** SDL3 text-input events with IME composition rects reported back for the editor's text fields (ImGui handles its own IME via the SDL3 backend — nothing for us to write) and for the runtime console once the runtime UI exists (Section 4.3). Gamepad text entry is explicitly out of scope.
6. **Focus-loss correctness:** on window focus loss, zero all held states and cancel edges (the classic "stuck run key" bug — SDL3 sends `SDL_EVENT_WINDOW_FOCUS_LOST`; handle it in the wrapper, not per-game).
7. **API sketch.**

```cpp
namespace mce::input {
    struct ActionMap { /* named actions, bindings, loaded from cooked input map asset */ };

    // Gameplay polls typed actions only — never devices:
    bool    down(const char* action) const;
    bool    pressed(const char* action) const;    // edge, valid this tick
    float2  axis(const char* action) const;       // deadzoned + curved, e.g. "Move"

    // Editor/testing: replay a recorded input stream (determinism harness reuses this)
    void    applySnapshot(const DeviceSnapshot&);
}
```

**Rationale in one line:** SDL3 for events/devices is free and already paid for; the only intellectually honest work — the semantic action layer and tick-aligned sampling — is exactly the part we write, and it doubles as the determinism test harness.

---

## 4. UI

### 4.1 Editor UI

**Surveyed options:**

- **Dear ImGui (docking branch):** immediate mode; C++-native; the de facto standard engine tool UI (used in production editors across the industry since the mid-2010s; the docking branch is what most engine tools actually ship on). Costs: default look (mitigated by theming + font atlas), immediate-mode state lives per-frame, and it is emphatically not a shipped-game UI.
- **Qt 6:** the serious retained alternative — full widget set, real docking, real undo frameworks. But: LGPL/commercial license friction, a second UI paradigm and enormous dependency surface, C++ build weight, and its idioms (signals/slots, QObject ownership) would colonize the editor codebase in ways that teach Qt, not engine architecture. Rejected.
- **Nuklear:** single-header IMGUI, but weaker tooling ecosystem, no docking, small widget set. Strictly dominated by ImGui for editor work.
- **egui (Rust):** not applicable — Rust stack was rejected at the panel.
- **Slint / GTK / web-based (Electron/CEF):** license, size, or interop costs with zero engine-learning payoff. Rejected.

**Committed decision: Dear ImGui docking branch, linked into the editor target only (see 4.4).** Specifics:

- **Docking branch pinned** (it is production-stable and is what real engine editors use); multi-viewport (OS windows outside the main window) **off by default** — it is the least mature corner and the single main window with dockspace is enough for M1–M3.
- **Widgets we do NOT get for free and will hand-build on top:** the outliner/tree (ImGui tables + custom id-space discipline), the asset browser, gizmos, curve editors, node graphs. **Gizmos:** recommend **ImGuizmo** (the standard ImGui companion for translate/rotate/scale gizmos, used across engine tools). It is not on the closed dependency list, so it needs a one-page ADR at M1 — it is a 2-file, single-purpose, ImGui-native addition, the easiest ADR case imaginable. **ImPlot** (plots) similarly ADR-gated, deferred until the profiler view needs it.
- **Editor architecture:** one `EditorContext` owning panels as regular C++ objects; each panel draws via ImGui per frame; **undo/redo is a command stack over reflection-driven property writes** (the inspector emits `SetPropertyCommand{entity, propertyId, oldValue, newValue}`), not ImGui state — this is what keeps the editor honest about the reflection system.
- **File dialogs:** SDL3's built-in (`SDL_ShowOpenFileDialog`) — no native-file-dialog dependency needed.

### 4.2 Runtime game UI — the real fork in the road

**Surveyed options for shipped-game UI:**

| Approach | Examples | Pros | Cons for MCE |
|---|---|---|---|
| **Immediate mode (ImGui itself)** | many PC tools, mods, some shipped indie UIs | zero design work, already in tree | memory-heavier per-frame (vertex buffers each frame), no artist workflow, no text layout richness, wrong visual language for games; folding it into shipping code couples the runtime to ImGui forever |
| **Retained HTML/CSS-derived** | **RmlUi** (open source, MIT-like, actively maintained, the spiritual successor to libRocket), Noesis GUI (commercial), the deceased Scaleform lineage, Coherent/Ultralight | rich text/layout, artist-friendly (CSS/HTML or XAML), battle-tested | RmlUi is a sizable dependency (its own layout engine, its own renderer backend we'd write against our RHI — real work but its existence proves the seam), and it imports a *lot* of vocabulary we didn't design; Noesis is commercial-priced |
| **Retained scene-graph canvas (Unity uGUI / Unreal UMG style)** | Unity uGUI, Unreal UMG, Godot Control nodes | full control, minimal runtime cost, teaches real UI architecture (layout, hit-testing, text shaping, 9-slice, focus/navigation) | it is a real system to build — weeks, not days; needs text shaping decisions (stb_truetype is on the approved list; full HarfBuzz shaping is overkill for MCE's scope) |
| **Custom immediate-mode-for-games (SlimUI-ish)** | various indie engines | tiny | same artist-workflow hole as ImGui; marginal savings over 4.1's approach |

**Committed decision: runtime UI is a **deferred-but-designed** custom retained system, RmlUi held as the pre-approved-fallback exception.**

- **The design happens at M4, the build at M5** — but the *decision* is recorded now so nothing accidentally couples to ImGui (binding rule: shipped-game UI must never fold into ImGui code paths).
- **The system we commit to designing: `mce::ui`, a small retained-mode scene graph** rendered through the same 2D MultiDrawIndirect batch path as sprites (no second renderer):
  - **Widget tree** with anchors/offsets layout (the Unity uGUI model — anchored rects resolve against parent rect; adequate for HUDs/menus, vastly simpler than full CSS flow);
  - **Primitives first:** panel (9-slice), image, text (stb_truetype atlas, SDF or per-font bitmap ranges; HarfBuzz-class shaping explicitly out of scope), button/toggle/slider (state machines, hit-test by tree traversal);
  - **Input model:** actions from Section 3 (`pressed("UI_Confirm")`), focus/navigation graph for gamepad menus — this is where the action-mapping layer pays off;
  - **Data flow:** data-bound widgets via the reflection system (a health bar binds to a component property path), so the UI reads state; UI *never* writes gameplay state except through explicit events — same editor/runtime authority principle as the scene graph;
  - **Definition format:** widgets defined in C++ (builder API) for M5; a cooked `.mce`-UI binary (layout tree serialized via reflection) only if/when an artist workflow appears.
- **RmlUi is the named fallback:** if M5's schedule collapses or a text-heavy game needs real layout/HTML authoring, RmlUi enters via ADR as a single-purpose runtime dependency with an adapter against our RHI (its backend interface is small). Naming it now prevents both scope creep (building a layout engine twice) and quiet drift (nobody sneaks it in without the ADR).
- **Immediate-mode is explicitly rejected for shipped UI** (Casey Muratori's 2005 IMGUI argument is about *tools* ergonomics; Sean Middleditch's critique and a decade of shipped-tool experience agree that animation, layout richness, and artist workflows favor retained for player-facing UI). ImGui remains our **debug overlay** in dev builds — drawn through the editor path, compiled out of shipping (Section 4.3).

### 4.3 How the editor stays out of the shipping runtime (enforcement, not good intentions)

This is a binding-adjacent concern and gets mechanism, not vibes. Four enforcement layers:

1. **CMake target separation (the real wall).** `mce_runtime` (the shipped engine lib) **does not link ImGui and cannot even see its headers** — ImGui is a dependency of `mce_editor` (and, in dev configs only, `mce_devhost`, the debug-game harness). CMake dependency direction makes leakage a link error, not a review finding:
   ```
   mce_editor.exe   -> mce_editor_layer -> mce_runtime + imgui(docking) + ImGuizmo(ADR)
   mce_devhost.exe  -> mce_devhost_layer -> mce_runtime + imgui          (dev-only target)
   game.exe         -> mce_runtime        (no imgui anywhere downstream)
   ```
2. **No `#ifdef MCE_EDITOR` in runtime code.** Conditional compilation would weld the two together while pretending to separate them; separate targets with separate `main`s (editor main vs game main) is the Unity/Godot shape and keeps the editor *owning* the canonical scene (binding rule) instead of the runtime hosting an editor mode.
3. **CI checks.** (a) grep: no `imgui.h` include outside `src/editor/` and `src/devhost/`; (b) symbol check: the `mce_runtime` static lib must contain no `ImGui*` symbols (trivial `llvm-nm`/dumpbin scan in the pipeline).
4. **Debug tooling in shipped games** (console, stats overlay) belongs to `mce::ui` + a tiny convar registry, so a shipped game can keep a debug console *without* ImGui — this is designed in from M4, not retrofitted.

**Editor-authority recap (binding rule, restated because it governs the UI code):** the editor owns the canonical scene representation and serializes it through the reflection system; the runtime consumes cooked scenes and never mutates them. Concretely: `mce_editor` holds `SceneDocument` (undo stack, dirty tracking, glTF import targets), and "Play in editor" spins the runtime against a **transient cooked copy** — the runtime can no more write the open document than a shipped game can.

---

## 5. Recommendation for MCE (committed decisions, one per row)

| Decision | Choice | Rejected alternative (with reason) |
|---|---|---|
| Audio device layer | **SDL3 audio stream + callback** | miniaudio (would absorb the mixer we're committed to writing; not on approved list); XAudio2 (Windows-locked); OpenAL Soft (whole spatialization model we didn't design) |
| Audio mixer/voices/DSP | **Hand-written `mce::audio`**, 48 kHz float32 internal, buses + voice pool + Freeverb-style reverb + RBJ-cookbook filters, RT audio thread via lock-free command FIFO | SDL_mixer (outsources the learning objective); FMOD/Wwise (engines-in-themselves, license, authoring-tool dependency) |
| Audio format policy | **Cook to 48 kHz float32 `.mce` chunks**; in-memory for SFX, ring-buffer streaming from mapped `.mce` for music/dialog; IMA-ADPCM at cook only if size demands | Runtime OGG/Vorbis decode (dependency hole, no learning payoff); runtime resampling (deleted entirely by cook normalization) |
| Input platform layer | **SDL3 events/gamepad/text**, wrapped once in `mce::platform::InputSystem`; gameplay never sees `SDL_Event` | Raw Win32/WM_INPUT (yak-shaving); GLFW (second platform lib for no gain) |
| Input semantic layer | **Hand-written action mapping** (actions/bindings/action-maps/deadzones/curves) modeled on Unity's Input System + Unreal Enhanced Input | Raw device polling in gameplay (untestable, unremappable) |
| Tick alignment | **Input sampled per 60 Hz sim tick**, edges computed from state snapshots; determinism unit test with synthetic event streams | Event-timestamp-driven gameplay (breaks fixed timestep + replay) |
| Editor UI | **Dear ImGui docking branch**, multi-viewport off, **ImGuizmo via ADR** for gizmos; undo/redo as reflection-driven command stack | Qt (license + dependency gravity + wrong lessons); Nuklear/egui (weaker/dominated) |
| Runtime game UI | **Deferred, but design committed at M4:** `mce::ui` retained scene-graph, anchored-rect layout, primitives-first, rendered through the shared 2D MDI batch path, bound to state via reflection; **RmlUi named as pre-approved fallback exception** (ADR) | ImGui in shipping (couples runtime forever, no artist workflow); building an HTML/CSS engine from scratch (months for negative value); Noesis (commercial license) |
| Runtime/debug separation | **CMake target wall**: `mce_runtime` cannot see ImGui; separate editor/devhost/game mains; no `#ifdef MCE_EDITOR`; CI include-grep + symbol scan; shipped debug console via `mce::ui` | `#ifdef`-gated editor inside the runtime (pretend separation, ships the editor by accident) |

### Sequencing (which milestone carries what)

- **M0:** editor shell = ImGui docking + ImGui_ImplSDL3 backend + ImGuizmo ADR filed; Tracy zones around the ImGui frame and the audio FIFO.
- **M1:** input wrapper + action layer + tick-sampling determinism tests; first playable 2D game uses actions, not raw keys. Audio: device callback + basic mixer/voices (gain, pan, loop, streaming music) — enough to have sound in the 2D game.
- **M3:** full DSP chain (filters, reverb, limiter), bus mixer UI in the editor, distance attenuation + occlusion hooks; gamepad as a first-class target.
- **M4:** `mce::ui` design doc + prototype (text atlas, panel, button, focus nav) *rendered through the sprite batcher*; shipped-debug-console design.
- **M5:** `mce::ui` powers the first shipped game's menus/HUD; the RmlUi fallback ADR is only exercised if the M4 prototype slips badly.

### Honest risks

1. **Audio RT discipline is easy to get subtly wrong** (clicks from non-atomic params, allocation in the callback). Mitigation: the command FIFO is written once, unit-tested with a fake callback, and the mixer's hot path is forbidden heap access by construction (frame-linear or preallocated).
2. **The action layer can over-grow** (response curves, per-device profiles, controller-remap UI). Mitigation: ship the Unity-shaped core, defer profiles/remap UI to M3+.
3. **`mce::ui` is a second renderer-adjacent system and could eat M5.** Mitigation: the M4 prototype is timeboxed; if it slips, the named RmlUi fallback converts a schedule risk into a one-ADR dependency rather than a slipped milestone.
4. **ImGui's pull on runtime code is cultural, not technical** — the target wall plus CI greps exist precisely because "just one imgui.h include in the runtime" is the failure mode to expect.
