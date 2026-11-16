# M-AMMOSHOP — buying ammo

Status: step 1 done (2026-10-28). Small gameplay milestone for the GTA-like slice; the cash earned by M-CASH and
M-MISSIONPAY now has a second use next to the garage.

## Design

`AmmoShop3D { radius (3 m), price_per_round (2), stock_to (120), enabled }` (size 12) on a placed entity. System
`kit3d_ammoshop` runs after `kit3d_payspray`. A buyer is an entity with `Weapon3D`, `Wallet3D` and a transform within
`radius`. If the weapon has a `magazine`, is not `infinite_ammo` and its `reserve` is below `stock_to`, the shop sells
`min(stock_to - reserve, cash / price_per_round)` rounds through `take_cash` and pushes `AmmoBought3D`. A full reserve
sells nothing, so standing in the shop is safe to leave running.

## Limits

No weapon purchase (only ammo for the weapon already held), no per-weapon prices, no key press to confirm.
