# M-ARRESTLOSS — an arrest costs ammunition

Status: step 1 done (2026-10-29). Follow-up to [M-WASTED](M-WASTED.md): getting busted already fines the player; now the police also keep ammunition.

## Design

`Confiscate3D { ammo_kept (0) }` (size 4) on the entity that has `Respawn3D`. In `kit3d_respawn`, when the entity wakes up busted
(reason 1) its `Weapon3D.reserve` becomes `floor(reserve * ammo_kept)`. Wasted (reason 2) changes nothing.

## Limits

Only the spare ammunition of the entity's own `Weapon3D`; the loaded magazine and other weapons are untouched; no caption.
