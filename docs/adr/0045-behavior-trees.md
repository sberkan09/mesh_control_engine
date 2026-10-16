# ADR-0045 — Behavior trees as data: .mcbt, AiBrain and Blackboard

**Status:** Accepted (2026-10-16). **Milestone:** M-AI step 1 (off-tier, opt-in). **Plan:**
`docs/milestones/M-AI.md`. **Reference:** `src/engine/kit/include/mce/kit/ai.h`,
`src/engine/asset/include/mce/asset/formats.h` (CHNK_BT), `src/engine/asset/cook/src/bt_importer.cpp`.

**Context.** MCE games build enemies and NPCs out of the FSM helper (`kit/fsm.h`), the grid and navmesh agents, and
C++ systems. That works for a grunt with four states, but every AI past that becomes a hand-written cascade of
flags, and none of it is data an agent or a designer can edit, preview or check. Unreal ships behavior trees with a
blackboard, Unity has a Behavior package, and Godot users reach for a plugin. MCE's edge is determinism and MCP
reach, so the AI layer must be data the cook checks, state the hash and saves see, and nodes that reach the kit
with no code.

**Options weighed.**

| Option | For | Against |
|---|---|---|
| **Behavior trees, cooked to a flat node array, run state in components** | The standard game-AI shape (Unreal, BehaviorTree.CPP); a tree reads as a plan; composites and decorators cover most of what FSM cascades do by hand; flat pre-order arrays need no pointers and cook small | A tree's memory has to fit fixed-size components |
| Extend the FSM helper with data tables | Already in the engine | States and guards are code (function pointers); hierarchies and parallel behavior stay hand-written |
| Utility AI alone | Good at choosing between many options | Poor at sequencing an action's steps; usually paired with a tree. It comes in step 4 as a selector node |
| GOAP | Plans on its own | Hard to debug and to make deterministic within a budget; overkill for most indie games |

## Decisions

1. **A `.mcbt` is JSON the cook turns into `CHNK_BT`.** Every node is an object with exactly one kind key (as
   `.mcseq` steps are). Composites: `sequence`, `selector`, `parallel` (`policy` all or one). Decorators: `invert`,
   `succeed`, `fail`, `repeat`, `retry`, `cooldown`, `timeout`, `guard`. Leaves: `check`, `wait`, `set`, `emit`,
   `task`. The chunk is `BtHeader | BtKey[] | BtNode[] | strings`, with nodes in pre-order: a node's first child is
   the next node and each node stores where its subtree ends. Limits are 128 nodes, 63 children, depth 32 and
   32 timed nodes. The reader checks the shape, so a bad payload never reaches the runner.
2. **Keys are typed and declared by the tree.** These are bool, int, float, vec3 and entity, each with an optional
   default. The cook gives each key a slot in a fixed `Blackboard` component: 16 numbers shared by bool, int and
   float, 8 vectors and 8 entities, plus a bit per slot for "is set". Comparisons are for numbers only. Vec3 and
   entity keys test `is_set`, and an entity key that names a dead entity counts as unset.
3. **All run state lives in `AiBrain`, so a tree is deterministic and saved.** The component has a status byte per
   node (bits 0-1 the result, bits 2-7 a composite's current child), 32 float slots (timers, counters and task
   memory), the brain's clock, its run count and the hash of the tree the state belongs to. A new tree resets the
   state, so a hot reload never misreads old statuses. Both components hash, save and replay like any other.
4. **Memory composites; one root run per tick.** A running node resumes where it was. A sequence or selector
   carries on at its running child, and nothing re-evaluates earlier children until the tree restarts (observer
   aborts come in step 2). A node that enters clears its subtree's results. A finished root restarts on the
   *next* tick. Repeat and Retry also restart their child on the next tick. With node count and depth bounded, a
   tree never loops inside a tick and its cost per tick is bounded.
5. **Timing is tick math, like sequences.** Wait and Timeout count the ticks they ran, with a thousandth-of-a-tick
   tolerance. Cooldown keeps "ready at" on the brain's clock, so it survives the root restarting.
6. **Halting is explicit.** When a parent ends a running subtree (a timeout, a parallel that finished, a restart),
   every running task in it is called once with `halting` set, and its status returns to Idle.
7. **Tasks are the code hook, by name.** `BtRegistry::register_task` maps a name to `BtStatus (*)(BtTaskCall&)`.
   The call gets the frame, the entity, the blackboard, its own slot as memory, `entering` and `halting`, and the
   node's `key`, `value` and `vector` arguments. An unregistered task fails, is logged once, and is reported by
   `validate_tasks`. Step 2 adds stock data tasks for movement, animation, sound, spawning and events.
8. **One FixedUpdate_Pre system, `ai_brains`.** `install_stock` installs it ahead of the path and navmesh agents,
   so a move a tree asks for starts the same tick. It does not run during cutscenes. A brain whose tree has keys
   and whose entity has no `Blackboard` gets one through Commands with the defaults written, and starts on the
   next tick.

**Not now.** Observer aborts and the stock tasks are step 2, perception is step 3, and utility, tokens and point
queries are step 4. The editor panel is step 5, and the MCP tools are step 6. Subtrees referencing other `.mcbt`
files and randomness in nodes are left out until a game needs them. Randomness will come from the sim RNG so
replays stay exact.

**Step 4 (SBA-131).** Utility selection is a node kind whose children are `Score` nodes; considerations are a
table after the nodes in `CHNK_BT` v3, each a blackboard key, a response curve and a weight. The selector
recomputes every score each tick and switches only when another child beats the running one by `hysteresis`.
Tokens are an `AiTokenPool` component that brains share by name or by an entity key. Point queries are the
stock task `find_point`, not a node kind, so a game can replace it.

**Step 3 (SBA-130).** Perception is a separate system ("ai_perception", ahead of "ai_brains") that writes
blackboard keys named by the `Perceiver`; trees never query the world for what they see. Its memory lives in the
`Perceiver`'s runtime fields, so it is hashed and saved like the brain.

**Step 2 (SBA-129).**
- **Observers.** `BtNode::abort` on a guard (CHNK_BT v2) makes it an observer that re-checks its condition every
  tick. `self` halts the guard's running child when the condition fails. `lower`, allowed only on a selector's
  child, makes the selector check that guard before resuming a later running child: the first such guard that
  holds halts the running child and the selector restarts at it in the same tick. Polling every tick gives the
  same results as Unreal's on-change observers in a fixed-tick sim and keeps no subscription state.
- **Stock tasks.** `install_ai` registers `move_to`, `look_at`, `animate`, `play_sound`, `spawn`, `raise`,
  `wait_event` and `log` as ordinary tasks, so a game can replace one by name. They read a string argument
  (`BtNode::arg`) and a key through `bt_key_position`, and reach the kit through the components' own fields and
  events, as a game would.
