#pragma once
// CHNK_ROADS: a cooked road network (M-OPENWORLD B1). The layout and its reader land with the
// .mcroads importer; this seam only reserves the chunk type and its schema version, kept out of
// formats.h so the many TUs that include it do not rebuild for the road kit.

#include "mce/types.h"

namespace mce::asset {

inline constexpr u8 ROAD_NETWORK_VERSION = 1;  // CHNK_ROADS: lanes, junction connectors, signals (M-OPENWORLD B1)

}  // namespace mce::asset
