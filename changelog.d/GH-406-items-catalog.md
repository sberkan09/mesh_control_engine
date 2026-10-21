- Item catalogs (M-INVENTORY step 1, GH-406): `.mcitems` cooks to the new `CHNK_ITEMS` chunk: item definitions (stack, weight in
  fixed-point thousandths, tags, named stats, icon and prefab links) and nested weighted loot tables, with the checks the runtime
  reader repeats (duplicate ids, limits, ranges, table loops, depth 8). `asset_create` / `asset_schema` know the `ItemCatalog` kind;
  user guide 5.1 and 5.7. The inventory components and the loot roller follow.
