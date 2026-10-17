- Pedestrian crowds, M-OPENWORLD B3 (ADR-0048): `kit::CrowdSpawner3D` keeps up to `max_count` `kit::Pedestrian3D`
  walking the pavements (`sidewalk` of a `.mcroads` road) of a `RoadNetwork3D` in a ring around a focus entity or the
  camera and removes the ones that fall behind. Pedestrians carry no behaviour tree (a few numbers each), round
  corners or cross roads at junctions, wait at the kerb for the lights, step every tick, every 4th or every 16th by
  distance, and replay and save exactly. `mce/kit/crowd.h`; user guide chapter 29.
