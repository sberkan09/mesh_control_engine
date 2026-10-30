# M-WORLDEDGE: invisible walls at the map edge (2026-10-30)

`samples/09_openworld/gen_assets.py` (`city_surface`) adds four 20 m high `BoxCollider3D` walls ("World edge 0..3") just outside the 1120 m ground slab, so the player, cars and walkers cannot leave the map. Data only; covered by the existing sample tests (`openworld_sample_player`, `test_mcp_openworld`).

Limits: invisible (no fence mesh, no warning); the sky and fog still show the edge; no out-of-bounds respawn is needed because nothing can leave.
