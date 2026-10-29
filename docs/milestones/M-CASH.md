# M-CASH — money: cash drops, a wallet, fines

Status: steps 1 and 2 done (2026-10-27). Own gameplay milestone for the GTA-like slice, the open end M-WASTED left: a fine on
`Respawned3D` has nothing to take it from, because `Inventory` has no money item.

## Why

The slice loop is crime, chase, busted or wasted, wake up poorer. Today nothing is earned and nothing is lost. Money is
the cheapest thing that makes the loop matter, and it is what a shop, a mission reward or a car dealer would later use.

## Design

Money is its own small component, not an inventory item: it needs no catalog, it is a single number, and it is hashed and
saved for free.

- `Wallet3D { i32 cash }` (size 4) on the player or any entity that carries money. Negative is not allowed by the
  systems; the field itself is plain so a game can show debt if it wants.
- `CashDrop3D { i32 min, i32 max }` (size 8) on an entity with `Health3D`: when it dies (`Died3D`) and the killer has a
  `Wallet3D`, the killer is credited a deterministic amount in `[min, max]` (the world RNG, no spawned pickup in step 1).
- Events: `CashChanged3D { Entity entity; i32 delta; i32 cash }`, pushed by every credit or debit.
- Helpers: `give_cash(World&, Entity, i32)` and `take_cash(World&, Entity, i32) -> i32` (returns what was actually taken,
  never below zero).
- `Respawn3D` gets `fine_busted` and `fine_wasted` (u16 each; the two pad bytes were not enough, so the component grows from 12 to 16): when the wait
  ends `kit3d_respawn` calls `take_cash` for the reason's fine and pushes `Respawned3D` as today.
- Systems: `"kit3d_cash"` in FixedUpdate_Pre after `"kit3d_wanted"`, installed by `install_wanted3d` (no new install
  wiring), in `wanted3d.cpp`.

## Steps

1. `Wallet3D`, `CashDrop3D`, `CashChanged3D`, `give_cash` / `take_cash`, the drop on `Died3D`, tests (kill pays in range,
   no wallet no pay, take never below zero, deterministic across two runs), user guide section in `09-physics.md`.
   New reflected components need a row in `scripts/ci/sample_coverage.allow`, a `"components"` bump in the MCP transcript
   golden and a regenerated `docs/engine/capabilities.md`.
2. The fines on `Respawn3D` and a "-$N" caption through `captions`.

## Out of scope

Pickup entities that fly to the player, shops and prices, a money HUD (the game's, on `CashChanged3D`), bank accounts.

## Built (step 1)

`Wallet3D`, `CashDrop3D`, `CashChanged3D`, `give_cash` / `take_cash` and the system `kit3d_cash` live in
`wanted3d.h/.cpp`. The amount is drawn from an Rng seeded by the victim's entity index, so replays pay the same. A wallet
saturates at 2e9. Tests: a kill pays inside [min, max], no wallet or no drop pays nothing, `take_cash` stops at zero,
two identical runs pay the same.

## Built (step 2)

`Respawn3D::fine_busted` (100) and `fine_wasted` (200), both `u16`: when the wait ends `kit3d_respawn` calls `take_cash`
for the reason's fine and, with `caption` 1, captions installed and a fine above 0, pushes a "-$N" `ShowCaption` for 2 s.
`CashChanged3D` reports the debit. No wallet means nothing is taken. Tests: the fine is taken, a poor wallet goes to zero,
no wallet takes nothing, the caption shows only when something was taken.
