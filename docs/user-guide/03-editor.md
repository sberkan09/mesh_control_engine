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
| **Hierarchy** | the entities as a parent/child tree, siblings in file order. Select, multi-select, create, rename, delete, drag to reparent; the **Filter by name** box above the tree (its **Select N matches** button selects every hit) shows only the entities whose name contains every word you type (any case), with their parents, hits in colour |
| **Inspector** | the selected entity's components, one widget per reflected field, with `Add Component` and per-component removal. Prefab instances show override marks with Revert and Apply to prefab |
| **Assets** | the project catalog as a folder tree plus a filtered list; the **Grid** checkbox switches the list to thumbnails (32–128 px, drawn on the CPU a few per frame: textures, a sound's waveform, meshes once cooked, materials as a lit sphere, particle presets as a burst of their particles, a coloured tile for other kinds, redrawn when the file changes). Drag assets into the viewport, the hierarchy or an inspector field; rename and move keep GUIDs; **+ New** makes a starter file of any JSON kind (material, particle, sequence, string table and so on) at a path you type, cooked before it is written, and opens it in its panel; the right-click menu adds **Duplicate...** (a copy under a fresh GUID) and **Delete...** (the file and its sidecar, after listing the assets that still name it) and **Used by...** (the assets whose files name this one); **Unused** lists the assets no asset file names, to find leftovers (a script that loads by path or the start scene is not seen, so check before deleting); File > "Rescan assets" after adding files outside the editor |
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

| **Material** | opens on a double click on a material in Assets: a live lit-sphere swatch, the base colour, metallic, roughness, emissive, alpha mode, UV scale, the varnish, glass and height lobes, and the five texture slots (pick from the list or drop an image from Assets). **Save** cooks the text first and writes nothing if the cook would fail; **Revert** reloads; a dirty material saves when you open another |
| **Particle** | opens on a double click on a particle preset in Assets: a live swatch (the emitter run for 1.5 s), emission (rate, burst, cap, duration, area), motion (lifetime, speed, direction, spread, gravity, drag, spin), look (size and colour over life, additive blend) and the texture (list or drop). **Save** and **Revert** work as in the Material panel; a range keeps min below max because the cook refuses the other order |
| **Sequence** | opens on a double click on an action sequence in Assets: the steps as a list (wait, wait_event, move_to, anim, sound, stop_sound, emit, call, spawn, destroy) with Add, Remove, Up and Down, the selected step's fields below (sound and prefab are picked from the project or dropped from Assets), the name and the loop flag. **Save** cooks the text first and writes nothing if the cook refuses it (a negative wait, a loop that never waits, a missing sound); **Revert** reloads |
| **Strings** | opens on a double click on a `.csv` string table in Assets: every locale in one grid, a row per key and a column per locale (the base `en` first). Cells are edited in place; a locale that lacks a key shows a tinted empty cell, the header line counts what each locale still lacks, and **Missing only** lists just those keys. **Filter** narrows by key, **Add key** adds a row, `x` removes a key from every locale. **Save** cooks each changed table first and writes nothing the cook refuses (a bad key); comment rows and extra columns in the files are kept. **Revert** reloads |
| **Mixer** | the open scene's audio mixer as sections: one strip per bus (low-pass, high-pass and the reverb unit and send), the two reverb units, the master limiter and the ducks. **On** adds the component the section edits, turning it off removes it and the bus is dry again. A slider drag is one undo entry. It writes the mixer components of chapter 10 onto `Mixer ...` entities, so the scene file stays the one source |
| **Source** | any JSON source file as text. A double click in Assets opens it for a file with no panel of its own (a sound set, input map, catalog, remote config, convar defaults, road network), and **Edit source...** in the Assets right-click menu opens any JSON source. The cook checks the text a moment after you stop typing and lists what it refuses with line and column; **Save** writes only text the cook accepts and **Revert** reads the file again. Keep the file's `"guid"` member: a text without it is refused, because the asset would be gone |
| **Scenes** | one tab per open scene file. Opening another scene (Assets, the File menu's Open entry) keeps the one that was open: its unsaved changes are parked in the same autosave file crash recovery uses and come back, still unsaved, when you click its tab (a switch starts a fresh undo history). A `*` marks unsaved changes; the close button on a tab with changes asks before dropping them. A scene with no file yet (untitled) cannot be parked: the usual "unsaved changes" prompt stays for it, and Quit asks while any tab has changes |

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
| Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+Shift+S | new, open, save, save as (File > Open Recent lists the last ten scenes of the project, newest first; a scene that was deleted drops off the list when you click it) |
| Ctrl+P | Quick Open: type to search assets, entities, panels and commands; Up/Down pick, Enter runs (an asset opens like a double-click in Assets, an entity is selected, a panel comes to the front), Esc closes |
| Ctrl+1..9 / 1..9 | camera bookmarks (Viewport focused): Ctrl+number stores the current view (the 2D camera and the 3D orbit camera together) in a slot, the number alone flies back to it; View > Camera bookmarks lists them; slots are kept per project |
| Alt+Left / Alt+Right | step back and forward through the entities you selected, like a browser's history (Edit > Previous selection / Next selection); entities that have been deleted are skipped; the history is per open scene and not saved |
| Alt+Up / Alt+Down / Alt+Shift+Down | select around the selection in the parent/child tree: Alt+Up the parents, Alt+Down the children, Alt+Shift+Down everything below; Edit > Select also has Siblings. Nothing related leaves the selection as it is |
| Ctrl+A / Ctrl+I | select every entity of the scene / every entity that is not selected now (Edit > Select > All, Invert); not while a text field owns the keyboard |
| Edit > Select > Similar | select every entity that has exactly the same components as the primary selection (the one clicked last) |
| Edit > Make names unique | entities that share a name get a numbered suffix (`Wall`, `Wall (2)`, `Wall (3)`; the first keeps its name) among the selection, or the whole scene when nothing is selected; one undo step |
| Edit > Copy path | puts the hierarchy path of each selected entity (`Room/Wall/Torch`, one per line) on the system clipboard, for bug reports, scripts and MCP clients |
| Edit > Reset transform | puts position, rotation and scale of each selected entity back to the identity (0, no rotation, 1) relative to its parent; one undo step |
| Edit > Flip horizontally / vertically | mirrors the selected entities across the middle of the selection (world positions) and negates their scale on that axis; one entity flips in place; one undo step |
| Edit > Snap to grid | rounds the world X and Y of each selected entity to a multiple of the snap step (the **Snap step** in the View menu); one undo step |
| Edit > Duplicate right / Duplicate up | duplicates the selection and moves the copies by one snap step (the **Snap step** in the View menu) along X or Y; the copies become the selection, so repeating the command lays out a row; one undo step |
| Ctrl+Shift+1..9 / Alt+1..9 | selection sets: Ctrl+Shift+number stores the entities you have selected in a numbered slot, Alt+number selects them again (entities deleted since are left out); Edit > Selection sets lists the slots; the sets belong to the open scene and are not saved |
| Edit > Rename selection... | names every selected entity from a pattern in one undo step: `##` is a number (`Torch_##` gives Torch_01, Torch_02, ... in scene order, from the "First number" you set), `*` is the current name (`Old_*` keeps it); the box previews the first four results |
| Ctrl+Z / Ctrl+Y | undo / redo (the Edit menu names the step) |
| Ctrl+C / Ctrl+X / Ctrl+V / Ctrl+D | copy, cut, paste, duplicate the selection |
| Ctrl+G / Ctrl+Shift+G | group the selection under a new parent / dissolve the selected parents (their children move up one level); the entities stay where they are in the scene |
| Edit > Align | with two or more entities selected: line them up on the left, middle or right (x) or bottom, middle or top (y) of the selection, or (three or more) space them evenly between the outermost two; one undo step, the other axis stays |
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
