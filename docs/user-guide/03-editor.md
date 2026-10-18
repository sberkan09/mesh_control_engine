# 3. The editor

The editor edits one scene (or one prefab) of one project at a time. Every change you make, from
the inspector, the gizmo, a paste, an asset drop or an MCP agent, goes through the same command
funnel, so everything is undoable and everything is mirrored into the viewport the same frame.

## 3.1 Starting it

Use the project's own editor, which knows your game's components and runs your systems in
play-in-editor:

```sh
build/<preset>/projects/MyGame/my_game_editor -project projects/MyGame
build/<preset>/projects/MyGame/my_game_editor -project projects/MyGame -scene scenes/level2.mcscene
```

Flags you may want (all are convars, see chapter 16): `-scene <path under assets/>`,
`-editor.panel Profiler` (bring a panel's tab to the front), `-editor.select Player` (select an
entity after opening), `-editor.ini ""` (do not persist the dock layout).

The stock `mce_editor` works the same way with the module loaded (`mce_editor -module
build/<preset>/projects/MyGame/libmy_game.so -project projects/MyGame`; `mce editor MyGame` with the
SDK); without `-module` it knows engine components only.

## 3.2 The panels

| Panel | What it shows and does |
|---|---|
| **Viewport** | the scene, rendered the way the game renders it, plus gizmos, grid, selection and physics overlays. In play mode it shows the running play world |
| **Hierarchy** | the entities as a parent/child tree, siblings in file order. Select, multi-select, create, rename, delete, drag to reparent |
| **Inspector** | the selected entity's components, one widget per reflected field, with `Add Component` and per-component removal. Prefab instances show override marks with Revert and Apply to prefab |
| **Assets** | the project catalog as a folder tree plus a filtered list. Drag assets into the viewport, the hierarchy or an inspector field; rename and move keep GUIDs; File > "Rescan assets" after adding files outside the editor |
| **Cook** | the last cook's result and every diagnostic as `file:line:col`; Cook and Cook + Pack buttons |
| **Play** | play-in-editor controls, the tick count and state hash, the live values of the selected entity, and the event-flow monitor |
| **Tilemap** | gives the selected entity a tilemap layer, picks the brush from a tileset, toggles the paint tool |
| **Terrain** | the selected entity's `Terrain3D` brushes: raise, lower, smooth, flatten, noise, paint a splat layer, cut and fill holes; radius, strength and hardness; the sculpt toggle; stroke undo and redo (chapter 5, "Terrain") |
| **UI Document** | a `.mcui` UI document's widget tree, the selected widget's fields and a live preview at phone, tablet and desktop sizes (chapter 12, "Editing documents in the editor"). Opens from the View menu or a double click on a `.mcui` in Assets |
| **Timeline** | a `.mctimeline`'s tracks and keys on a time ruler, a curve view of the selected transform or property track, the selected key's fields, record mode and the viewport preview (chapter 13.5, "Editing timelines in the editor"). Opens from the View menu or a double click on a `.mctimeline` in Assets |
| **Animation** | an `.mcanim` animation graph as boxes and arrows, its parameters, states and transitions, and a preview of an entity's clips and graph in the viewport (chapter 8.9, "Editing graphs and previewing clips"). Opens from the View menu or a double click on an `.mcanim` in Assets |
| **Console** | the engine log and a command line for convars and confuncs |
| **Stats** | editor frame rate, sim ticks, viewport size |
| **Profiler** | **Live**: editor frame-time history against the 16.6 ms budget, per-stage times, per-system ECS timings. **Capture**: a `.mcperf` from the player or MCP `perf_run`, with its budget verdict, a frame strip, a per-frame breakdown, spikes, systems, and a compare with a second capture (chapter 14, "Is it fast enough?") |

Panels dock anywhere; View > Reset layout restores the default. The layout persists in
`mce_editor.ini`.

## 3.3 Viewport controls (2D scene mode)

| Action | Input |
|---|---|
| Zoom at the cursor | mouse wheel |
| Pan | right or middle drag |
| Select | left click (Ctrl toggles, Shift adds) |
| Marquee select | left drag on empty space |
| Move the selection | left drag on an entity, or the move gizmo |
| Move / rotate / scale tool | W / E / R, or the Edit menu |
| Snap to grid | Edit > Snap to grid (Ctrl inverts while dragging) |
| Cancel a drag | Esc or right click |
| Frame the selection (or the scene) | F |
| Delete the selection | Del |
| Paint tiles | T (with the Tilemap panel) |

A whole gizmo drag is one undo step.

**3D orbit view** (View > 3D orbit view, O): left drag orbits, right or middle drag pans, the wheel
dollies. With "Orbit camera collision" on, the camera boom is swept against the scene's 2D box
colliders extruded along Z, so it does not pass through walls.

**Sculpting a terrain** (the Terrain panel's "Sculpt in the viewport", which switches to the orbit view):
the brush's rim follows the terrain under the cursor; left drag applies the brush every frame (Ctrl
applies the opposite: lower for raise, fill for hole), Alt+left drag still orbits, Esc ends the
stroke. The view re-bakes the terrain about five times a second while you drag. A stroke is one undo
step (Undo stroke / Redo stroke in the panel), and every stroke saves the `.mcterrain`, so the cook,
the player and the collision see exactly what you sculpted.

## 3.4 Keyboard shortcuts

| Shortcut | Action |
|---|---|
| Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+Shift+S | new, open, save, save as |
| Ctrl+Z / Ctrl+Y | undo / redo (the Edit menu names the step) |
| Ctrl+C / Ctrl+X / Ctrl+V / Ctrl+D | copy, cut, paste, duplicate the selection |
| Del | delete the selection (with its subtree) |
| F2 or double-click | rename (in the Hierarchy; also rename / move an asset in the Assets panel) |
| F5 | play in editor / stop |
| F6 | pause / resume |
| F10 | single step (one fixed tick) |
| Ctrl+F5 | run the scene in the external player (`mce_player`; File > "Run in player"); while it runs, a "Stop player" item in the menu bar ends it |
| F7 | cook |
| W / E / R | move / rotate / scale tool |
| F | frame |
| O | 3D orbit view |
| T | paint tiles |
| Ctrl+Q | quit |

## 3.5 Editing entities

**Create.** Right-click empty space in the Hierarchy or the Viewport: Create Entity, Create Entity
Below (a sibling after the selection), Create Child. A new entity has a name and a
`LocalTransform`.

**Add components.** In the Inspector, `Add Component` lists every reflected component the editor
knows, engine and game alike. A component appears with its C++ default values.

**Edit fields.** The Inspector is generated from reflection: numbers get drag fields (with the
`Range` an author declared), booleans checkboxes, enums combos, vectors and colours per-lane
fields, strings text boxes, asset references a drop target plus a picker, entity references a
picker. Fields marked `ReadOnly` are shown but not editable; `Transient` fields are runtime state
and never saved. Help text declared on a field shows as a tooltip. Edits to the same field
coalesce into one undo step until you edit something else.

**Parent and child.** Drag an entity onto another in the Hierarchy to make it a child (it keeps its
world placement); Shift-drag to place it before that entity among its siblings; drag below the tree
to unparent. "Unparent" is also in the context menu. A child's `LocalTransform` is relative to its
parent; the engine writes the composed `WorldTransform` at run time.

**Copy, paste, duplicate.** Ctrl+D inserts the copy right after its source. Pasted entities keep
their internal references to each other.

**Multi-select.** Ctrl toggles, Shift selects a range in the Hierarchy. Gizmos move the whole
selection; the Inspector edits the first selected entity.

## 3.6 Assets in the editor

- **Drag a texture** from the Assets panel into the Viewport to get a new entity with a `Sprite`
  showing it at the drop point; onto a Hierarchy row to make that sprite a child; onto an
  `AssetRef` field in the Inspector to set the reference.
- **Drag a prefab** into the Viewport to create an instance; double-click or "Open prefab" to edit
  the prefab asset itself.
- **Double-click a scene** to open it (you are asked about unsaved changes). A UI document
  (`.mcui`) opens in the UI Document panel, a timeline (`.mctimeline`) in the Timeline panel, an animation graph (`.mcanim`) in the Animation
  panel.
- **Rename / move (F2)** rewrites the path in the registry and moves the sidecar with the file, so
  every reference keeps resolving. Dropping an asset on a folder moves it.
- **New files** you copy into `assets/` by hand need a sidecar before they cook. The editor's
  cook does not write sidecars; run the project's cook once with `--adopt` (section 5.3) or ask an
  MCP agent for `cook_run {"adopt": true}`, then File > "Rescan assets".
- **Copy GUID** on an asset puts its GUID on the clipboard (prefix it with `guid:` in text edits and MCP calls).

## 3.7 Cook and hot reload

**Cook (F7)** runs the project's cook into `<Project>/cooked/`, then remounts it so the viewport
shows the new content. **Cook + Pack** additionally writes `<Project>/assets.mce`, the single
shipping file. Errors are listed in the Cook panel as `file:line:col: error: message`; an asset
that fails keeps serving its previous cooked output, so a typo in one scene does not blank the
project.

**Hot reload** polls `assets/` twice a second (`-editor.hot_reload_interval`, 0 turns it off).
When a source file changes on disk, for example a PNG saved from an image editor or a `.mcscene`
edited by an agent, the editor re-cooks only that asset and its dependents and swaps the result in.
File > "Hot reload now" forces a pass. A failed re-cook keeps the previous output and reports in
the Cook panel.

## 3.8 Play-in-editor

F5 cooks the open document in memory (unsaved changes included) and instantiates it into a fresh
play world with the same systems the player installs, plus your game's `install()`. The viewport
switches to the scene's primary game camera (View > "Game camera while playing" toggles that).
View > "Gameplay debug (while playing)" (off by default) draws a 3D game's colliders, triggers (green while occupied), joints, character capsules, vehicle wheels and waypoint paths over the viewport, the same lines the MCP `debug_lines` and `debug_view` tools return.

- **Pause (F6), Step (F10), speed.** The Play panel has a speed slider from 0.05x to 4x and preset
  buttons. Pause, step and speed change how much simulated time flows per real second; the fixed
  tick itself never changes, so a run in the editor gives the same state hash as the same tick count
  in the player.
- **Inspect.** Select an entity; the Play panel shows its live component values, `Transient`
  fields included. The event-flow monitor lists each event type with how many were pushed and how
  many are readable this tick.
- **Stop (F5)** destroys the play world. Nothing you did in play mode touches the document; the
  Inspector is read-only while playing.
- **Input.** Click the Viewport to give it focus: while it has focus your keys go to the game,
  with the project's input map (`.mcinput`, chapter 11) or the built-in one. The mouse reaches the
  game while the pointer is over the Viewport. Keys you press with another panel focused stay with
  the editor; F5, F6 and F10 always work. Pointer position is measured in editor-window pixels,
  so for mouse aiming use the real game (`my_game`, or Ctrl+F5 to launch the player).

## 3.9 Autosave and recovery

The editor autosaves a scene with unsaved changes every five minutes
(`-editor.autosave_interval`, 0 means only on focus loss) and whenever the window loses focus, to
`<Project>/.mce/autosave/<scene path>.autosave`. If the editor did not exit cleanly, the next open of
that scene offers "Recover unsaved changes" or "Discard". Saving the scene removes the autosave.

Saves are atomic (temp file, then rename), so a crash mid-save never leaves a half-written scene.

## 3.10 Undo

Everything that changes the document is undoable: inspector edits, gizmo drags, create, delete,
rename, reparent, paste, asset drops, tilemap strokes and every MCP tool call (each call is one
step). History holds 256 steps or 16 MiB and is cleared when you open another scene. "Apply to
prefab" writes the prefab file outside the history; the instance-side change is undoable.

## 3.11 The console panel

The Console panel mirrors the engine log and runs console lines: a convar name alone prints its
value and help, `name value` sets it, `help` lists convars and functions. Cheat and
restart-only convars are refused while cheats are locked (chapter 12).
