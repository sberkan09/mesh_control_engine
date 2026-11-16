# M-HEALSHOP — paying for a medic

Status: step 1 done (2026-10-28). Small gameplay milestone for the GTA-like slice; third cash sink after the garage
(M-PAYSPRAY) and the ammo shop (M-AMMOSHOP).

## Design

`HealShop3D { radius (3 m), price_per_hp (1), enabled }` (size 8) on a placed entity. System `kit3d_healshop` runs after
`kit3d_ammoshop`. A buyer is a living entity with `Health3D`, `Wallet3D` and a transform within `radius` whose `current`
is below `max`. It is healed `min(ceil(max - current), cash / price_per_hp)` hit points through `take_cash` and
`HealBought3D` is pushed. A healthy buyer sells nothing, so standing in the clinic is safe.

## Limits

No armor, no revive of the dead (Respawn3D owns that), no cooldown, no confirm key.
