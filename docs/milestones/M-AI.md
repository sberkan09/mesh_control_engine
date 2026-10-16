# M-AI — Game AI as data (milestone record)

**Status:** active (added 2026-10-16; steps 1-2 done). Plan: ROADMAP §M-AI; ADR-0045; Linear SBA-128 … SBA-133.

**Why:** MCE has the parts an enemy moves and senses with: grid A* and `PathAgent`, the navmesh and `NavAgent3D`
(M-GAMEPLAY3D), `Raycast3D` / `Overlap3D` sensors, and the FSM helper. What it lacks is the layer that decides.
Every AI past a few states is a C++ cascade of flags and timers that an agent or a designer cannot edit, preview
or check. Unreal ships behavior trees with a blackboard and an environment query system, Unity has a Behavior
package, and Godot users reach for a plugin. In MCE the decisions should be data the cook checks, run state the
hash, saves and replays see, and nodes that reach the kit with no code.

It needs no device. Every step gates headless in `local_ci.sh`.

**Not in scope:**
- GOAP planners.
- Machine-learned agents.
- LLM-driven NPCs (M10+, see the ROADMAP).
- Crowd simulation beyond what the navmesh agents already avoid.

## Steps

| # | Step | Linear | State |
|---|---|---|---|
| 1 | Behavior tree runtime: `.mcbt` cooked to `CHNK_BT`, typed blackboard keys, `AiBrain` and `Blackboard` components holding all run state, tasks by name (ADR-0045) | SBA-128 | done |
| 2 | Stock data tasks (move to a key or entity on the grid or the navmesh, look at, animate, play a sound, spawn, raise and wait for events) and blackboard observer aborts; the stock player runs trees from scenes | SBA-129 | done |
| 3 | Perception: `Perceiver` (sight cone and range, hearing, forget time) and `Stimulus` (a visible target with a team, noise events), line of sight through 2D and 3D rays, memory into blackboard keys | SBA-130 | not started |
| 4 | A utility selector with response curves and hysteresis, `AiTokenPool` for attack and role tokens, point queries scored by distance, line of sight and cover | SBA-131 | not started |
| 5 | The editor's Behavior Tree panel: tree, inspector, undo, cook validation; in play-in-editor the active path, results and blackboard, and a perception overlay | SBA-132 | not started |
| 6 | MCP `bt_create`, `bt_add_node`, `bt_set`, `bt_trace`; the user guide chapter; the demo and the gates | SBA-133 | not started |

## Step 1 — the behavior tree runtime (SBA-128)

| Piece | Where |
|---|---|
| `CHNK_BT`: `BtHeader`, `BtKey`, `BtNode`, `BtView::read` (checks the tree's shape), `write_behavior_tree` | `src/engine/asset/include/mce/asset/formats.h`, `src/engine/asset/src/formats.cpp` |
| `.mcbt` importer: one kind key per node, typed keys, errors at file:line:col | `src/engine/asset/cook/src/bt_importer.cpp` |
| `AiBrain`, `Blackboard`, `BtSignal`, `BtRegistry` (resolve, define, tasks), `ai_brains`, `bb_*` by name | `src/engine/kit/include/mce/kit/ai.h`, `src/engine/kit/src/ai.cpp` |
| `install_stock` installs it ahead of the path and navmesh agents; the editor, MCP and cook know the component and the asset type | `src/engine/kit/src/stock.cpp`, `engine_components.cpp` (three hosts), `asset_tools.cpp` |
| Tests: the cook and its errors, a truth table for every node kind, the blackboard by name, a guard from a scene, two runs hash the same, saves resume | `tests/kit/test_ai.cpp` |

**Design notes.**
- **Memory composites.** A running sequence or selector resumes at its running child. An alert set mid-patrol is
  seen when the patrol branch finishes and the root starts over (the test checks exactly this). Step 2's observer
  aborts are how a tree reacts sooner.
- **One root run per tick.** A finished root starts again on the next tick, and Repeat and Retry restart their
  child on the next tick. A tree never loops within a tick, and with 128 nodes and depth 32 its cost per tick is
  bounded.
- **The state fits two components.** `AiBrain` holds a byte per node (the result, plus a composite's current child)
  and 32 slots for timers, counters and task memory. `Blackboard` holds 16 numbers, 8 vectors and 8 entities, with
  a set bit for each. The cook assigns every slot, so the runner never allocates.
- **Timing is tick math.** Wait 0.5 s completes on its 30th tick at 60 Hz, and Timeout 0.25 s fails its child on
  the 15th, within a thousandth of a tick. Cooldown keeps "ready at" on the brain's clock, so it survives the root
  restarting.
- **Halts are explicit.** A timeout, a finished parallel or a restart calls each running task in the stopped
  subtree once with `halting` set.
- **A new tree resets the brain.** `AiBrain::tree_hash` is the hash of the cooked payload, so a hot-reloaded tree
  never reads old statuses.

## Step 2 — stock tasks and observer aborts (SBA-129)

| Piece | Where |
|---|---|
| `BtNode::abort` (`asset::BtAbort`: none, self, lower, both) and `BtNode::arg` (a task's string), CHNK_BT v2; the reader refuses a lower abort without a selector parent | `formats.h`, `formats.cpp` |
| `.mcbt`: a guard's `abort`; the stock tasks' named arguments, checked by the cook (keys of the right type, guids, numbers); `arg` for a game's tasks; the sounds and prefabs as dependencies | `bt_importer.cpp` |
| The observers: a guard re-checks its condition every tick; a selector lets an earlier lower-aborting guard take over | `ai.cpp` |
| The stock tasks `move_to`, `look_at`, `animate`, `play_sound`, `spawn`, `raise`, `wait_event`, `log`; `BtTaskCall::tree` and `arg()`, `bt_key_position` | `ai_tasks.cpp`, `ai.h` |
| `samples/ai_demo`: the stock player runs a guard that patrols on a navmesh, hears a bell's alarm and catches a thief (`ctest -R ai_demo_player`) | `samples/ai_demo` |
| Tests: the abort truth table, every stock task, the cook's errors, a chase that hashes the same on two runs | `tests/kit/test_ai_tasks.cpp` |

**Design notes.**
- **Observers poll, every tick.** Unreal re-evaluates a decorator when its blackboard key changes;
  here the guard's condition is checked on each tick the branch is relevant. A key change between
  ticks is seen on the next tick either way, and polling needs no subscription state in the save.
- **Lower aborts are checked by the selector.** A selector resuming its running child first checks
  its earlier children that are lower-aborting guards. The first that holds halts the running child
  and the selector restarts at it, in the same tick. Every selector on the running path does this,
  so a guard at the root interrupts a task any depth below.
- **The stock tasks are registered names.** `install_ai` registers them; a game can register its own
  function under a name to replace one. Their memory slot marks the first tick, because the agents
  and animators run after the brain and their state is still the previous tick's.
- **move_to chases through the agent.** An entity key becomes `NavAgent3D::follow`, so the navmesh
  re-plans as the target moves without the tree re-planning every tick. A halt clears `move_to` and
  `follow` (a `PathAgent` goes inactive).
- **raise reaches the sequences.** It pushes a `BtSignal` and a `SeqSignal`, so a cutscene's
  `wait_event` can wait on an AI beat; `wait_event` hears both.
