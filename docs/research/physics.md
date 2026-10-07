# Research: Physics — Fixed Timestep, Collision, Dynamics, and the Build-vs-Embed Decision

**Domain:** physics · **Project:** Mesh Control Engine (MCE) · **Date:** 2026-10-06
**Status:** Research → feeds `docs/architecture/physics.md`. Binding constraints from the stack decision apply: 2D physics is **hand-written** (swept shapes + sequential impulse per Erin Catto's Box2D GDC material; Box2D itself is studied, never linked), Jolt 5 is the **pre-approved 3D exception valve** (any embed still requires a written ADR before the first `#include`), fixed-timestep simulation at 60 Hz with deterministic-order solving is a test target, generational handles only across frames, zero heap allocation in hot paths, exceptions/RTTI off in the engine core, and every third-party dependency lives in the closed vcpkg manifest.

---

## 1. Scope and framing

Physics is the subsystem where "build from scratch" is most often called insane — and where it is also most instructive. The question this document answers is not "which physics library is best" but the split question:

1. **What does MCE write itself** — where is hand-rolling physics the fastest route to deep engine knowledge at acceptable correctness risk?
2. **What does MCE embed** — where does correctness (tunneling, stacking, mesh collision, character controllers) demand years of someone else's tuning?
3. **What does the boundary between the two look like**, so the embed can be swapped, upgraded, or quarantined without touching gameplay code?

The survey covers: simulation timing (§3.1), broadphase (§3.2), narrowphase and contact generation (§3.3), shapes and materials (§3.4), rigid-body dynamics and solvers (§3.5), CCD (§3.6), character controllers (§3.7), triggers/raycasts/queries (§3.8). The build-vs-embed evaluation (§4) covers custom, Box2D v3, Jolt 5, Rapier, PhysX 5, and the rest of the field. §5 commits decisions; §6 maps to milestones; §7 lists risks.

Out of scope: ragdolls/vehicles/cloth/soft bodies/destruction (explicit "Won't" in MCE requirements), GPU rigid-body simulation, networked physics, and multiplayer determinism across machines (MCE's determinism contract is same-platform, same-binary — see §3.1.3).

---

## 2. State of the art — what shipping engines actually do

The first and most important data point: **nobody at scale hand-rolls physics for shipped games anymore.** Jason Gregory's *Game Engine Architecture* (3rd ed., ch. 12) frames physics as the subsystem where even AAA studios overwhelmingly embed middleware rather than maintain their own solver. The current field:

| Engine / game | 2D | 3D | Notes |
|---|---|---|---|
| **Unity** | Box2D (built-in) | PhysX; **Jolt integration shipped in Unity 6** | Two independent worlds behind one component shape — exactly MCE's two-world model. |
| **Unreal** | (Paper2D = no 2D solver) | **Chaos** (in-house since UE4.26, replaced PhysX) | The cautionary tale: even Epic, with unlimited engineers, took years and still gated features for years after the swap. |
| **Godot** | GodotPhysics (in-house, rewrite 2020) | **Jolt is the default 3D physics as of Godot 4.4** (previously in-house GodotPhysics) | The most instructive recent datapoint: an open-source engine *abandoned* its in-house 3D solver for Jolt and shipped the swap as a headline feature. |
| **Bevy (Rust)** | — | **Avian** (formerly bevy_xpbd, XPBD-based) | The hobby-engine world *does* hand-roll — as a community curriculum, which is what MCE's 2D engine is. |
| **Horizon Forbidden West** (Guerrilla) | — | **Jolt** (in-house by Jorrit Rouwe, open-sourced) | Proof Jolt is AAA-ship quality, not a hobby library. Rouwe's GDC 2023 talk covers the architecture. |
| **Box2D v3** (2024) | itself | — | Erin Catto's ground-up rewrite: C API, island-based **multithreaded** solver, years of accumulated solver tuning. |
| **Rocket League / fighting games** | — | — | Psyonix's GDC talks on Rocket League's deterministic 120 Hz physics show the *demand side* for the fixed-tick discipline MCE already binds. |

Three trends define the current state of the art, and all three point the same way for a solo project:

1. **Islands + multithreading** (Box2D v3, Jolt, PhysX) — parallelism over independent contact islands. Real, but it *breaks naive float determinism* (island composition changes accumulation order) and buys nothing at MCE's scale (≤ 2,000 bodies, single 16.6 ms budget). MCE deliberately stays single-threaded (§5, D8).
2. **Substepping + soft constraints.** Catto's *Soft Constraints: Fulfilling the Dream* (GDC 2011/2014) formulation — a constraint as a damped spring with `gamma`/`beta` — subsumes Baumgarte stabilization, restitution, and springs in one mathematically clean form. Sequential impulse with these tools is still the shipping standard for rigid bodies; **XPBD** (Macklin, Müller, Chentanez — *Unified Particle Physics*, 2016; *Small Steps in Physics Simulation*, 2019) is the modern challenger and wins for deformables/ragdolls, but for game rigid bodies sequential impulse remains the shipping norm (PhysX, Box2D, Jolt all use impulse-based velocity solvers with position correction).
3. **Speculative contacts** as the mainstream CCD answer (Bullet, Box2D's margin, discussed in Paul Firth's speculative-contacts articles) — cheap, deterministic, tunable, at the cost of occasional ghost collisions at high margins.

The conclusion for MCE is the one *Game Engine Architecture* argues and Godot just demonstrated at scale: **embed for 3D correctness; hand-roll only where the curriculum lives.** For MCE, the curriculum lives in 2D.

---

## 3. Architecture survey — the physics engine, layer by layer

### 3.1 Time: fixed timestep and determinism

**The problem.** A variable-dt integrator makes simulation quality depend on frame rate: explosion behavior, tunneling distance, and solver stability all change with `dt`. Worse, it makes replays, determinism tests, and networking impossible.

**The canonical fix** is Glenn Fiedler's *Fix Your Timestep* (Gaffer On Games): run simulation at a fixed `h` (MCE binds 60 Hz) using an accumulator, clamp the accumulator to avoid the spiral of death, and interpolate *rendering* between the last two sim states:

```cpp
constexpr float kFixedDt = 1.0f / 60.0f;
float acc = frameDelta;                       // clamped, e.g. max 0.25 s
while (acc >= kFixedDt) {
    prev = cur;                               // snapshot pose ring
    world.step(kFixedDt);                     // deterministic: same inputs → same outputs
    acc -= kFixedDt;
}
float alpha = acc / kFixedDt;                 // renderer: lerp(prev, cur, alpha)
```

Successors refine, not replace, this: substepping the *solver* inside a fixed tick (2× substeps at 1/120 when stability demands, config-gated so the determinism hash never silently changes), and "fix without timesteps" hacks (recording per-frame dt) rejected outright — they reintroduce the exact nondeterminism the fixed step exists to remove.

**Determinism mechanics that actually matter on MSVC/x64** (same-platform, same-binary contract — cross-machine lockstep is out of scope and out of MCE's requirements):

- `/fp:precise` on every physics TU in *every* configuration; never `/fp:fast`, never `/arch:AVX*` in the determinism config (FMA contraction reassociates float math silently).
- Total iteration order everywhere: entity-id order on import, body-id order on solve, sorted output on queries. **No hash-map iteration in any physics path** — open pair sets are sorted vectors keyed by `(minId, maxId)`.
- No time-dependent state: sleep timers count ticks, not seconds.
- A `hash_state(seed)` hook folded into the engine-wide determinism harness, run Debug **and** Release every PR.

This is testable and cheap — but only if physics is single-threaded. Parallel island solving changes float accumulation order per island composition; adopting it later is a deliberate, ADR-gated determinism trade, not an optimization to slip in.

### 3.2 Broadphase

The broadphase answers "which pairs *might* overlap" in near-linear time. The candidates:

1. **Brute force O(n²)** — fine to ~200 proxies, useless beyond. Worth writing as the test oracle for whatever comes after (any broadphase must produce identical pair sets).
2. **Sweep-and-prune (SAP)** — sort proxies on one axis, prune on interval overlap; incremental re-sort exploits temporal coherence (near-sorted lists stay near-sorted), giving ~O(n) amortized. Classic source: I-COLLIDE (Cohen et al., 1995); PhysX shipped SAP variants for years. Weakness: worst cases with many same-axis-extent proxies (grids of same-size boxes — i.e., exactly tile-based 2D games) degrade toward O(n²) on one axis.
3. **Uniform grid / spatial hashing** — excellent when sizes are uniform (particles, tile maps), poor when size distribution is wide; cell-size tuning is a per-game liability.
4. **Dynamic AABB tree (BVH)** — the Box2D/Bullet answer: an incremental balanced binary tree of fat AABBs (margin + per-proxy motion-predictive fattening), leaf insertion by lowest-cost descent, subtree rebalancing on update. O(log n) queries (raycast via stack walk, overlap via descent), handles wildly mixed sizes, no per-game tuning, and it never needs a full rebuild. Cost: it is the most code (~600–900 lines with fuzz tests) and the classic home of subtle bugs (stale node links, refit-below-rebalance mistakes).

**Verdict for MCE:** the dynamic AABB tree, one implementation, from the first milestone — not an MVP throwaway grid plus a real broadphase later. At MCE's scale any of the four is *fast enough*; the tree is chosen because it is the field's converged general-purpose answer (Box2D, Bullet), because it is the same structure the narrowphase raycasts and shape casts walk, and because it removes per-game tuning. The brute-force path stays as a 30-line test oracle inside the unit tests, which is where brute force belongs.

### 3.3 Narrowphase and contact generation

This is where most homebrew engines die, because contact *manifold generation* (not detection) is the hard part. The pipeline per candidate pair:

- **Circle–circle:** center distance vs summed radii; 1 contact point. Trivial, and the base case for capsule work.
- **Circle/polygon and capsule/polygon:** closest-feature search (SAT over the polygon's face normals plus the circle center axis), clamp to the edge. 1–2 points.
- **Polygon–polygon (box is a 4-gon):** full **SAT** over face normals → choose **reference face** (most anti-parallel) → find **incident face** → clip incident face against the reference face's side planes → keep ≤ 2 points with penetration ≤ 0. This is Box2D's `b2CollidePolygons` shape, and Dirk Gregorius's GDC contact-creation talks are the canonical walkthrough of *why* the reference/incident construction produces manifolds stable enough to warm-start. Getting the clipping right (and the feature ids right) is the single highest-value exercise in the entire from-scratch physics curriculum.
- **Arbitrary convex hulls / 3D convex pairs:** GJK (Gilbert–Johnson–Keerthi, 1988) for separation, EPA (Expanding Polytope Algorithm) for penetration depth. Elegant, but *hard to make robust* (degenerate simplices, termination tolerance). Box2D deliberately avoids GJK/EPA in its hot path for polygons; Jolt uses GJK-based casts. MCE needs GJK only for **shape casts** (swept queries, character controller), not for resting contact in 2D — SAT + clipping fully covers the 2D shape vocabulary (§3.4).

**Contact identity for warm starting:** each manifold point carries a **feature id** (edge/vertex indices from the clipping step); next tick, manifolds are matched by feature id so accumulated impulses carry over. Without this, stacks crawl and jitter — warm starting is the single biggest stability-per-cost lever in sequential impulse (Catto, *Modeling and Solving Constraints*, GDC 2009/2010).

**Position correction** (drifting penetration out without adding energy): three known options — Baumgarte bias velocity in the normal impulse (simple, adds bounce), **split impulse / pseudo-velocities** (position error solved in a separate pass with its own velocity bookkeeping — Box2D 2.x's default), and NGS (nonlinear Gauss-Seidel position projection). MCE takes split impulse: it is the field's default, hand-computable, and does not inject energy into resting stacks.

### 3.4 Collision shapes and materials

**2D vocabulary (in-house, cooked where authored from meshes):** circle, box (OBB: half extents + offset + rotation), convex polygon (≤ 16 vertices, CCW; convex hull via Andrew's monotone chain at cook time), edge/chain segments for static level geometry. Capsule arrives free as a swept circle in the controller. This is exactly Box2D's proven vocabulary, deliberately *not* enlarged: concave shapes decompose to convex children at authoring, never at runtime.

**Materials:** density (mass/inertia computed from the shape — polygon mass properties are closed-form and belong in the hand-written math library's test suite), friction (Box2D mix rule `sqrt(a·b)`), restitution (`max(a,b)`, applied only above a velocity threshold so resting contacts stay quiet). One collider per body at MVP; compounds arrive later as child entities, reusing the scene hierarchy rather than inventing a fixture-list API.

**Units and convention:** meters-kilograms-seconds, Y-up, right-handed, radians — matching both Box2D's guidance (tuning degrades outside ~0.1–10 m) and Jolt's native convention, which means the 3D embed needs **no** unit or handedness conversion at the boundary.

### 3.5 Rigid bodies and the solver

The body is deliberately boring: pose, velocity, inverse mass/inertia, damping, flags — stored **SoA over generational ids inside the physics world**, not as ECS components. Three reasons, in weight order: the solver wants dense cache-friendly arrays, not archetype churn; pose/velocity must not be ECS-authoritative (physics mirrors poses *into* `LocalTransform` — a two-writer problem if velocities live in components); and stale-handle semantics (generation bump → lookup fails, never aliases) mirror the ECS's own handle invariant and are unit-tested the same way.

**Integration:** semi-implicit (symplectic) Euler — velocities first, then positions — the stable order every reference engine uses.

**Solving:** sequential impulse (a projected Gauss-Seidel over velocity constraints), per Erin Catto's GDC series, which is the entire published curriculum for this solver:

1. Broadphase pairs → narrowphase manifolds; match by feature id → warm-start impulses.
2. **Islands:** union-find over contact/joint-connected bodies — the solve and (later) sleep unit.
3. **Velocity iterations** (Box2D defaults: 8): per contact point, normal impulse `dn = -(vrel·n + bias)/kn`, accumulated and clamped ≥ 0; then Coulomb friction clamped to `±μ·dn` (the friction pyramid).
4. **Position pass** (3 iterations, split impulse): slop 0.005 m, max correction 0.2 m/step — Catto's constants verbatim, because they are 14 years of tuning, not taste.
5. Integrate.

**Alternatives, evaluated and rejected for MCE's core:** XPBD (better for deformables and very stiff stacks via substeps; a second solver paradigm with no shipping-game rigid-body track record to match impulse solvers — the wrong thing to learn first); Featherstone reduced-coordinate (articulation-grade, appropriate for ragdoll-centric engines like Haemimont's — overkill); position-based dynamics for cloth (out of scope). Sequential impulse is chosen because the literature (Catto's slides are effectively a free textbook), the reference implementation (Box2D source), and the tuning knowledge all concentrate there.

**Joints (v0.5):** distance and revolute only, with springs expressed through the soft-constraint `gamma/beta` formulation — which makes the spring case a parameter change, not new solver code. Motors, welds, wheels: only if a shipped game demands, via slip note.

**Sleeping:** islands track a tick-count idle timer; a fully idle island sleeps and is skipped in the solve. Cheap (it falls out of island traversal) and mandatory for the 2,000-body budget.

### 3.6 Continuous collision (CCD)

The bullet-through-paper problem: a 300 m/s projectile moves 5 m per tick — through any wall. The three mainstream answers:

1. **Substepping** — smaller `h` for fast bodies. Simple, predictable cost, bounded benefit.
2. **Speculative contacts** — inflate/narrow the solve against predicted motion; the margin catches approaching tunneling in the *next* step. Cheap, deterministic, no separate TOI pass; cost is occasional ghost collisions at margins (Bullet and Box2D both live with this).
3. **Time-of-impact (TOI) casting** — conservative advancement of the two bodies along their relative motion (Box2D's continuous path; Jolt uses per-body GJK-based shape casts against the broadphase during its step). Most robust, most code.

**MCE's committed answer:** the character controller sweeps *every tick by construction* (§3.7), which covers the entity that actually matters. Dynamic-vs-static tunneling is handled by the velocity clamp + fat-AABB predictive margins; flagged fast bodies (projectiles) get explicit shape casts at spawn/step — the simplest correct form of (3) — with speculative margins as the safety net for everything else. Full per-body TOI machinery is explicitly deferred (and expected never to be needed: no game in MCE's scope fires bullets that outrun 60 Hz sweeps of body-sized objects).

### 3.7 Character controllers

The most decision-dense topic in game physics, and the one with the clearest industry consensus: **players are kinematic, not dynamic.** A rigid-body player fights the solver on every slope, ships with uncontrollable mass caveats, and makes moving platforms miserable. The canonical shape is the **kinematic capsule moved by swept shape-casts** — Quake's collide-and-slide lineage, Box2D's character-body guidance, and exactly what Jolt's `CharacterVirtual` implements in 3D (the reference implementation; Bullet's `btKinematicCharacterController` is the canonical *negative* example, famous for subtle edge-case failures).

Per-tick shape for 2D (`move_and_collide`): horizontal sweep → vertical sweep → slope policy (walk the slope plane up to `maxSlopeAngle`, wall beyond it) → snap-down within a small distance (kills stair-bounce and downslope flight) → one-way platforms as a downward-sweep-only filter with a start-pose test → **moving-platform carry by explicit velocity accumulation** (the controller adds `platformVelocity · dt`), never by solver friction — friction-carry is the classic jitter source.

The discipline line that must not be crossed: **ground/wall/ceiling flags are instantaneous physics output; coyote time, jump buffering, and wall-slide timers are gameplay systems reading those flags.** Physics must never learn what a jump is.

### 3.8 Triggers, raycasts, and queries

- **Sensors/triggers:** broadphase pairs with no solver response. Events come from **pair-set diffing** (sorted open-pair set; added → `TriggerEnter`, removed → `TriggerExit`), which gives exactly-once semantics, deterministic ordering, and free synthesis of `TriggerExit` on body destroy. Never emit per-tick "is touching" floods.
- **Raycasts:** broadphase traversal (BVH stack walk, slab test per node AABB) → narrowphase refine on candidates → deterministic sort (`t` ascending, tie-break ascending body id) → filter (layer mask, exclusion span, predicate veto — the hook one-way drop-through uses). Closest-hit returns `optional<RayHit>`; all-hits land in caller-provided frame-allocated buffers. Zero heap, callable from any phase.
- **Shape casts** (swept volume queries) reuse the narrowphase pair algorithms with the shape moved along the sweep — and they are the *foundation* of the character controller, which is the design's nicest property: the controller is a query consumer with policy, not a second collision system.
- The same query service must back **editor picking** and gameplay — one implementation, two consumers, deterministic ordering in both.

---

## 4. Build vs embed: the candidates

The evaluation criteria, in MCE's priority order: (1) correctness of the resulting *games*, (2) learning value of what is hand-built, (3) velocity — time to a running physics world that supports a shippable 2D artifact, (4) fit with the committed stack (C++20/MSVC v143, no exceptions/RTTI in the core, vcpkg manifest, CI, determinism contract), (5) maintenance surface for one person over years.

### 4.1 Hand-write everything (custom 2D + custom 3D)

- **Cost:** Erin Catto's Box2D is the product of well over a decade of GDC-iterated solver research. A working prototype (circles, boxes, impulse solve, no warm start) is ~2 weeks; a *shippable* 2D engine (warm starting, feature-id persistence, sleeping, tunnelling edges, joint compliance) is realistically **3–6 months of spare-time polish after the prototype**, and the long tail never ends. 3D multiplies edge-case cost ~3× (the split-second call that killed Godot's in-house 3D solver: they embedded Jolt instead).
- **Learning value:** the highest of any option — this is the primary argument *for* hand-rolling 2D, and the reason the binding from-scratch list mandates it.
- **Correctness:** lowest, but *bounded and testable* in 2D's shape vocabulary — every behavior is hand-computable on paper, which is precisely what makes it a good curriculum.

### 4.2 Box2D v3 — the reference, not the runtime

Catto's 2024 rewrite: C API (trivially MSVC-clean), island-based **multithreaded** solver, the accumulated tuning of the 2.x decade, MIT-licensed, vcpkg-packaged. As a *runtime embed* it is the best 2D library in existence — and it is still rejected for MCE's runtime, because the binding from-scratch list makes 2D physics the hand-written centerpiece and the learning-curve argument is at its absolute strongest here (small shape vocabulary, tractable math, superb literature). But Box2D earns two committed roles:

- **Study reference:** source + GDC slides as the walking skeleton for the in-house engine, constants lifted verbatim (§3.5).
- **Test oracle:** linked **only into the test executable**, never into any runtime or shipped target — identical scenes stepped in both engines, state hashes and behavior compared within tolerance. This mirrors the math library's DirectXMath-oracle policy and needs an ADR (test-time-only dependency, same class as glslangValidator).

### 4.3 Jolt 5 — the 3D embed (pre-approved valve)

Jorrit Rouwe's Jolt 5: MIT, C++17/20, the physics of *Horizon Forbidden West*, now **Godot 4.4's default 3D physics** and shipped as a Unity 6 integration — the de-facto mid-size 3D choice of the current era. Strengths that map directly onto MCE's needs: `CharacterVirtual` (the best-documented kinematic character controller in open source — capsule-vs-trimesh sliding, internal-edge rejection, step offset, stick-to-floor), mesh/convex-hull shapes with ActiveEdges, per-body CCD, deterministic same-platform/same-binary when stepped single-threaded, no RTTI requirement and exception use guarded behind its own macros, vcpkg port available, units/handedness matching MCE's convention exactly. Costs, accepted honestly: a boundary to keep clean (CI grep, exactly like the GL-leak check), a pinned version to defend against churn, a due-diligence spike on build flags against `/EHs-c-`/`_HAS_EXCEPTIONS=0` *before* the first include, and a recorded plan B (a hand-written kinematic-only 3D capsule controller over MCE's own shape-cast API — §3.6 lifted to 3D — which satisfies the character-controller requirement's letter if the toolchain fights).

### 4.4 Rapier — rejected

Rust, the strongest of the new-generation solvers, with an XPBD-adjacent heritage and first-class determinism support. Non-starter here: embedding a Rust library into a C++/MSVC solo project drags in a second toolchain and an FFI boundary — precisely the two-language tax the stack decision rejected when it passed over the C# candidate. Its 2D/3D quality does not compensate; its learning value *for this project's C++ curriculum* is near zero.

### 4.5 PhysX 5 — rejected

BSD-3 open source since December 2022, and the historical AAA default (Unity/Unreal until Chaos). Rejected: enormous API surface and SDK weight for one maintainer, no 2D story at all, its differentiator (GPU rigid bodies) targets CUDA and million-body scenes MCE will never simulate, and bring-up time on the committed toolchain is the worst of the field. The correct engine to study for *API design* (scene queries, filtering) — the wrong one to link.

### 4.6 The rest — briefly

- **Bullet 3:** historically important, upstream effectively dormant, outclassed by Jolt in every dimension MCE cares about. Rejected.
- **ODE:** archaic solver, dated API. Rejected.
- **Havok:** proprietary, unavailable. Not a candidate.
- **GodotPhysics:** a reference for how an in-house engine looks, not an embeddable library.
- **Avian (Bevy):** the strongest XPBD rigid-body reference implementation; watchlist reading for solver ideas, not a dependency (Rust).

### 4.7 Comparison against MCE's criteria

| Criterion | Custom 2D | Box2D v3 embed | Jolt 5 embed (3D) | Rapier | PhysX 5 |
|---|---|---|---|---|---|
| Learning value (from-scratch goal) | **maximal** | none | low (integration only) | none | none |
| Correctness ceiling for shipped games | bounded, testable in 2D | **maximal 2D** | **maximal 3D** | high | maximal |
| Velocity to shippable 2D artifact | 3–6 mo to harden | days | n/a | days | n/a |
| Stack fit (C++20/MSVC, no-exceptions core, vcpkg) | full | full (C API) | good (flags spike + pin) | **fails** (Rust toolchain) | poor (weight) |
| Determinism control (same-binary 60 Hz) | **total** | high | high (single-threaded) | high | partial (GPU paths) |
| Maintenance surface for one dev | self-owned, bounded in 2D | zero | pinned-version watch | FFI + toolchain | large |

The row that decides it is learning value × stack fit: Box2D would *remove the curriculum* in the one domain where it is tractable, and Rapier/PhysX fail the stack outright — leaving the only coherent split: **hand-written 2D, embedded Jolt for 3D, Box2D as oracle.**

---

## 5. Recommendation for MCE

Committed decisions (mirrored into `docs/architecture/physics.md` D1–D8; the Jolt adoption must be recorded in `docs/adr/0003-embed-jolt-for-3d.md` before the first `JPH` include):

**D1 — MCE hand-writes the 2D physics engine (`mce_physics2d`), fully, from M1.** Scope: circle/box/convex-polygon(≤16)/edge-chain shapes; static/kinematic/dynamic bodies; dynamic AABB tree broadphase (one implementation, fuzz-tested, brute-force oracle in tests); SAT + reference/incident clipping with feature-id manifolds; sequential impulse with warm starting, split-impulse position correction, Box2D's literal default constants; island traversal + sleeping (v0.5); joints = distance + revolute (+ spring via soft constraints), nothing more; swept character controller (move-and-collide with slope policy, snap-down, one-way platforms, explicit velocity-accumulation platform carry); sensors via pair-diff events; raycast/shape-cast/overlap queries; debug draw. **Stacking is explicitly out of MVP scope** (the known solver-tuning rabbit hole; cut-ladder rung 6 pre-agreed: > 3 weeks of solver instability ⇒ dynamic-vs-static only + kinematic player). This is the single richest learning deliverable in the project: manifolds, constraints, islands, BVH, determinism — all bought once, in the dimension where they are tractable.

**D2 — MCE does NOT hand-write a 3D rigid-body engine. Ever, as a milestone deliverable.** The 3D curriculum was considered and rejected on the record: it duplicates the 2D curriculum at ~3× edge-case cost against an explicit requirements judgment ("hand-rolling 3D rigid bodies is a year of edge cases"). The only in-house 3D physics code is what the *query* API needs anyway (sphere/box/capsule sweeps for the camera boom) — which is collision, not dynamics.

**D3 — MCE embeds Jolt 5 for 3D (`mce_physics3d`), behind a facade, admitted by the pre-approved exception, recorded by ADR.** `mce_physics3d` is the only target in the repo allowed to include `Jolt/*.h` (CI grep, mirroring the GL-leak check). Jolt steps single-threaded (`JobSystemSingleThreaded`) on MCE's 60 Hz fixed tick with a fixed 4 MiB temp-allocator carve-out; `ContactListener` bridges into MCE event types shaped identically to the 2D ones; mesh/convex colliders are **cooked** (cook tool links Jolt, computes hulls + ActiveEdges offline, runtime deserializes from `.mce` — never runtime-built). The `CharacterVirtual` controller and camera sweeps are the 3D deliverable; dynamic knockable props are a v1.0 "Could," not a commitment.

**D3b — Jolt adoption triggers (write them into the ADR as conditions, expected at the M5 timebox):** any 3D game prototype needs (a) static/concave level geometry (trimesh), (b) a kinematic character controller against meshes, or (c) compound shapes — or the 2D curriculum milestone is complete and the 3D artifact demands stacking. Until one fires, Jolt is not included anywhere.

**D4 — Box2D v3 is a test-time oracle dependency, never shipped.** Linked only into `test_physics2d`; identical scenes stepped in both engines, compared within hand-documented tolerance (the oracle validates the in-house solver; it does not replace it). Requires an ADR under the cook/test-time dependency class (same class as glslangValidator: no runtime presence, no shipped binary).

**D5 — Rapier, PhysX, Bullet, ODE, Havok: rejected** with the reasons in §4 recorded (toolchain tax, weight, dormancy, availability). Re-evaluation requires a new ADR; the burden of proof sits with the embed, not the from-scratch default.

**D6 — The boundary is a facade with the RHI's discipline, applied to physics.** Gameplay code sees only `mce::physics` types: `World2D`/`World3D` facades (pimpl; no third-party types in any public header), MCE plain-struct components (`RigidBody2D`, `Collider2D`, `CharacterController2D`, …) that are reflection-registered with zero physics includes, typed double-buffered events, and queries returning `Entity` + MCE math types. Body ids are physics-internal generational handles — never stored in components, never serialized. One world per dimension, no fake common base class uniting them; 2D/3D API drift is caught by a compile-time mirror test that names both worlds' methods with identical spelling.

**D7 — Simulation contract.** Physics steps only inside the single `FixedUpdate.Physics` phase slot: IMPORT (change-tick-driven component diff, entity-id order) → STEP (fixed `h = 1/60`, asserted — calling `step` with any other dt is a hard assert) → EXPORT (poses mirrored into `LocalTransform`, controller flags written) → EVENTS (pair diffs + contact begin/end). Structural change only at command-buffer sync points; body mutation calls valid only from FixedUpdate (assert elsewhere); physics is authoritative for pose of bodied entities during play, and there is no physics world at all in editor edit mode. All physics TUs compile `/fp:precise` in every configuration; no `/arch:AVX*` in the determinism config; iteration order is total everywhere (no hash-map iteration in any physics path).

**D8 — Single-threaded through v1.0, on the record.** Parallel islands (Box2D v3's model, Jolt's job system) are deliberately not planned: they would break the float-accumulation-order determinism contract for zero benefit at MCE's scale. Sanctioned relief order if the 2,000-body budget is threatened: SIMD broadphase hotspots (scalar kept as oracle) → substep tuning → only then a determinism ADR for parallel islands.

**D9 — Testing and budgets.** The suite: hand-computed contact-manifold cases (table-driven, solved on paper first), solver behavior cases against hand-computed impulse solutions, controller suite (600-tick zero-slide platform ride, one-way, drop-through, slope-limit, snap-down), trigger exactly-once fuzz, query determinism (byte-identical hit sets, Debug and Release), 20-body stack settle test (v0.5), full-tick determinism hash over 10k ticks in Debug *and* Release, Box2D-oracle scene diffs (test build only), headless purity (zero GL calls). Performance: broadphase+narrowphase+solve for 500 mixed dynamic 2D bodies under 3 ms of the 16.6 ms budget, Tracy zones (frame, broadphase, narrowphase, solve, import/export) from M0 per the profiling binding.

**D10 — Debug surface from day one.** `debug_draw` (colliders, fat AABBs, contact points/normals, broadphase tree) fills a `DebugDrawSink` the editor renders — physics never calls the RHI. Every render feature that visualizes physics gets a RenderDoc capture before merge, per the graphics-debugging binding. An embed you cannot see is an embed you cannot trust.

### What the boundary buys

Because everything above the physics targets is written against facades, the Jolt embed is a **migration, not a rewrite** — exactly the property the RHI seam buys for graphics. Swapping Jolt for a future major version touches ~15 facade functions inside one target; replacing either world wholesale would not touch a single gameplay system. And the mirror-signature rule keeps 2D/3D games feeling like the same engine, which is a product requirement (UC-B), not an aesthetic one.

---

## 6. Milestone mapping and risks

| Milestone | Physics deliverable |
|---|---|
| **M3 (Artifact A, 2D game)** | `mce_physics2d` rung-6 shape: dynamic-vs-static contact response, swept character controller (one-way + platform carry), sensors, queries, dynamic AABB tree, events, debug draw, determinism hash from day one. Stacking/warm-start deferred to M3.5. |
| **M3.5** | Solver hardening: warm-started sequential impulse, feature-id persistence, island traversal, the 20 hand-computed manifold cases. |
| **M5 (3D)** | **Jolt ADR + `mce_physics3d`**: `CharacterVirtual` controller, camera sweeps, cooked mesh colliders, 3D events/queries, same-binary determinism test. Joints, sleeping, compound colliders (via child entities) in 2D. |
| **v1.0 (only if demanded)** | Dynamic 3D props through the same Jolt embed. |

Principal risks (full register lives in the architecture doc): the **solver-tuning rabbit hole** (mitigated by Box2D's literal constants, stacking out of MVP scope, and the pre-agreed cut-ladder rung); **Jolt build-flag friction** against the no-exceptions runtime (ADR due-diligence spike *before* the first include; recorded plan B: hand-written kinematic-only 3D controller over MCE's shape-cast API); **Jolt version churn** (exact pin in `vcpkg.json`, upgrades only via ADR, bounded by the ~15-function facade); **determinism drift** from FMA contraction or `/fp:fast` (physics TUs pinned to `/fp:precise` everywhere, hash test runs Debug and Release every PR); **event floods** from particle storms (pair-diff emission, per-tick caps, convar); **transform-authority fights** (debug assert on illegal `LocalTransform` writes to bodied entities).

---

## 7. References

- Glenn Fiedler, "Fix Your Timestep!" — Gaffer On Games; the fixed-step accumulator + interpolation contract, and the networking-series determinism context.
- Erin Catto, Box2D GDC slide series — *Sequential Impulse*, *Modeling and Solving Constraints* (2009/2010), *Soft Constraints: Fulfilling the Dream* (2011), the dynamic AABB tree and character-body talks; Box2D 2.x source and the v3 (2024) C rewrite README. Study reference and test oracle; **never linked at runtime**.
- Dirk Gregorius, GDC talks on SAT, contact creation, and warm starting — the reference/incident-face manifold construction.
- Christer Ericson, *Real-Time Collision Detection* (Morgan Kaufmann, 2005) — the narrowphase bible: slab tests, clipping, hulls.
- Gilbert, Johnson & Keerthi (1988) — GJK; used by MCE only for shape casts.
- Jason Gregory, *Game Engine Architecture*, 3rd ed., ch. 12 — physics subsystem scope and the middleware-embed framing.
- Ian Millington, *Game Physics Engine Development* — the from-scratch pedagogy reference.
- Jolt Physics (Jorrit Rouwe, GitHub) — architecture docs, `CharacterVirtual` samples; Rouwe, "Architecting Jolt Physics for 'Horizon Forbidden West'," GDC 2023.
- Godot 4.4 release notes (Jolt as default 3D physics); Unity 6 Jolt integration — evidence the 3D embed is the field's current default.
- Macklin, Müller & Chentanez, *Unified Particle Physics for Real-Time Applications* (2016) and *Small Steps in Physics Simulation* (2019) — XPBD; evaluated, rejected for MCE's rigid-body needs.
- Paul Firth, speculative-contacts articles; Bullet's speculative CCD — the CCD design space (§3.6).
- Randy Gaul, "How to Create a Custom Physics Engine" — the best from-scratch 2D tutorial series; useful on-ramp, not an authority on solver tuning.
- MCE internal: `docs/architecture/physics.md` (decisions D1–D8, test strategy, budgets), `docs/REQUIREMENTS.md` §3.6 (FR-PHY-010..100), `docs/research/existing-engines.md` L6 (fixed timestep), `docs/research/ecs-gameplay.md` (phase pipeline, command buffers, event queues).
