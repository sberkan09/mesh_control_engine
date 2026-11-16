# M-GAMEPAD: every openworld action has a pad binding (2026-10-31)

Data only (`samples/09_openworld/gen_assets.py` -> `assets/input/openworld.mcinput`). A walk through the map found two gaps: `Radio` had only the Q key, and the UI map's `Navigate` (pause menu focus) had only keys, so the pause menu could be opened with Start but not moved through. Now:

- `Radio`: D-pad left.
- `Navigate`: D-pad (right/left/up/down) and the left stick (deadzone 0.4), next to the arrow and WASD keys.
- The generator asserts that every Gameplay and UI action has at least one `gamepad_button` or `gamepad_axis` binding, so a future action cannot ship pad-less.

Not done: a hands-on play on a real pad (needs a device), touch controls, rebinding UI. The sample tests do not press pad buttons; the binding names are validated when the input map is cooked (a wrong name fails the cook).
