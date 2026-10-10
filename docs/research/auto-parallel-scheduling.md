# Auto-parallel system scheduling: experiment and outcome

*2026-10-10.* The ROADMAP's deferred "auto-parallel scheduling experiment (M3+ flag, gated by
FR-RUN-040)" was built, measured and **not merged**. Systems stay serial on the main thread
(ecs-gameplay.md §4.1). This page records what was tried, the numbers, and what would reopen it.

The prototype is commit `03ed48b` ("experiment(ecs): flag-gated auto-parallel system scheduling")
on the `claude/auto-parallel-scheduling-cg1fw6` branch. Its merge into `dev_claude` keeps the commit in
history, and the next commit on that branch reverts it, so `git show 03ed48b` still shows the
code and `tests/ecs/test_parallel.cpp`.

## What was built

- **`SystemAccess`**, a per-system declaration: `reads<Ts...>()`, `writes<Ts...>()`,
  `resources<Ts...>()`, `events<Ts...>()`, `commands()`. It is attached with
  `World::set_system_access(phase, name, access)`. `SystemFn` is a bare `void(*)(Frame&)`, so
  there is no query signature to derive sets from. The plan in ecs-gameplay.md §4.1 ("derive
  per-system read/write sets from query signatures") does not fit the shipped API. Every access
  has to be declared by hand.
- **`World::set_parallel(N)`**, off by default. N worker threads plus the main thread run each
  phase as *contiguous batches in registration order*. A system joins the open batch only if it
  is declared, conflicts with no member, and does not name a member as its `after`. Conflicting
  pairs therefore keep their serial order, and only commuting systems overlap.
- **Determinism by construction.** Member k of a batch gets change tick `first + k`, the same tick
  serial execution gives it. The "who is running" state (system id, write tick) moved from World
  fields into a per-thread run context. Workers never touch the Commands stream or the entity
  allocator.
- **Safety rules found on the way.** `Commands::spawn` reserves the entity handle *immediately*
  (`generations_`/`locations_` can grow), so a system that records Commands cannot overlap any
  system that looks entities up. Such a system runs alone. Registering a new component type or
  resource inside a batch is a registry insert, so it asserts. Resources cannot be split into
  read and write, because `try_resource<T>` hands out a mutable pointer. Two systems that touch
  the same resource always conflict.
- **Declaration checks.** In assert builds, a declared system's queries, `try_get`,
  `try_get_mut`, `set`, `touch`, resources, events and commands are checked against its
  declaration. Type-erased access (`get_raw`/`set_raw`) and arbitrary `f.world()` use are not
  checked.

## Results

Linux container, 4 vCPU (Xeon 2.1 GHz), `linux-gcc-release`, 3 workers + main thread.

**Determinism (FR-RUN-040).** A world with 8 systems over 3 phases (spawning through Commands,
change filters, events and shared resources) gives the same `state_hash`, change-filter counts,
event stream and final `change_tick` with 0, 1 and 3 workers. It held over 30 repeated runs.
The full `test_ecs` suite is clean under ThreadSanitizer. TSan did catch a real race in the
first version of the worker pool: a late worker could still be reading the previous batch's job.
The fix makes every worker check in before a batch ends.

**Synthetic load** (4 declared, mutually independent systems, one component each):

| Case | Serial µs/tick | Parallel µs/tick | Speed-up |
|---|---:|---:|---:|
| Game-sized: 64 rows, one add | 0.40 | 3.28 | ×0.12 |
| Game-sized, 60 Hz paced (16 ms idle gaps) | 4.17 | 192.15 | ×0.02 |
| 10k rows, one add | 37.07 | 20.63 | ×1.80 |
| 100k rows, one add | 388.86 | 151.23 | ×2.57 |
| 1k rows, 16 trig calls each | 330.55 | 100.36 | ×3.29 |
| 1k rows, 16 trig, 60 Hz paced | 343.91 | 230.93 | ×1.49 |
| 20k rows, 16 trig | 6385.25 | 2088.07 | ×3.06 |
| 100k rows, 16 trig | 31033.62 | 10834.09 | ×2.86 |

The mechanism works once each system does tens of microseconds of work. Below that, the hand-off
costs more than the work. At a real game's 60 Hz pacing, waking sleeping workers costs about
0.2 ms per phase, which is 45× the whole serial tick of the game-sized case.

**The engine's own games:**

- **Ashen Court** (`-headless 1 -ticks 1800`): the whole fixed-step sim, physics included, takes
  **11–14 µs/tick**. Its 8 `FixedUpdate_Pre` systems are one `after` chain.
- **02_boids** at 20k boids: 0.2 ms/tick, dominated by `steer`, which reads the grid that
  `grid_build` (its `after` target) writes. `integrate` and `wrap` are alone in their phases.
  There is nothing to overlap.
- Across samples, projects and the kit, 36 of 62 `add_system` calls name an `after` target.
  Nearly every phase with more than one system is a strict chain, and the games share state
  through one game-state resource and Commands. Even with every system declared, almost every
  batch would hold one system.
- The serial path with the flag off costs nothing measurable: boids at 0.16–0.22 ms/tick
  before and after, with identical hashes.

## Decision

**Not merged.** It is safe in the sense FR-RUN-040 needs, but it gives no win for any game MCE
has. The best case hides microseconds inside a 16.6 ms frame, and the realistic case (60 Hz pacing,
tiny systems) is slower. It would also cost:

- a second scheduling model, which is the "old/new systems" pattern P6 rejects;
- hand-written access declarations that can drift from the code (they are checked only in assert
  builds and only for typed access);
- module code running off the main thread, which breaks the rule in ARCHITECTURE.md §6.1 and
  scripting.md §4.2 ("module code never leaves the main thread");
- a per-thread lookup on every change-tick stamp.

## What would reopen it

A shipped or in-progress game whose fixed-step systems exceed about **2 ms/tick**, with **two or
more independent systems of at least 50 µs each in the same phase**, shown by
`World::set_profiling` or Tracy. Before reopening system-level overlap, try these:

1. **Data parallelism inside one heavy system** (a `par_each` over table rows on `mce::jobs`, with
   per-row writes only). The boids `steer` shape is where the time actually goes, and it needs no
   access declarations.
2. If system overlap is still wanted: Commands buffers per batch with entity reservation deferred
   to the sync point, read-only resource access (`resource<const T>`), and declarations generated
   from an `MCE_SYSTEM` signature instead of written by hand.
