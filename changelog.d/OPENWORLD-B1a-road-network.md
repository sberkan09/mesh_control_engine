- Road networks, M-OPENWORLD B1 (ADR-0046): a `.mcroads` asset lists junctions and roads (lanes each way, shape
  points, speed, sidewalk, priority, drive side) and the cook builds every lane, the connectors through each
  junction with their turns, conflicts and right of way (`none`, `priority`, `stop`, `signal`) and the traffic-light
  phases into `CHNK_ROADS` (`asset::RoadView`). MCP `asset_create` makes one (kind `RoadNetwork`); user guide
  chapter 29.
