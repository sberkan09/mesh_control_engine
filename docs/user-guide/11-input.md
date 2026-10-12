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

Unless the project ships its own map, the player (and every project host) loads the starter-kit
map: a **Gameplay** map (Move, Look, Jump, Fire, Pause) and a **UI** map (Navigate, Confirm,
Cancel) that, when pushed on top, blocks the gameplay map so menus do not move the player. Maps
stack up to eight deep; the top map's actions shadow same-named actions below.

### Your own map: `.mcinput`

Put an `.mcinput` file under `assets/` (for example `assets/input/controls.mcinput`) and the cook
turns it into an input-map chunk. The player loads the first one in the pack by path; `-input_map
<guid or path>` picks another. With none, the starter-kit map above is used.

```json
{ "format": "mce-input", "version": 1, "guid": "<uuid>",
  "maps": [
    { "name": "Gameplay", "actions": [
      { "name": "Move", "type": "axis2d", "bindings": [
        { "key": "D" }, { "key": "A", "scale": -1 },
        { "key": "W", "axis": "y" }, { "key": "S", "axis": "y", "scale": -1 },
        { "gamepad_axis": "LeftX" },
        { "gamepad_axis": "LeftY", "axis": "y", "scale": -1, "deadzone": [0.2, 1], "curve": "linear" } ] },
      { "name": "Jump", "bindings": [ { "key": "Space" }, { "gamepad_button": "South" } ] },
      { "name": "Dash", "bindings": [ { "key": "LShift" }, { "gamepad_button": "RightShoulder" } ] },
      { "name": "Look", "type": "axis2d", "bindings": [ { "mouse_delta": "x" }, { "mouse_delta": "y" } ] } ] },
    { "name": "UI", "blocks_below": true, "actions": [
      { "name": "Navigate", "type": "axis2d", "bindings": [ { "key": "Right" }, { "key": "Left", "scale": -1 } ] },
      { "name": "Confirm", "bindings": [ { "key": "Enter" } ] },
      { "name": "Cancel", "bindings": [ { "key": "Escape" } ] } ] } ] }
```

- An action's `type` is `button` (the default), `axis1d` or `axis2d`. A binding names exactly one
  device: `key`, `mouse_button`, `mouse_delta` (`"x"`/`"y"`), `mouse_wheel` (`"x"`/`"y"`),
  `gamepad_button` or `gamepad_axis`. `axis` (`"x"` default, `"y"` only on `axis2d`) says which
  component it drives; `scale`, `invert`, `deadzone` `[inner, outer]` and `curve` (`linear`,
  `smooth`, `quadratic`, `cubic`) are optional.
- Key names are the `platform::Key` names: `A`..`Z`, `0`..`9` (or `Num0`..`Num9`), `F1`..`F12`,
  `Space`, `Return` (or `Enter`), `Escape`, `Backspace`, `Tab`, `Grave`, `Left`/`Right`/`Up`/`Down`,
  `LShift`, `LCtrl`, `LAlt`, `LGui` and the `R` versions. Gamepad names are the SDL standard layout
  (`South`, `East`, `West`, `North`, `Start`, `Back`, `LeftShoulder`, `DpadUp`, ..., `LeftX`,
  `LeftY`, `RightX`, `RightY`, `LeftTrigger`, `RightTrigger`). Mouse buttons: `Left`, `Middle`,
  `Right`, `X1`, `X2`.
- The first map is the one the stack starts with. Keep a map named **UI** with Navigate, Confirm
  and Cancel if you use `mce::ui` menus, and use the action names behind the `InputState` fast
  fields (Move, Look, Jump, Fire, Pause, Confirm, Cancel, TriggerL, TriggerR) where they fit:
  those fields fill only from actions of those names. Any other action is read by name
  (`in.pressed(MCE_SID("Dash"))`).
- Touch (phones, M-MOBILE). Zones are `[x0, y0, x1, y1]` over the window, 0..1 with y down, and a
  finger belongs to every zone it **started** in, wherever it slides afterwards.
  - `touch_zone`: an on-screen button, held while such a finger is down.
  - `touch_tap`: one tick when a short, still touch lifts (under 0.25 s and 3 % of the screen's
    shorter side).
  - `touch_stick` (`"x"`/`"y"`, optional `"zone"`): a floating virtual stick centred where the
    finger landed; full deflection at 12 % of the shorter side; `deadzone` and `curve` apply. The
    oldest finger in the zone drives it. Screen y is down, so the y binding of a Move stick takes
    `"scale": -1`, as for gamepad sticks.
  - `touch_swipe` (`"x"`/`"y"`, optional `"zone"`): ±1 for one tick when a fast touch lifts
    (at least 8 % of the shorter side within 0.5 s), on the axis it mostly moved along.

  ```json
  { "name": "Move", "type": "axis2d", "bindings": [
      { "touch_stick": "x", "zone": [0, 0, 0.5, 1] },
      { "touch_stick": "y", "scale": -1, "zone": [0, 0, 0.5, 1], "deadzone": [0.15, 1] } ] },
  { "name": "Jump", "bindings": [ { "touch_tap": [0.5, 0.15, 1, 1] } ] }
  ```

  The default map already has this layout: a Move stick on the left half, Look and Jump (tap) on
  the right half below a Pause corner at the top right. `InputState::touch_points()` gives the
  raw fingers (oldest first, normalized) for tap-to-point games (`kit::ViewInfo::touch_to_world`,
  chapter 7.2, turns one into a world point), and `ui::apply_touch_pointer`
  makes the oldest finger the `mce::ui` pointer. Touches are part of the recorded stream.
- Unknown keys, a typo'd field, two devices in one binding, a touch zone outside 0..1 or a `"y"`
  binding on a button are cook errors with file and line.

A custom host can also build a map in code with `InputMapBuilder` (see
`samples/03_platformer/main.cpp` and `05_ashen_court/main.cpp`).

## 11.4 Rebinding

The input system supports capture rebinding (`begin_rebind` swallows the next press and binds it),
binding overrides that re-apply when a map loads, and saving them to a text file
(`save_bindings` / `load_bindings`). The player runtime does not expose a rebinding screen or
persist overrides yet; a custom host can.

## 11.5 Headless and in the editor

- `-headless 1` on its own publishes no input: the snapshot stays zeroed, so a headless hash
  depends only on the scene and the tick count.
- `-input <file>` feeds a scripted `InputScript` (`mce/input/script.h`) instead of the keyboard,
  headless or windowed. One segment per line, `#` starts a comment:

  ```
  # <from>[-<to>] <Action>[=<value>] [<Action>[=<value>] ...]
  10-70 Move=1,0 Jump      # ticks 10..70: Move (1, 0) and Jump held
  100 Fire                 # tick 100 only (a press)
  120- Move=-1,0           # from tick 120 to the end of the run
  ```

  Ticks are 1-based sim ticks. A value is a number, `x,y` for a 2D axis, or `true`/`false`; an
  action with no value is held. Unnamed actions are released, and edges follow the normal rules.
  A bad file is an error (exit 1). With scripted input, an autoplay switch in your game (a
  component flag that drives the character from code, as Undertow, Prism Break and Lamplighter
  used) is optional.
- Play-in-editor uses the same map as the player and takes keys while the Viewport has focus
  (section 3.8). Where no host installs `InputState`, install an idle one in your `install()` so
  your systems can still read it (section 6.8).

## 11.6 Replays

`InputRecorder` appends each tick's live snapshot; `InputReplayer` feeds it back. A replay file
(`save::write_replay`) frames the stream with the scene GUID and the start and final state hashes,
so replaying it checks itself. Wiring a recorder into the player is not done; the API is there
for a custom host and for tests. To play a fixed sequence of inputs through the stock player, write
an input script and pass `-input <file>` (section 11.5).
