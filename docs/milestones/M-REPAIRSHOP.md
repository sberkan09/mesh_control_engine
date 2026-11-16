# M-REPAIRSHOP: repair shop / pay and spray (2026-10-30)

Design: section I of `M-OPENWORLD-GAPS.md`. Gameplay kit only (`wanted3d.h/.cpp`, `impact_damage.h/.cpp`).

- `repair_vehicle(World&, Entity car)`: undoes `kit_wreck` (health back to max, `dead` 0, handbrake off, `use_input` 1 with a driver, smoke off).
- `RepairShop3D` (24 bytes) + system `kit3d_repairshop` (after `kit3d_chopshop`): a nearly stopped car with a `Wallet3D` driver within `radius` is
  repaired for `price`; the wanted level drops by `wanted_drop` (0 = clear) at `price_per_level` each while no police sees the driver. All or
  nothing, once per visit (`served`), event `Repaired3D`.
- Sample: "Repair shop 1/2" pads at (-55, 7) and (-175, -7) in `09_openworld`.
- Tests: `tests/kit/test_wanted3d.cpp` ("a repair shop mends a wreck ..."), `test_mcp_openworld` counts 2 shops.

Limits: a wreck cannot drive in (it must be pushed or die there); no spray animation or colour change; police sight is this tick's only;
only the player's car is served (traffic is locked, police cars have no seat); no caption yet.
