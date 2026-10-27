# M-WASTED — busted and wasted: the player comes back

Status: open (designed 2026-10-27, no code yet). Own gameplay milestone, the follow-up M-ARREST asked for: `Arrested3D` and
`Died3D` exist, but nothing happens after them; the game has to write the whole "busted / wasted" flow itself.

## Why

In the GTA-like slice the loop is crime, chase, then busted (arrest) or wasted (death), and in both cases the player wakes
up at a station or hospital, poorer, with the wanted level gone, and plays on. Today a dead or arrested player stays where
they are.

## Design

One reflected component on the player and a marker on placed entities: `Respawn3D` (on the entity with `Health3D`) and
`RespawnPoint3D { kind }` (Hospital, Station).

- `Respawn3D { delay (s, 3), busted_fine (u32, 100), wasted_fine (u32, 200), engine-written: timer, reason }`.
- A system `"kit3d_respawn"` in FixedUpdate_Pre after `"kit3d_wanted"` reads `Died3D` (entity has `Respawn3D`) and
  `Arrested3D` (offender has `Respawn3D`) and starts the timer with the reason. When it runs out it moves the entity to
  the nearest `RespawnPoint3D` of the matching kind (Station for busted, Hospital for wasted; the nearest of any kind when
  the matching kind is missing; it stays put when there is none), restores `Health3D` (alive, full), calls `clear_wanted`,
  takes the fine from the entity's `Inventory` money item if it has one (never below zero), and pushes
  `Respawned3D { entity, reason, position, fine }`.
- An arrest already clears the wanted level; on death `clear_wanted` is added here.
- Everything is in components, so it is hashed, saved and deterministic. Weapons lost on arrest are the game's, on
  `Respawned3D`.

## Steps

1. `Respawn3D`, `RespawnPoint3D`, `Respawned3D`, the system, `install_stock`, tests: die, wait, wake up at the hospital with
   full health and no wanted level; an arrest goes to the station; no point means it stays; the fine never goes below zero.
   New reflected components need entries in the three `engine_components.cpp`, a row in `scripts/ci/sample_coverage.allow`
   and a `"components"` bump in the MCP transcript golden.
2. A "WASTED" / "BUSTED" caption through the existing `captions` (the event is the hook) and a user guide section.

## Out of scope

Hospital bills that scale with the wanted level, a fade-to-black camera, losing weapons (the game's, on the event).
