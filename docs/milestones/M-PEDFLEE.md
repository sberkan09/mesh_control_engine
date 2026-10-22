# M-PEDFLEE — Pedestrians run from whoever hurt them (milestone record)

**Status:** steps 1 to 3 built (added 2026-10-22; owner: the M-INVENTORY thread; device-free; no Linear issue: the free limit).
Source: the GTA feasibility assessment (AI behaviour: fleeing) after M-PEDDEAD.

**Why.** A shot pedestrian that survived kept strolling. A crowd that ignores gunfire reads as dead scenery.

**What.** `PedestrianMode::Flee` (appended to the enum, saved as a u8). In `crowd.cpp` a pedestrian with `Health3D.hits > 0`, alive and `since_hit < 5 s` flees: on its
edge it turns to the end farther from `last_attacker`, moves `kPedestrianFleeScale` (2.5) times its speed, skips the wait for a red light, then walks again.
No new field (`Pedestrian3D` stays 44 bytes), so no save change; the behaviour follows from `Health3D`. Test: `tests/kit/test_crowd.cpp` (both attacker sides, pace, calming down).

**Step 2.** The crowd system reads `WeaponFired3D`: a pedestrian within 30 m of a shot runs from its muzzle for 5 s (a short list of scared pedestrians in `CrowdState`, capped at 512,
not saved). Test: `test_crowd.cpp` (both sides of the shooter, a far shot, calming down).

**Step 3.** A running pedestrian is also a source: neighbours within 8 m catch the panic with one second less to run per hop (so it dies out), and a hurt pedestrian warns the
ones near it. The scared list now holds every runner with its last position. Test: `test_crowd.cpp` (a close neighbour catches it, a far one does not, the ripple ends).

## Open items
- Tuning the radii and the hop decay against a dense street (needs a measured crowd, not a unit test).
