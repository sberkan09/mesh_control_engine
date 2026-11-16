# M-FALLDEATH: a safety net under the world

**Status:** done (one step). Found in [M-OPENWORLD-GAPS](M-OPENWORLD-GAPS.md) section C: the sample had no fall-off handling.

`FallDeath3D { min_y = -30 }` (`mce/kit/fall_death.h`, 8 bytes) on an entity with a `Health3D`. System `kit_fall_death`
(FixedUpdate_Pre, after `kit3d_health`): below `min_y` the entity takes 1e9 damage through `apply_damage`. `Damage3D` and
`Died3D` fire, so the existing `Respawn3D` (M-WASTED) wakes the player at the nearest hospital with the usual fine. An
invulnerable entity is left falling; a dead one is skipped.

Sample: the player carries `min_y = -40` (the slab is at 0, the world-edge walls keep players in, so this only catches
holes and launches).

Tests: `tests/kit/test_fall_death.cpp` (above or below, invulnerable, one `Damage3D`); `test_mcp_openworld` checks the
component is reachable over MCP.
