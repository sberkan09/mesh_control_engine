// M-GI2 50h: GiVisibility3D's helpers (mce/kit/gi_visibility.h).

#include "mce/kit/gi_visibility.h"

#include <cmath>

namespace mce::kit {

f32 gi_visibility_max_offset(const GiVisibility3D& v) {
    if (std::isnan(v.max_offset)) return 0.4f;
    return v.max_offset < 0.0f ? 0.0f : (v.max_offset > 0.45f ? 0.45f : v.max_offset);
}

bool gi_visibility_bake_ok(const GiVisibility3D* v) { return !v || !v->enabled || v->bake_format == kGiRecordFormat; }

}  // namespace mce::kit
