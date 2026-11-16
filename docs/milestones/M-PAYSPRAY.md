# M-PAYSPRAY — pay-and-spray: pay a garage to lose the wanted level

Status: step 1 done (2026-10-28). Own gameplay milestone for the GTA-like slice; the use for M-CASH's wallet that closes
the loop: crime, chase, pay, clean slate.

## Why

Busted and wasted are the punishments; the other way out of a chase is a garage. Today the only ways to lose the level
are to hide until the heat decays or to be arrested or killed.

## Design

`PaySpray3D { radius (3 m), price_per_level (100), enabled }` on a placed entity (size 8). The system
`"kit3d_payspray"` (FixedUpdate_Pre, after `"kit3d_cash"`) looks at every entity with `Wanted3D` and `Wallet3D` at level
1 or more within `radius` of an enabled garage. If its cash covers `price_per_level` x level it pays through `take_cash`,
`clear_wanted` runs and `Sprayed3D { entity, garage, paid, level }` is pushed. Too poor, too far, switched off or without
a wallet: nothing happens. Whether police in sight should stop it (GTA does) is the game's rule on top; the garage does
not check it.

## Built (step 1)

`PaySpray3D`, `Sprayed3D` and the system live in `wanted3d.h/.cpp`, installed by `install_wanted3d`. Tests: pays level x
price and clears the level, once; too poor, too far, switched off, no wallet leave the level alone.

## Out of scope

Refusing while police see the player, a re-paint of the car, a "-$N" caption (the game's, on `Sprayed3D` /
`CashChanged3D`), a drive-in animation.
