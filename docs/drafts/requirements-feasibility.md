# MCE Requirements Draft — Feasibility & Risk (Solo-Developer Stress Test)

**Draft:** requirements-feasibility.md | **Date:** 2026-10-06 (rev 2) | **Status:** DRAFT for review — one of several requirements lenses (this lens: feasibility, effort, risk, scope discipline)
**Project:** Mesh Control Engine (MCE) — a from-scratch 2D/3D engine with editor, built by one developer on Windows 10 x64.
**Committed stack (binding, do not re-litigate here):** C++20 / MSVC v143, OpenGL 4.5 Core behind a thin RHI, SDL3, Dear ImGui (docking), archetype ECS / math / 2D physics / audio mixer hand-written, glTF 2.0 → cooked `.mce`, CMake presets + vcpkg.

---

## 0. Why this document exists

Every other requirements lens asks *what should the engine do?* This one asks the question that actually kills solo engine projects: **can one human finish it, and what gets cut when they can't?**

The evidence base for the failure mode is well documented: Hazel (The Cherno's C++/OpenGL/ImGui engine — the closest public project to MCE's shape) grew feature-by-feature with no shippable-artifact gates and eventually stalled mid-Vulkan-rewrite; CryEngine is the commercial form of the same disease (feature surface vs. nurture imbalance); Piccolo proves a solo editor engine is *possible* but drifted via AI-assisted sprawl and thin tests. Against that: id's tiny teams shipped era-defining engines on hard, simple formats and ship gates; Handmade Hero shipped something runnable every week for years; Our Machinery ran explicit per-subsystem memory budgets. The distilled lesson (see `docs/research/existing-engines.md`, lesson L8): **a solo engine dies from ungated scope, not from technology.**

So this document does five things:

1. States the **velocity model** — how many hours actually exist (§2).
2. Rates **every candidate feature area** S/M/L/XL and shows the honest total — *including content production, which rev 1 of this document omitted entirely* (§4).
3. Names the **top 10 scope risks** with early-warning signs and pre-agreed cut-lines (§5).
4. Defines the **minimum viable engine** — the smallest thing that ships one 2D demo *and* one 3D demo (§6).
5. Phrases **requirements with measurable acceptance criteria**, tiered MVP / v0.5 / v1.0 (§7), plus a global **cut ladder** (§7.2) that is decided *now*, while nobody is emotionally attached to any feature, and a set of **global feasibility rules** (§8).

**Rule for reading this document:** every requirement in §7 carries an effort tag and a tier. If the two conflict with anyone's enthusiasm, the tier wins. Features are added to a tier only by written slip note (binding: milestone gates may not absorb slipped scope silently).

**Rev-2 change log** (what the roadmap pass taught us, folded back in): (a) content production is now a first-class effort line and a named risk (R10) — the rev-1 total of ≈ 1,200–1,500 h counted engineering only and under-counted M3; (b) the MVP tier is mapped to the roadmap's M0–M4 (+ M3.5 stretch) sequence, with M3.5 named the tier's designated overrun absorber; (c) physics MVP scope is the rung-6 shape by default (kinematic player, dynamic-vs-static), not a degradation of it; (d) content sourcing is pinned to CC0 kits (Kenney, Poly Haven) so the MVP gate never depends on hand-modelled art.

---

## 1. Product definition, stated in feasibility terms

MCE is a **single-developer 2D/3D engine whose success criterion is not feature parity with Godot — it is the number of small complete games shipped with it.** Each shipped game is worth more feasibility evidence than any engine feature, because it exercises cook → load → simulate → render → audio → input → package end to end, which is where hobby engines quietly rot (Hazel never got there; it had a renderer and an editor but no cook pipeline and no shipped game).

Therefore the feasibility-first product shape is:

- **One engine, two demos as contractual artifacts** (§6): a 2D game and a 3D scene, both launchable by double-click by a non-programmer.
- **Depth before breadth in every tier:** a physics system that handles one genre well beats one that handles five genres badly.
- **Every milestone ends in a runnable artifact** (already binding) — this document adds the *number-of-hours* envelope per milestone so "runnable artifact" can't quietly mean "nine months of infrastructure."
- **Content is scoped like code.** A demo is not "done" when the engine works; it is done when art, audio, and level assembly hours are spent and reconciled. Under-planning content is how "programmer art shame" silently converts an engine project into a content project at month 10 (Risk R10).

---

## 2. The velocity envelope — the honest arithmetic

All effort estimates below are grounded in one assumption, which must be validated in week 1:

> **Baseline velocity: 10–15 focused hours/week** (evenings + one weekend block; the same assumption used in `docs/research/platform-engineering.md` §1.2, which computes that a 3-minute build steals a whole evening per week).

| Horizon | Focused hours |
|---|---|
| 1 month | 43–65 h |
| 6 months | 260–390 h |
| 12 months | 520–780 h |
| **24 months (2 yr)** | **1,040–1,560 h** |
| **36 months (3 yr)** | **1,560–2,340 h** |

Two conclusions fall straight out of this table and they drive everything below:

1. **The 2–3 year horizon contains roughly 1,000–2,300 focused hours.** For calibration: Hazel's on-screen portion alone (window, renderer, ECS wrapper, scene serialization, editor shell) is estimated by its episode count and pacing at well over 300 hours, and it never reached cook pipeline, physics, audio, or a shipped game. EnTT (one subsystem MCE's ECS is measured against) represents multiple person-years of a specialist. **The full MCE vision is XL — the plan only works if the tier discipline in §7 is treated as law, not aspiration.**
2. **Fixed overheads are the silent killer at this velocity.** At 12 h/week, one hour lost per day to waiting on a slow build, a flaky vcpkg restore, or a broken CI run is ~40% of capacity. This is why the platform research commits to unity builds + PCH + presets as *feasibility* requirements, not conveniences (REQ-MVP-01, §7).

A second, less comfortable arithmetic: **content hours are real hours.** Level assembly, kitbashing, SFX selection, and attribution passes do not benefit from unity builds or PCH — they are irreducibly manual. The roadmap's tier map carries them explicitly; §4 of this document does too.

**Velocity instrumentation requirements (binding feasibility rules):**

- **REQ-F-01 (MVP):** A weekly log (`docs/devlog/hours.md`, one line per session: date, hours, engineering vs. content, what moved, what's blocked) is kept from day one. *Acceptance:* at any milestone gate, total invested hours are known to ±2 h; velocity (h/week, 4-week rolling) is computable and is re-checked against the estimates in §4.
- **REQ-F-02 (MVP):** If 4-week rolling velocity drops below 6 h/week for two consecutive 4-week windows, the project formally down-tiers one rung of the cut ladder (§7.2) rather than slipping dates. *Acceptance:* the slip note template in `docs/adr/` contains a "velocity trigger" checkbox.

---

## 3. Effort scale used throughout

Effort is in **focused solo hours at the §2 baseline**, including tests, docs, and debugging — not just happy-path coding. Calendar translation at 10–15 h/week is given because calendar time is what kills motivation (a 3-week task at 12 h/week is a month of real life).

| Tag | Hours | Calendar (12 h/wk avg) | Examples from this project |
|---|---|---|---|
| **XS** | ≤ 8 h | ≤ 1 week | frame allocator, `Result<T,E>`, Tracy integration |
| **S** | 8–25 h | 1–2 weeks | SDL3 wrapper, input state snapshot, `.mce` reader |
| **M** | 25–70 h | 2–6 weeks | math library w/ DirectXMath oracle tests, reflection traits, 2D sprite batch on MDI |
| **L** | 70–160 h | 6–13 weeks | archetype ECS, RHI + GL backend, 2D physics solver, audio mixer + DSP, glTF importer + cook |
| **XL** | > 160 h | > 3 months | 3D PBR renderer + Forward+, editor maturity, runtime UI |

Note on notation: the roadmap document sizes whole *milestones* (S ≈ 2–4 wk … XL ≈ 14–22 wk); those are sums of the per-feature tags above and the two scales are not interchangeable. Milestone hours in the roadmap are the authority for gate planning; the table below is the authority for *feature-area* audits.

Calibration anchors from the literature: Handmade Hero took ~650 episodes (~500+ hours) to ship *one* 2D game with software rendering and zero editor; Box2D's solver (Catto's sequential-impulse design) is the core of a multi-year project; EnTT/flecs are specialists' multi-year works. Anything in MCE estimated under these anchors is probably miscounted.

---

## 4. Feature-area effort table (the scope audit)

Each row: what it includes at the **tier at which it first appears**, effort, and the *dominant* risk. Estimates assume the committed stack (GL 4.5 DSA, SDL3, ImGui) and the bindings (fixed timestep, frame allocator, handles, CI). Engineering hours only; content production is tracked separately in row 22.

| # | Feature area | Contents | Effort | Dominant risk |
|---|---|---|---|---|
| 1 | Project scaffolding | CMake presets, vcpkg manifest, CI (build+test+clang-tidy+ASan), target graph, PCH/unity | M | CI flakiness on windows runners; GL context in CI |
| 2 | Core runtime | `Result<T,E>`, logging, asserts/crash handler, frame linear allocator, pools, generational `Handle<T>`, convars, Tracy zones | M | None serious — scope-traps exist (a TLSF allocator is a **deferred** M2 item, not core) |
| 3 | Math library | vec/quat/mat (RH, Y-up, col-major, radians), transforms, SIMD *later*; DirectXMath as test oracle | M | SIMD temptation — scalar-first is a binding, enforce it |
| 4 | Platform layer | SDL3 window/events/file dialogs/time/mmap; platform/game module split (Handmade Hero boundary) | S | Small. Do not let "portability" creep in — Win10 x64 only is binding |
| 5 | RHI + GL backend | RenderDevice/CommandBuffer, DSA, persistent-mapped staging rings ×3 frame slots, fences, MDI, ≤1 s shader hot-reload; GL-symbol-leak CI check | L | Shader/feature creep through the seam; keeping it ~2–3k lines |
| 6 | ECS (archetype) | Entities, archetype tables, queries, phase pipeline, command buffers, change ticks, event queue, sparse-set location index, determinism harness | L | Over-engineering: observers, relationships, parallel scheduling are all *later* |
| 7 | Reflection + serialization | Macro + constexpr traits, inspector-first UI hints, binary scene serialization via reflection | M | Under-scoped by everyone; it serves editor + save/load + cook + ECS registration |
| 8 | 2D renderer | Extract→cull→sort→MDI sprite batch, texture atlas, stb_truetype text, cameras, sorting layers, DebugDraw | M | Text rendering is a quiet M by itself; atlas packing another S |
| 9 | Asset pipeline v1 | glTF 2.0 importer (**static meshes + materials only**), PNG/WAV decode, meshoptimizer at cook, `.mce` container (magic/version/CRC/16B-align), cook CLI, GUID registry, deterministic outputs | L | glTF full-spec rathole (skins/morph = +L); container versioning discipline |
| 10 | Fixed-step sim + input | Accumulator loop (Gaffer), interpolation, input sampled per tick with state-derived edges, action mapping layer | S | Determinism testing discipline |
| 11 | Editor v1 | ImGui docking shell, hierarchy, **reflection-driven** inspector, entity CRUD, scene save/load, translate gizmo (via ADR) | L | The #1 scope sink — see Risk R2 |
| 12 | 2D physics (rung-6 shape) | Swept AABB + circle, dynamic-vs-static solver, kinematic `move_and_collide` controller, sensors/queries, one-way platforms | L | Solver stability rabbit hole is *deferred*, not deleted — see Risk R5 |
| 13 | Audio v1 | SDL3 callback, voice pool + virtual voices, SPSC command FIFO, streamed music ring, cook-time resample/normalize (60 Hz command granularity) | M | DSP polish is unbounded — timebox reverb quality explicitly |
| 14 | 3D renderer (PBR) | Forward+ clustered, Cook-Torrance BRDF, CSM with texel snapping, sky, post chain (ACES/bloom/FXAA), debug draws, RenderDoc verification | XL | The #1 technical risk — see Risk R1 |
| 15 | Solver hardening (M3.5) | Warm-started sequential impulses, island traversal, restitution tuning, 20 hand-computed manifold cases | S–M | Named overrun absorber — slips whole into v0.5, never gates MVP |
| 16 | Job system | Counter/dependency fork-join, learned inside the cook tool first, cook parallelization | M | Tempting to pull into runtime early; binding says cook-first |
| 17 | Editor maturity | Undo/redo command stack, asset browser, rotate/scale gizmos, prefab instantiate/override, multi-scene, play-in-editor, timeline/curve editor | XL | Play-in-editor alone is an L; curve/node editors are v1.0-at-best |
| 18 | Packaging/shipping | Module packaging (`engine.dll`+`game.dll`), one-command zip ship, clean-VM soak, symbolicated crash dumps, project template | M | Small but *novel*; nobody plans it, everyone needs it |
| 19 | Runtime UI (mce::ui) | Retained scene graph, anchored rects, 9-slice, text, focus nav; rendered via 2D batch; RmlUi fallback trigger | L | Explicitly deferred debt (binding); design at M4, build v1.0 |
| 20 | IBL + 3D extras | Split-sum IBL, GTAO, 3D LUT grade, hi-res post | L | Pure garnish — first cut on every ladder |
| 21 | Scripting / skinned animation | Lua/sol2 glue; glTF skins → joint palettes, blend subset | L each | **Not in v1.0 scope** unless a shipped game demands it; cut candidates #1 and #2 |
| 22 | **Content production** | CC0 sourcing + kitbashing (Kenney 2D/3D packs, Poly Haven), level assembly (5 Ember Step levels, Ashen Court courtyard, UC-D slice), SFX/music selection + attribution pass, cooking integration | **M–L total, spread across tiers (≈ 220–340 h)** | The rev-1 omission. Under-planning here converts engine time into content time at exactly the wrong moment (Risk R10) |

**The honest total.** Rows 1–19 + 22 (everything except row 20–21's deferrables) is **≈ 1,080–1,740 h engineering + ≈ 220–340 h content — right at the 2–3 year ceiling with one slack band, not two.** Anything that goes wrong (and something will — Risk R7) must be paid for by the cut ladder in §7.2, not by overtime. This is the single most important sentence in this document: rev 1's ≈ 1,200–1,500 h single-number band both omitted content and under-counted the 2D gameplay stack; the corrected band is materially larger and the tier discipline is therefore not optional.

---

## 5. Top 10 scope risks — early-warning signs and pre-agreed cut-lines

Each risk: what it is, why it's real (named precedent), the early-warning sign (a measurable trigger), and the cut-line (what gets dropped, decided now so it isn't negotiated under fatigue).

**R1 — 3D renderer gold-plating (XL).** Shadows, IBL, Forward+, post FX, and "one more visual feature" can consume the entire project. Unreal's RHI exists because rendering expands to fill any budget; Hazel stalled *in* renderer work.
*Early warning:* the 3D timebox (M4/M5) week 8 without a lit, textured, orbit-camera mesh in RenderDoc; any Tracy frame where render features outnumber gameplay systems.
*Cut-line (pre-agreed order, matching `docs/research/rendering-3d.md` §9):* IBL → GTAO → post FX → cascaded shadows (keep single cascade) → Forward+ (fall back to plain 8-light forward) → PBR specular (fall back to Lambert+specular alias). The 3D demo ships with **diffuse+directional light+one shadow map** at minimum; everything else is garnish. Timebox breaches execute this order; they never extend (binding).

**R2 — Editor gravity well (XL).** The editor is the single largest subsystem and the most fun to polish. Undo/redo, gizmo UX, asset browser, prefab UX, timeline editors can eat a year.
*Early warning:* editor commits exceed 50% of total commits in any 4-week window outside M6/M8; or any inspector widget is hand-rolled per-component (Piccolo's drift failure — binding forbids it).
*Cut-line:* MVP editor = hierarchy + reflection inspector + save/load + translate gizmo. No undo until v0.5 (scenes are small; re-open the scene), no asset browser until v0.5 (drop `.mce` onto the window), no prefabs until v0.5 (copy-paste entities), no multi-scene until v1.0. Cut order within maturity work: timeline/curve editor first out, then asset thumbnails (ladder rung 1a/1b). If M3's platformer slips, the editor loses the gizmo (entities are placed by inspector fields) — *not* the game its inspector.

**R3 — glTF importer rabbit hole (L).** glTF 2.0's spec surface (skins, morph targets, KHR extensions, animation curves, sparse accessors) is a project-sized trap.
*Early warning:* the importer consumes > 25% of a milestone's hours; a second import path appears for an extension; > 10 unresolved extension warnings; cook time per asset exceeds 5 s.
*Cut-line:* importer supports **only**: static meshes, PBR metallic-roughness base material params, node transforms, node-transform animation tracks (v0.5), embedded or sibling buffer images. Closed extension allowlist (KHR_mesh_quantization in, Draco/basisu/meshopt-compression out); unknown extensions are *skipped with a cook warning*, never implemented on demand. Skinned animation is tier-late (§7) and skippable.

**R4 — ECS maximalism (L).** flecs/Bevy envy: relationships, observers, queries-of-queries, parallel scheduling, archetype chunking — each is a paper worth reading and a month worth not-writing.
*Early warning:* ECS commits with no gameplay feature using them; benchmark code with no game; `World::metrics` growth without a consumer.
*Cut-line:* MVP ECS = tables + queries + command buffers + phase pipeline + change ticks + determinism harness. **Not in MVP:** relationships/hierarchy edges (a `Parent` component + flat transform system first), observers (poll change ticks), auto-parallel scheduling (single-threaded is binding until M3+), chunked tables (dynamic tables first — per `docs/research/ecs-gameplay.md`). Change detection (`is_added/is_changed`) and `ChildOf` hierarchies are v0.5 rows, admitted only after the MVP gate is green.

**R5 — Physics solver depth (L).** Sequential impulses are straightforward until stacking, warm starting, and Baumgarte vs. NGS bias tuning eat weeks (read any Box2D issue tracker).
*Early warning:* solver-bug weeks > 3; any tunneling repro at Artifact A speeds; solver commits contain magic constants you can't explain.
*Cut-line:* **the MVP ships rung-6 physics by default** (a pre-decision, not a degradation): dynamic-vs-static AABB/circle contacts, kinematic swept `move_and_collide` player with platform carry via velocity accumulation, one-way platforms, sensors. Stacks of 3+ dynamic boxes are M3.5 (the tier's shock absorber, which may slip whole into v0.5 without gating MVP); joints are distance-only at v0.5; CCD for the player at v0.5. If M3.5 still fails its timebox, Artifact A ships with velocity clamping and an honest demo design that never stacks crates — Catto's own slides treat tuning as iterative.

**R6 — Determinism obsession (M → unbounded).** Determinism is binding and test-targeted, but it can be chased into unproductive corners (cross-machine float identity, hash-order paranoia in cook, `-fp:fast` debates in builds nobody ships).
*Early warning:* any week where determinism test infrastructure work exceeds gameplay work; the first cross-config hash mismatch opening a cross-machine investigation.
*Cut-line:* determinism is defined **narrowly**: same binary + same `.mce` inputs + same input stream ⇒ same entity state hash per tick, on one machine, in Debug and Release, over 10k ticks. That is testable and achievable (fixed timestep, `/fp:precise` in physics/sim TUs, no heap-order or hash-map iteration dependence, FlatMap for sim-visible iteration). Cross-machine/cross-driver bit-identical rendering is **explicitly out of scope forever**. The determinism test gates merge from M1 (binding) — which is exactly why its scope must stay narrow.

**R7 — Milestone slippage / the Hazel pattern (project-killing).** Ungated growth: "just one more feature before the artifact" — including one more *document* before the code.
*Early warning:* > 2 consecutive weeks with no runnable-artifact improvement; slip notes that carry scope silently; devlog (REQ-F-01) showing 3+ weeks below 6 h/week; docs/commit ratio above 20% after M0 starts; ADRs written for things that are not decisions.
*Cut-line:* binding already says every milestone ends in a runnable artifact with no silent scope slip. Feasibility adds teeth: **a milestone that misses its estimate by > 50% triggers the named overrun absorber** (M3.5 slips whole into v0.5; M4 cuts content scope per the ladder) — the MVP *gate* never moves. Two consecutive misses trigger a project-level scope review — options: extend the calendar honestly (the 3-year end of the horizon exists for this), or cut a subsystem (usually: runtime UI, then IBL/post, then editor maturity). After M0, the only standing documents are: REQUIREMENTS (updated at gates), the architecture doc (updated when structure changes), ROADMAP, ADRs (decisions only), devlog. No new research docs without a named decision they unblock.

**R8 — Build/CI/environment friction (M, recurring).** MSVC + vcpkg + GL-in-CI + ASan + clang-tidy is five Ways To Lose An Evening. The platform research already flags: no reliable ccache on MSVC, unity-build collision breakage, slow windows runners.
*Early warning:* any single week where > 4 h went to build system rather than engine; CI red > 2 days; PR CI > 30 min sustained; any unity collision needing > 1 h untangle.
*Cut-line:* CI runs build + unit tests + determinism + clang-tidy always; **ASan is a per-PR smoke with a weekly full job** if the per-PR cost exceeds 30 min; golden-image CI runs only on the dev machine (R9); the VS-generator preset is allowed to be temporarily broken during a Ninja-preset crunch; pinned-ports breakage has a pre-written vendored-SDL fallback ADR.

**R9 — Verification-infrastructure flakiness (M, sneaky).** Rendering the same scene on two GPU drivers never produces identical pixels (different shader compilers, FP rounding, sample patterns); ASan and clang-tidy each generate their own noise streams.
*Early warning:* a golden test failing only on another machine; time spent on epsilon-tuning image diffs; a clang-tidy backlog treated as merge-blocking.
*Cut-line (binding-compatible):* golden-image tests are **dev-machine-only**, compare against same-machine baselines with a perceptual threshold (block-mean luminance diff < 0.5%) rather than pixel equality, and auto-rebaseline via a one-line commit convention. Never gate CI on them; gate *merge of render features* on a human-eyeballed RenderDoc capture (binding) — that's the real safety net and costs zero flakes. clang-tidy gates on *new* code only, never on the existing backlog.

**R10 — Content production underestimation (M–L, the rev-1 blind spot).** "It's just a small demo" hides 20–40 h of sprite/tileset selection, kitbashing, SFX auditioning, and attribution bookkeeping per artifact — and the shame of shipping programmer art is a documented motivation killer for solo devs.
*Early warning:* content hours missing from a milestone's estimate table; "I'll make the art myself in Blender" appearing in the devlog; the MVP gate approaching with placeholder art.
*Cut-line:* **MVP content is sourced, not authored** — Kenney CC0 2D/3D packs and Poly Haven assets, plus freesound/JSFX SFX bundles, with a one-time attribution pass (≈ 15–25 h per artifact). No hand-modelled art anywhere in the MVP gate; any hand-modelled asset is a slip note, not a default. Each gate publishes a **content-hours table** next to the engineering one (REQ-G-09). If content time threatens the gate, cut content scope (fewer levels, smaller diorama) — never engine scope.

---

## 6. Minimum viable engine (MVE) — the smallest thing that ships a 2D demo AND a 3D demo

**Definition.** The MVE is the smallest set of MCE capabilities that produces **two artifacts a non-programmer can launch by double-clicking on the dev machine**, exercising the same engine code paths in both:

- **Artifact A — 2D demo (Ember Step, single-screen slice):** a single-screen 2D platformer: tile/terrain sprites, a player (run/jump, kinematic swept controller + dynamic-vs-static AABB physics), 2–3 enemy FSM behaviors, collectibles with trigger events, one SFX set + one music loop, start and win/lose states, HUD-free (win condition = an on-screen text sprite). Art from CC0 packs, fully arted, not programmer-prototypical. Ships as a zip: `game.exe` + `assets.mce` + SDL3 DLLs.
- **Artifact B — 3D demo (the lit diorama, Ashen Court's precursor):** one glTF scene (≥ 10k triangles, kit-assembled from CC0 3D kits), orbit/zoom camera via input actions, a directional light with one shadow map, fog, and a spinning animated prop (node-transform animation from glTF, not skinned). Same zip shape.

**Why both, and why these.** Artifact A exercises the full 2D + physics + audio + game-state pipeline and is reachable in months (Hazel's batched 2D renderer is the public proof-of-pace). Artifact B exists to force the 3D pipeline (glTF → cook → MDI, lighting, shadow, camera controls) *before* the project can drift into "2D engine forever" — the most common solo-engine failure shape after the stall. Both use identical loop/ECS/asset/editor-shell machinery, which is what makes them *one* engine rather than two demos. Structurally, Artifact B is an **addition, not a dependency**: the cut ladder's final rung (below) can reduce the MVE to Artifact A alone without invalidating any MVP work.

**MVE capability checklist (everything not listed is out of MVE by definition):**

| Capability | In MVE | Explicitly NOT in MVE |
|---|---|---|
| Window/input (SDL3), fixed 60 Hz loop + interpolation | Yes | gamepad (keyboard only), text input |
| Math, allocators, handles, Result, Tracy zones | Yes | SIMD, TLSF, job system in runtime |
| RHI + GL 4.5 backend (DSA, staging ring, MDI, ≤ 1 s shader hot-reload) | Yes | second backend, pipeline caches beyond GL programs |
| ECS subset | Tables, queries, command buffers, phase pipeline, determinism harness | hierarchy edges, observers, parallel scheduling, change-detection API |
| Reflection | Enough for inspector + scene save/load + ECS registration | full trait coverage, editor UI hints beyond name/type/range |
| Assets | glTF static meshes + PNG textures + WAV → `.mce`; cook CLI; CRC + version; memory-mapped load | BCn compression, animation, streaming, hot-reload of assets |
| 2D render | Sprite batch, atlas, one bitmap font | particles (a small CPU pool is allowed if the demo needs it), 2D lighting, post FX |
| 3D render | Forward, diffuse+specular (Cook-Torrance), 1 directional light + 1 shadow map, fog | IBL, Forward+, cascades, bloom, GTAO |
| Physics 2D | Kinematic swept controller, dynamic-vs-static, gravity, one-way platforms | stacking, joints, CCD, sleeping |
| Audio | SFX one-shot + streamed music loop through mixer → master | buses, DSP, reverb, sample-accurate scheduling |
| Editor | ImGui dock shell, hierarchy, reflection inspector, save/load scene, entity CRUD, translate gizmo | undo, asset browser, prefabs, multi-scene, play-in-editor |
| Content | Ember Step slice + diorama fully arted from CC0 packs | any hand-modelled asset |
| Tooling | Tracy from M0 (binding), RenderDoc per render feature (binding), unit tests for math/ECS/`.mce` reader, ASan smoke | golden-image in CI (dev-machine only, per R9), fuzzing beyond `.mce` negatives |

**MVE effort accounting (calibrated against §4):** rows 1–13 of the feature table, minus editor polish and asset-pipeline extras ≈ **500–700 h engineering + ≈ 60–90 h content ⇒ 10–14 months at baseline velocity.** This is *consistent with* the roadmap's MVP tier (M0–M4 + M3.5 stretch ≈ 510–810 h eng + 50–70 h content) — the MVE is not a new plan, it is the existing M0–M4 with the fat trimmed and named, plus its content bill made explicit.

**MVE acceptance criteria (measurable):**

- **REQ-F-10:** Artifact A runs on a machine without dev tools at ≥ 60 fps (frame time p99 < 16.6 ms in Tracy) with zero console errors, from a double-click.
- **REQ-F-11:** Artifact B loads its `.mce` scene from double-click to first frame in < 1.0 s (memory-mapped load, no runtime decode).
- **REQ-F-12:** Both artifacts terminate cleanly (no crashes, no ASan findings in a 10-minute soak).
- **REQ-F-13:** A non-programmer (named friend/family member) completes Artifact A without instructions beyond a README paragraph.

---

## 7. Feature tiers and requirements

### 7.1 Tiers

**Tier definitions**
- **MVP** = MVE + the minimum that makes it an *engine* rather than two demos (editor can author the 2D game; cook is deterministic; CI green with the determinism harness). Roadmap mapping: M0–M4, + M3.5 as stretch.
- **v0.5** = the 3D engine is real (PBR, shadows, Forward+), physics and audio are respectable, the editor stops fighting the user, and Ember Step ships whole. Roadmap mapping: M5–M7.
- **v1.0** = small complete games can be *shipped* by someone who is not the engine author, using only editor workflows, the project template, and documentation. Roadmap mapping: M8–M9 (UC-D may slip to a named v1.1).

**MVP tier** (target: months 0–14 at baseline velocity; = M0–M4 + M3.5 stretch)

| ID | Requirement | Acceptance criterion (measurable) | Effort |
|---|---|---|---|
| REQ-MVP-01 | Project skeleton: CMake 3.28+ presets (`msvc-debug/release/asan/ci`), vcpkg manifest, target graph `mce_core→ecs→rhi→rhi_gl→render→asset→runtime`, PCH+unity | Fresh clone → ≤ 3 commands → building and running editor shell in ≤ 15 min on the dev machine; CI green on windows-latest | M |
| REQ-MVP-02 | Core runtime: frame linear allocator, pools, `Handle<T>` generational, `Result<T,E>`, logging, crash handler (minidump + archived PDB), convars, Tracy frame/zone markers | Tracy capture shows named frame + ECS iteration zones; `--crash-test` dump opens in VS against archived PDBs; hot paths allocate 0 bytes from heap (asserted in debug) | M |
| REQ-MVP-03 | Math library: vec2/3/4, quat, mat3/4, transform composition, RH/Y-up/radians/col-major; DirectXMath oracle unit tests | ≥ 95% function coverage by oracle tests; property tests for inverse/compose round-trips within 1e-5 | M |
| REQ-MVP-04 | RHI + GL backend: buffers/textures/pipelines/commands, DSA-only, 3-slot persistent-mapped staging ring with fences, MDI, shader hot-reload ≤ 1 s; no GL symbol outside backend dir (CI grep) | CI GL-leak check passes; hot-reload of a fragment shader mid-run changes output within 1 s without restart; staging ring shows 0 sync stalls in Tracy at 60 Hz with 100k sprites | L |
| REQ-MVP-05 | Archetype ECS: entities, component tables, queries, phase pipeline (`Startup`/`FixedUpdate.Pre/Physics/Post`/`Render.Pre/Submit`), command buffers, change ticks, `World::state_hash` determinism harness | 100k entities × transform+velocity iterate+write in < 1.5 ms (single thread, Tracy-verified); determinism CI hashes 3 consecutive runs + Debug-vs-Release identically | L |
| REQ-MVP-06 | Reflection + serialization: macro+constexpr traits; binary scene read/write; reflection-driven inspector | Scene save→load→save produces byte-identical files (round-trip test); inspector renders any registered component with zero per-component widget code (enforced by review) | M |
| REQ-MVP-07 | Asset pipeline v1: glTF static import (meshes/materials/nodes), stb textures, WAV decode, TTF font atlas; meshoptimizer vertex-cache optimization; `.mce` container (magic/version/CRC/16-byte-aligned chunks); cook CLI; deterministic outputs | Cook twice from same sources → byte-identical outputs (CI test); `.mce` reader has fuzz + CRC-corruption negatives; 10k-triangle mesh mmap load < 5 ms | L |
| REQ-MVP-08 | 2D renderer: extract→cull→sort→MDI sprite batch (≥ 10k quads/frame), texture atlas, stb_truetype text, ortho cameras with pixel snap, layer sorting, DebugDraw MVP | 10k quads at 60 Hz p99 < 4 ms GPU submit (Tracy); one MDI per atlas page in RenderDoc | M |
| REQ-MVP-09 | Fixed-step sim + input: 60 Hz accumulator + clamp (Gaffer *Fix Your Timestep*), interpolated render transforms, headless mode, input sampled per tick from state snapshots with edge derivation | Determinism test: same binary + same synthetic input stream ⇒ identical entity state hash per tick across 3 consecutive runs (CI, from M1 onward) | S |
| REQ-MVP-10 | Editor shell: ImGui docking, hierarchy, reflection inspector, entity create/destroy, scene save/load, translate gizmo (ImGuizmo via ADR), editor-as-client of runtime API | Editing + save + reload + play yields identical scene state (hash) as before edit; `mce_runtime` compiles/links with **zero** ImGui includes (CI) | L |
| REQ-MVP-11 | 2D physics (rung-6 shape): kinematic swept `move_and_collide` controller (ground/wall/ceiling flags, one-way platforms, platform carry via velocity accumulation), dynamic-vs-static AABB + circle solver, sensors and queries | 2D demo platformer plays correctly (slopes, moving platforms, pickups via trigger events); contact manifolds validated by a unit suite of 20 hand-computed cases (M3.5); no tunneling at player speeds with fixed 60 Hz + swept queries | L |
| REQ-MVP-12 | Audio v1: SDL3 callback, 48 kHz float32 mixer, voice pool (8 real + unlimited virtual with priority stealing), one-shot + streamed loop, cook-time resample/normalize into `.mce`; 60 Hz command granularity | Audio callback does 0 allocations/locks (asserted); one-shot latency < 25 ms (loopback measured); music loop gapless (waveform comparison test) | M |
| REQ-MVP-13 | Input action layer: actions/bindings/maps, deadzone+curve, keyboard+mouse; gameplay never sees `SDL_Event` | Action-mapping unit tests with synthetic event streams; rebinding at runtime persists via reflection | S |
| REQ-MVP-14 | **Artifact A ships: the single-screen Ember Step slice, fully arted from CC0 packs** (see §6) | REQ-F-10/12/13 pass; content-hours table published (REQ-G-09) | — |
| REQ-MVP-15 | **Artifact B ships: the lit 3D diorama** (glTF static + node-transform animation, ≥ 10k kit-assembled CC0 triangles) | REQ-F-11/12 pass; triangle budget stated and verified at cook | L |
| REQ-MVP-16 | One-command ship v1: cook + package + zip (icon, version metadata); clean-VM soak | A fresh Windows 10 x64 VM runs both artifacts from the zip with no dev tools; 10-minute soaks clean | S–M |
| REQ-MVP-17 *(stretch)* | M3.5 solver hardening: warm-started sequential impulses, island traversal, restitution tuning, the 20-case manifold suite promoted to CI | 20-body stack settles < 1 s with no jitter; suite green in CI. *This row is the tier's designated overrun absorber: it slips whole into v0.5 and never gates MVP* | S–M |

**v0.5 tier** (target: months 14–24; = M5–M7)

| ID | Requirement | Acceptance criterion | Effort |
|---|---|---|---|
| REQ-V05-01 | 3D PBR forward renderer: Cook-Torrance GGX, ORM materials, directional + ≤ 8 point lights, 3-cascade texel-snapped CSM, sky + fog, ACES tonemap, bloom, FXAA; depth prepass; debug draw layer | Golden-image (dev-machine, perceptual threshold per R9) stable across 3 runs; RenderDoc capture reviewed before merge (binding); 1M-triangle scene at 60 Hz p99 < 12 ms on the dev GPU | XL |
| REQ-V05-02 | Forward+ clustered lighting (2.5D clusters, light list per cluster) | ≥ 256 point lights in one scene at 60 Hz; cluster build < 0.5 ms CPU (Tracy) | L |
| REQ-V05-03 | glTF node-transform animation playback (translation/rotation/scale tracks, linear interp) | Animated prop loops seamlessly; per-track timestamp seek works (cook stores normalized time) | M |
| REQ-V05-04 | Physics v0.5: warm-started solver islands, distance + revolute joints, sleeping, player CCD | 20-body stack settles < 1 s with no jitter (video + Tracy evidence); joints unit-tested against hand-computed constraint solutions | M |
| REQ-V05-05 | Audio v0.5: Master/Music/SFX buses, RBJ biquad filters, limiter on master, 48 voices, 3D spatialization, sample-accurate `play_at` scheduling | Bus graph render matches an offline reference mix (sample-compare within -60 dB) | M |
| REQ-V05-06 | ECS v0.5: change detection (`is_added/is_changed`), `ChildOf` hierarchy with dirty propagation, prefab cook + instantiate (no runtime inheritance) | Prefab edit re-cooks and updates all instances on reload; hierarchy propagation order test green | M |
| REQ-V05-07 | Editor v0.5: undo/redo (reflection-driven command stack), asset browser (drag `.mce`), rotate/scale gizmos, entity duplicate | Undo of any inspector edit restores byte-identical scene state; 10-step undo/redo of mixed edits passes round-trip test | L |
| REQ-V05-08 | Play-in-editor: embedded host with pause/step/slow-mo, all-or-nothing scene commits | Play→Stop→Play reproduces tick-0 state hash; enter play mode ≤ 1 s | L |
| REQ-V05-09 | Job system (counter/fork-join) used in cook first, then runtime culling if profiled | Cook of the MVP asset set is ≥ 2× faster than single-thread on the dev machine; runtime remains single-threaded unless a Tracy stage exceeds 25% of frame | M |
| REQ-V05-10 | **Full Ember Step ships** (5 authored levels, gamepad, particles, parallax, menu/pause/results, best-time persistence) | Product-draft UC-A acceptance checklist passes verbatim; 60 fps documented with budget table | — |
| REQ-V05-11 | **Ashen Court playable slice ships** (third-person controller via `World3D::move_character` + Jolt 5 behind ADR-0003, camera never clips, spatial audio, combat loop) | Product-draft UC-B v0.5 acceptance passes; frame budget table with ≥ 2 ms headroom | — |

**v1.0 tier** (target: months 24–36; = M8–M9) — *candidates, admitted only by slip-note if earlier tiers are green*

| ID | Requirement | Acceptance criterion | Effort |
|---|---|---|---|
| REQ-V10-01 | IBL (split-sum: SH diffuse + PMREM prefiltered specular + 256² BRDF LUT; all convolutions at cook) | Materials respond to an environment swap with plausible reflections (side-by-side golden images) | L |
| REQ-V10-02 | Post FX depth: 3D LUT grade, GTAO (ladder order: IBL first in, these first out) | Toggleable at runtime; < 1.5 ms GPU combined at 1080p | M |
| REQ-V10-03 | Project template + one-command new-project generator | `new_project.bat <name>` produces a compiling, cooking, shipping project in ≤ 10 min — and the v1.0 proof game is built through it, not around it | S–M |
| REQ-V10-04 | Editor maturity: prefab override UI, multi-scene tabs, asset thumbnails, timeline/curve editor (cut order: timeline first, then thumbnails — ladder rung 1) | Prefab edit propagates to all instances on reload; round-trip test covers override merge | XL |
| REQ-V10-05 | Runtime UI (mce::ui): retained scene graph, anchored-rect layout, 9-slice, text, focus/navigation, reflection data binding; RmlUi fallback per its pre-approved trigger | A shipped debug console renders through mce::ui with zero ImGui symbols in the runtime binary (CI symbol scan) | L |
| REQ-V10-06 | Packaging v2: symbolicated crash dumps for shipped builds, shipped debug console (convars over mce::ui), version metadata | Crash in a shipped build produces a symbolicated dump; console renders over the game | M |
| REQ-V10-07 | **UC-D: the proof game** — a vertical slice (one small level reusing Ember Step kits + one new mechanic) authored exclusively through the project template and editor workflows | Built without touching engine source except ≤ 2 gameplay systems; ships via REQ-V10-06 pipeline; passes the non-programmer double-click test | — |

**Explicitly deferred beyond v1.0 (named so they cannot sneak in):** scripting VM (Lua/sol2), skinned animation (unless Open Question 3 promotes it, displacing IBL + post FX per the ladder), streaming/world-partition, job-parallel runtime scheduling, live DLL hot-reload, Vulkan/D3D12 backend, networking, localization, mod support, terrain, navmesh/AI, GPU particles.

### 7.2 The cut ladder (decided now, executed without re-negotiation)

When time pressure hits, cut in this order — each rung is a pre-agreed answer to "if X slips, drop Y":

1. **Rung 1 — drop v1.0 niceties:** IBL, GTAO, LUT grade, prefab/override UI, multi-scene, timeline editor, asset thumbnails. The engine is v0.5-shaped. *Trigger:* any M5–M7 slip note.
2. **Rung 2 — drop editor polish:** undo/redo, asset browser, rotate/scale gizmos. Scenes are edited via inspector + save/load. *Trigger:* M3/M6 editor work exceeding its budget by 25%.
3. **Rung 3 — degrade 3D:** drop post FX → Forward+ (plain 8-light forward) → cascades (single shadow map) → shadows entirely; the 3D demo ships lit-diffuse. *Trigger:* first-lit-3D-scene timebox breach (binding).
4. **Rung 4 — degrade audio:** drop buses and DSP; mixer = voices → master limiter → device. Music + SFX still ship. *Trigger:* M3 audio threatening the 2D game's date.
5. **Rung 5 — degrade assets:** drop meshoptimizer cook step (ship raw quantized meshes), keep container + CRC. Cook determinism stays (it's cheap). *Trigger:* pipeline work threatening the MVE.
6. **Rung 6 — degrade physics:** dynamic-vs-static only, kinematic player, no sleeping, no joints. *Trigger:* solver instability consuming > 3 weeks. *(Note: rung 6 is now M3's default shape, not a degradation — see R5.)*
7. **Rung 7 — final cut line:** the MVE reduces to **Artifact A only**, with the 3D diorama becoming the *first* v0.5 milestone instead of part of the MVE. This is the "2D engine first, 3D later" shape — an acceptable, honest endgame, and the reason Artifact B is structured as an addition rather than a dependency. *Trigger:* the 2D game is not playable by month 12 of real time.

The ladder is one-directional. Rungs are re-adopted only when their triggering pressure is resolved *and* the next gate is green — never mid-milestone.

---

## 8. Global requirements (the feasibility constitution)

These apply across all tiers. They are the operational form of the bindings, phrased as measurable feasibility rules.

- **REQ-G-01 (runnable-artifact rule):** every milestone gate ends in a double-clickable artifact per binding. *Measure:* artifact exists, launches, and runs 10 minutes clean on a non-dev machine or clean VM.
- **REQ-G-02 (two-week visibility rule):** any engine subsystem that has not produced a *visible, runnable* change within 14 calendar days is descoped or cut to its next ladder rung. Visible = in the artifact, not in a test log. *Rationale:* the Hazel failure mode was invisible growth.
- **REQ-G-03 (timebox discipline):** every L/XL item in §7 carries a timebox in hours; a timebox breach triggers the §7.2 ladder automatically, and may only be overridden by a written slip note that also downscopes the next gate by one rung.
- **REQ-G-04 (one-path rule):** no subsystem ever has two live implementations (Unity's old/new-everything lesson, `existing-engines.md` §2.1). Replacing a subsystem means deleting the old one in the same milestone. *Measure:* CI grep for the old API name returns zero after the replace milestone.
- **REQ-G-05 (budget tables):** alongside the binding frame-time budget (16.6 ms), each gate publishes a per-subsystem **memory budget** table (Our Machinery practice): ECS heap, GPU allocations, audio buffers, cook scratch. *Measure:* table exists at each gate; Tracy memory plot reconciles within 20%.
- **REQ-G-06 (motivation guard):** the devlog records a "fun check" each Friday — did this week produce something you could show someone? Three consecutive "no" weeks invoke R7's scope review. *Rationale:* solo projects die of motivation before technology; id and Handmade Hero survived on continuous shippability.
- **REQ-G-07 (dependency freeze):** the closed dependency list is frozen for MVP; any addition requires an ADR *and* a §7.2 rung cost (adding a dependency must name what it displaces).
- **REQ-G-08 (estimate calibration):** at each gate, actual vs. estimated hours for the gate's items are tabulated; the next gate's remaining estimates are rescaled by the observed factor. First calibration point: M0.
- **REQ-G-09 (content budget rule):** every gate publishes a **content-hours table** (sourcing, assembly, attribution) next to the engineering table; MVP content is CC0-sourced (Kenney, Poly Haven, freesound/JSFX) and any hand-modelled asset requires a slip note. *Measure:* table present at each gate; MVP-gate artifacts contain zero hand-modelled assets absent a slip note.

---

## 9. Feasibility verdict

**The MVE (both artifacts, §6) is feasible for one developer at 10–15 h/week in roughly 10–14 months** (≈ 500–700 h engineering + 60–90 h content). It is genuinely thin: GL 4.5 DSA + SDL3 + ImGui removes the ceremony that makes raw D3D12/Vulkan bootstrap a cliff (per the technology panel), and every hard subsystem (ECS, physics, cook, RHI) has a first-class public reference to copy from (flecs/EnTT designs, Catto's solver notes, id/Unity cook precedent, Hazel's pacing for the 2D path).

**The full v1.0 vision is ≈ 1,080–1,740 h engineering + 220–340 h content — the 2–3 year ceiling with one slack band, not two.** Therefore the tiers and the cut ladder (§7) are not advisory: they are the mechanism by which the project survives its own ambition. The four highest-leverage commitments in this document are (a) the 3D diorama is admitted into the MVE as a *timeboxed, cut-order-declared* addition, (b) editor scope is capped by the reflection-driven inspector (no hand-rolled widgets, ever), (c) the two-week visibility rule (REQ-G-02) makes scope debt visible in weeks, not quarters, and (d) content production is planned, sourced, and budgeted as its own line — the single biggest correction from rev 1.

**Go/no-go checkpoints for the whole project** (beyond milestone gates): if Ember Step is not playable by month 12 of real time, the correct move per this document is rung 7 (ship 2D-only), not heroics. The project remains a success under every rung of the ladder — that is the point of the ladder.

---

## 10. Sources relied on (canonical; web search unavailable)

- Jason Gregory, *Game Engine Architecture* — subsystem scope/effort framing, layering.
- Glenn Fiedler, *Fix Your Timestep* (Gaffer On Games) — fixed-step sim requirement and determinism scope.
- Casey Muratori, *Handmade Hero* — platform/game split, incremental shippability, the realistic hour-cost of "from scratch."
- Erin Catto, Box2D GDC slides + solver history — physics effort calibration, solver constants, and the iterative-tuning reality of solvers.
- Fabien Sanglard, *Game Engine Black Book: id Software / Doom 3* — small-team scope discipline.
- Niklas Frykholm (Bitsquid) / Our Machinery blog — memory budgets, data-oriented scope.
- Bob Nystrom, *Game Programming Patterns* — scoping subsystems to patterns.
- Hazel (The Cherno) / Piccolo / Overload / Kohi — solo-engine pacing, stall modes, and the editor-drift failure mode.
- Unity/Godot/O3DE/Unreal public docs and postmortems — feature-tier reality checks (what an "editor" and a "cook pipeline" actually contain).
- flecs / EnTT documentation — ECS scope calibration (studied, not linked, per binding).
- Tracy / RenderDoc documentation — profiling and render-verification workflow costs.
- Kenney CC0 asset packs / Poly Haven — content-sourcing assumption behind the MVP content budget (REQ-G-09).
