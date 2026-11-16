# M-CASHPICKUP — money lying around

Status: step 1 done (2026-10-29). Small gameplay milestone for the GTA-like slice: cash can be found, not only earned from
kills (M-CASH) and missions (M-MISSIONPAY).

## Design

`CashPickup3D { cash (50), radius (1.5 m), respawn (0), left }` (size 16) on a placed entity. System `kit3d_cashpickup` runs
after `kit3d_healshop`. While `left` is above zero it counts down. Otherwise the first entity with a `Wallet3D` within
`radius` gets `give_cash(cash)` and `CashPickedUp3D { entity, pickup, cash }` is pushed. With `respawn` 0 the pickup entity
is destroyed; with a positive `respawn`, `left` is set and it can be taken again after that many seconds. If `give_cash`
refuses (full wallet) the pickup stays.

## Limits

No visual: a game hides a respawning pickup from the event; no magnet pull toward the player; the first holder in query
order wins when two stand in range.
