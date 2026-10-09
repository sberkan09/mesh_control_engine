# 11. Input

Gameplay never sees keys, buttons or SDL events. Each fixed tick the engine folds every device
event into one immutable snapshot, `input::InputState`, and your systems read that. The snapshot
is part of the determinism contract: it can be recorded and replayed, and the same snapshot
sequence gives the same game.

## 11.1 Reading input

```cpp
void player(Frame& f) {
    const input::InputState& in = f.resource<input::InputState>();
    const f32 mx = in.move.x;                 // -1..1, WASD / arrows / left stick / d-pad
    if (in.jump_pressed) { ... }              // edge: this tick only
    if (in.jump_held) { ... }                 // level
    if (in.fire_pressed) { ... }
    if (in.pause_pressed) { ... }
    const bool dash = in.has_action(MCE_SID("Dash")) ? in.pressed(MCE_SID("Dash")) : in.fire_pressed;
}
```

| Member | Action | Default bindings |
|---|---|---|
| `move` (vec2) | Move | W A S D, arrows, left stick, d-pad |
| `look` (vec2) | Look | mouse delta, right stick |
| `jump_pressed` / `jump_held` / `jump_released` | Jump | Space, gamepad South (A) |
| `fire_pressed` | Fire | J, left mouse, gamepad West (X) |
| `pause_pressed` | Pause | Escape, Start |
| `confirm_pressed`, `cancel_pressed` | Confirm, Cancel (UI map) | Enter or Space, Escape; South, East |
| `text()` | typed text this tick (UTF-8) | |
| `mouse_pos_px()`, `mouse_wheel()` | pointer state at the tick's end | |

Named access (`action`, `pressed`, `released`, `held`, `axis`, `axis2`) takes a `StringId`
(`MCE_SID("Jump")`) and works for any action in the current map. An unknown name reads as zero and
logs one error per name; `has_action` tells you whether a map defines it.

## 11.2 How the snapshot is built

- **Tick-quantized.** Tick N owns a half-open time window; events stamped inside it are folded in
  timestamp order. Input is therefore independent of the render rate: a 30 fps and a 240 fps
  machine see the same per-tick input.
- **Edges per tick.** `pressed` is true on the first tick a button is down, `released` on the
  first tick it is up. A tap shorter than a tick still shows as held for one tick. OS key repeat is
  not a press.
- **Focus loss** releases everything (release edges fire once), so nothing sticks when you alt-tab.
- **Gamepads** are "any pad": a button is held while any connected pad holds it; an axis takes the
  largest deflection. Sticks go through deadzones and curves defined by the map.
- **The editor** consumes the events it uses for its own UI; a release always reaches the game so
  no key sticks when you click out of the viewport.

## 11.3 Maps

The player (and every project host) loads the starter-kit map: a **Gameplay** map (Move, Look,
Jump, Fire, Pause) and a **UI** map (Navigate, Confirm, Cancel) that, when pushed on top, blocks the
gameplay map so menus do not move the player. Maps stack up to eight deep; the top map's actions
shadow same-named actions below.

There is **no input-map asset** yet: a project cannot cook its own map into the pack, so actions
beyond the default ones read as zero. A game needing more actions (Ember Step's `Dash`) reuses a
default action (Kindle dashes on Fire) and keeps `has_action` checks in place so it works
unchanged once a map asset exists. A custom host can build a map in code with `InputMapBuilder`
(see `samples/03_platformer/main.cpp` and `05_ashen_court/main.cpp`).

## 11.4 Rebinding

The input system supports capture rebinding (`begin_rebind` swallows the next press and binds it),
binding overrides that re-apply when a map loads, and saving them to a text file
(`save_bindings` / `load_bindings`). The player runtime does not expose a rebinding screen or
persist overrides yet; a custom host can.

## 11.5 Headless and in the editor

- `-headless 1` publishes no input: the snapshot stays zeroed, so a headless hash depends only on
  the scene and the tick count. Games that need input to progress in a headless test give
  themselves an autoplay switch (a component flag that drives the character from code), as
  Undertow, Prism Break and Lamplighter did.
- Play-in-editor feeds no input either: if the host has no `InputState`, install an idle one in
  your `install()` so your systems can still read it (section 6.8).

## 11.6 Replays

`InputRecorder` appends each tick's live snapshot; `InputReplayer` feeds it back. A replay file
(`save::write_replay`) frames the stream with the scene GUID and the start and final state hashes,
so replaying it checks itself. Wiring a recorder into the player is not done; the API is there
for a custom host and for tests.
