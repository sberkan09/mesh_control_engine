- Open-world sample, M-OPENWORLD E1: `samples/09_openworld` is a data-only 1 km² city slice that plays on the stock
  `mce_player`: a 5 x 5 road grid with traffic lights, traffic and a pedestrian crowd around the player, a parked car to
  get into and drive (`Rider3D`, `VehicleSeat3D`), a hitscan gun on the orbit camera and police who answer the wanted
  level (`Wanted3D`, `Police3D`). `tests/mcp/test_mcp_openworld.cpp` and the `openworld_sample_player` test gate it (log
  clean, replay hash, traffic and crowd counts, enter/exit, a gunfight that raises the wanted level). A scene file can now
  reference a `.mcroads` (`RoadNetwork3D.roads`): the scene cook did not know the `RoadNetwork` asset type. User guide
  chapters 18 and 29.7.
