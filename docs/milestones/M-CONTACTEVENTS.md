# M-CONTACTEVENTS — 3D contact events (design)

**Status:** design, written 2026-10-23 before the code. Owner: the gameplay thread (physics3d steps 1–2, kit steps 3–5). Device-free.
Source: the open items of [M-VEHICLEHIT.md](M-VEHICLEHIT.md) ("a zone is not a contact", "crash damage is not part of this step").

## Why

`World3D` (`src/engine/physics3d/include/mce/physics3d/world3d.h`) has bodies, joints, ragdolls, vehicles and read-only queries, but reports
nothing about collisions. The 3D "triggers" are not Jolt sensors either: `kit3d_triggers` tests body centres against `Trigger3D` volumes and
`kit3d_sensors` runs overlap queries, both in the kit after the step. So gameplay that should follow a real hit (a car into a wall or another
car, a crate knocked over, an impact sound) has nothing to read, and `kit_vehicle_ram` guesses with a zone in front of the car. 2D already has
the shape we want: `physics::ContactBegin2D { a, b, point, normal, approach_speed }`, sorted, capped (`max_contact_events_per_tick`), pushed
by the physics module in `FixedUpdate_Physics` and counted by `PhysicsEvents` / `FireOn::ContactBegin` in `effects.cpp`.

## What the backend can report, and what it costs

- Jolt Physics 5, stepped by `World3D::step` → `PhysicsSystem::Update(1/60, 1 collision step, temp, JobSystemSingleThreaded)`; no
  `ContactListener` is installed today (only vehicle step listeners). `PhysicsSystem::SetContactListener` gives `OnContactValidate`,
  `OnContactAdded(body1, body2, manifold, settings)`, `OnContactPersisted(...)` and `OnContactRemoved(SubShapeIDPair)`.
- `OnContactAdded` fires once per new body pair / sub-shape manifold, in the narrow phase, **before** the solver: we get the world-space
  normal (`mWorldSpaceNormal`, from body1 to body2), the contact points and both `Body&` (velocities, inverse mass), not the solved impulse.
  So we report the **approach speed** (relative point velocity along the normal) and an **impulse estimate** `speed × reduced mass`
  (static/kinematic side = infinite mass). Good enough for damage tiers and sound volume; the solved impulse would need a post-solve pass
  over `ContactConstraintManager` (not public API) — not worth it.
- `OnContactRemoved` gives only body IDs (the body may be gone); not used in v1 (no `ContactEnd3D`).
- Cost: one virtual call per new manifold, plus one per persisting manifold for `OnContactPersisted` (the base no-op is still dispatched
  once a listener is set). We return in O(1) when neither body opted in (user data → `slot_of` → a per-slot flag byte), never lock, never
  touch `ContactSettings`, so a world with no opted-in body simulates and hashes bit-for-bit as today. Budget check in `tests/physics3d/test_budget.cpp`.
- Characters (`CharacterVirtual`) are not bodies and never reach this listener; crowd pedestrians have no physics body at all.

## What

**Physics layer (`mce::physics`).**
- `struct ContactEvent3D { Entity a; Entity b; vec3 point; vec3 normal; f32 approach_speed; f32 impulse; }` — 48 bytes, padding-free,
  `static_assert`ed. `a < b` by (index, generation); `normal` points from `a` to `b` (flipped when Jolt's body1 is our `b`); `point` is the
  manifold's deepest point on `a`, world space (after `shift_origin`, a consumer one tick late sees the old origin: same as every pose).
- `World3D::set_contact_events(Entity, f32 min_speed)` (opt-in per body; `min_speed < 0` switches it off), `Span<const ContactEvent3D>
  contact_events() const` (the last step's, cleared at the start of `step`), `PhysicsConfig3D::max_contact_events_per_tick = 256`.
- A pair is reported when at least one body opted in and `approach_speed >=` the smaller opted-in `min_speed`.
- **Ordering:** collected in callback order, then `std::sort` by `(a, b)` and de-duplicated per pair (keep the highest speed of the step's
  manifolds) at the end of `step`, then capped. Jolt's single-threaded, CROSS_PLATFORM_DETERMINISTIC order is already stable, but the sort
  makes the output independent of it (and of a future multi-threaded job system).
- **Per-pair throttle:** a sorted `Array<u64>` of `(lo slot << 32 | hi slot, last tick)`; the same pair is not reported again for
  `kContactRehitTicks` (6 = 0.1 s) unless it is at least twice as fast (a bouncing crate makes one sound, a second crash still counts).
  Entries expire after the window; `destroy_body` drops its pairs. `hash_state` folds the throttle list only once a world has had an
  opted-in body (`had_contact_events`, the `had_joints` pattern), so existing replay hashes keep.

**Kit layer (`mce::kit`, `bodies3d.h`).**
- `ContactBegin3D` = the same 6 fields, `MCE_EVENT(ContactBegin3D, a, b, point, normal, approach_speed, impulse)`, registered with producer
  `FixedUpdate_Physics`. Events are double-buffered and never saved or hashed (`World::state_hash` folds component tables only); what a
  consumer does with them (Health3D) is hashed, hence the sort.
- Opt-in on the component without a size change: `RigidBody3D::pad_` becomes `u8 contact_events` and `pad2_` becomes `f32
  contact_min_speed` (default 2 m/s); `sizeof(RigidBody3D) == 112` stays, old saves load (both were zero; 0 = off).
- System `kit3d_contacts` (FixedUpdate_Physics, after `kit3d_writeback`, beside `kit3d_triggers`) syncs the flag to `set_contact_events`
  on change and pushes `contact_events()` into the queue. FixedUpdate_Post readers see them the same tick; FixedUpdate_Pre readers see them
  the next tick (the front buffer lives until the next Physics swap), which is where `apply_damage` must be called from.

## Steps (stacked PRs, each with `changelog.d/<ticket>.md`)

1. **Seam: `world3d.h` declarations** (physics3d header only). `ContactEvent3D`, `set_contact_events`, `contact_events`, the config field;
   `world3d.cpp` stubs (empty span, flag stored). `world3d.h` reaches ~202 of 839 TUs (`include_fanout.py` warns above 150), so this ships
   alone and merges first; nothing else in the header changes. Test: `tests/physics3d/test_contacts.cpp` — empty span after a step, unknown
   entity is a no-op, `sizeof(ContactEvent3D) == 48`.
2. **Physics: the Jolt listener** (`world3d.cpp` only). `ContactListener` in `Impl`, per-slot flag + `min_speed`, collect / sort / dedupe /
   throttle / cap in `step`, `hash_state` folding. Tests in `test_contacts.cpp`: sphere dropped on a static box → one event, normal up,
   speed ≈ √(2gh); not opted in → none; below `min_speed` → none; resting for 120 ticks → still one; bounce inside 0.1 s → one; three pairs
   created in reverse order → sorted by `(a, b)`; cap honoured; two runs → equal event lists and `hash_state`; a world without opted-in
   bodies keeps the hash of `master`. `test_budget.cpp`: 500 stacked boxes with the listener installed, no opt-in, within the current budget.
3. **Kit: the event** (`bodies3d.h/.cpp`, `effects.cpp`). `ContactBegin3D`, the two `RigidBody3D` fields, `kit3d_contacts`;
   `PhysicsEvents` counts 3D begins (`contact_begins`, `last_contact`, `contact_speed`; its point/normal stay 2D) and `FireOn::ContactBegin`
   fires on them, so impact sounds and particles need no code. `bodies3d.h` reaches 41 TUs. Test: `tests/kit/test_contacts3d.cpp` (a scene
   crate falls on the floor → one event, `AudioEmitter` with `FireOn::ContactBegin` fires once); determinism subject for kit bodies3d.
4. **Kit: crash damage** (new `kit/impact_damage.h/.cpp`, `vehicle_ram.cpp`). `ImpactDamage3D { f32 min_speed; f32 damage_per_ms;
   f32 impulse_scale; u8 enabled; u8 pad_[3]; }` (16 bytes, saved) on whatever should take damage from its own contacts; `kit_impact_damage`
   (FixedUpdate_Pre) reads last tick's `ContactBegin3D`, damage `(speed − min_speed + 1) × damage_per_ms` (the vehicle_ram formula) via
   `apply_damage`, instigator = the other entity or its `VehicleSeat3D` driver, once per pair per second via `Health3D::since_hit`.
   `kit_vehicle_ram` skips targets that have `ImpactDamage3D` (no double hits) and **stays** as the path for pedestrians and characters.
   Test: `tests/kit/test_impact_damage.cpp` (car into wall loses health, slow nudge nothing, driver is instigator, ram + impact = one hit).
5. **Optional polish:** `CharacterContactListener` in `move_character` (characters hit by bodies) and a capsule body for crowd pedestrians;
   only then can the vehicle_ram zone retire. Separate milestone if wanted.

## Open items / risks

- **Threading.** Jolt calls listeners from physics jobs, concurrently under a multi-threaded job system and under body locks. Today it is
  `JobSystemSingleThreaded`, so the collector is a plain `Array`; the listener uses only the `Body&` arguments (no `BodyInterface`, which
  would lock) and `MCE_ASSERT`s it runs on the stepping thread. A threaded job system later needs per-thread buffers; the sort already
  makes the result order-free.
- **Wide header.** Step 1 rebuilds ~200 TUs once; steps 2–5 touch only `.cpp` files and narrow headers. `kit3d.h` (186 TUs) is not touched.
- **Replay hash.** Unchanged for scenes without opt-in (no listener effect, throttle not folded). The throttle list is not saved: after a load
  a pair touching within 0.1 s can report once more. Acceptable; saving it would add a resource to the save format.
- **Pre-solve speed.** A sliding scrape that becomes a hard hit without a new manifold reports nothing; `OnContactPersisted` with a speed
  jump could cover it later (cost: work on every persisting manifold).
- **Compound / mesh bodies** give one manifold per sub-shape; the per-pair dedupe in step 2 keeps one event per body pair per step.
- **Ragdolls.** Only `create_body` sets a body's user data (`pack(e)`); ragdoll parts carry 0 and are skipped in v1. Mapping a part to
  its ragdoll's owner (a part slot table) is a follow-up if hits on a ragdoll should hurt. Vehicles are ordinary bodies and work.
- No `ContactEnd3D` in v1; add it (from `OnContactRemoved` + the throttle list) when a consumer needs "stopped touching".
