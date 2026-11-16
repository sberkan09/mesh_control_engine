# M-PEDDEAD — Dead pedestrians stay down (milestone record)

**Status:** done in one step (added 2026-10-22; owner: the M-INVENTORY thread, gameplay kit; device-free; no Linear issues: the free limit).
Source: the E1 open items in [M-OPENWORLD.md](M-OPENWORLD.md): "`Pedestrian3D` ignores `Health3D`: a dead pedestrian vanishes after 0.2 s".

**What.** `kit_crowd` checks a pedestrian's `Health3D`. A dead one (`dead` set) gets `PedestrianMode::Down`, stops (pace 0, no stepping), lies where it
fell for `kPedestrianCorpseSeconds` (10 s) and is then removed. It is not counted in the crowd population (`CrowdStats::pedestrians`), so the spawner refills the
street, and it is counted in the new `CrowdStats::down`. A pedestrian revived before the time is up walks again. Traffic only reads `Cross`, so cars ignore a body.

## Open items

- A ragdoll or a lying-down pose on `Down` is the game's (read `mode`); the kit does not play one.
- The corpse time is a constant; a spawner field would change `CrowdSpawner3D`'s saved size.
