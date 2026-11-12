#pragma once
// Ice-crystal halo and sun dogs as data (M-HALO step 54c). One optional Halo per scene (the first in ECS order is read).
// install_weather registers the component next to Cirrus; there is no system and nothing is stored between ticks: the
// component never feeds the simulation or the state hash. The render side is render::Halo (54a, drawn by 54b through
// the cirrus layer); Kit3DExtract fills it from this component and the cirrus cover the frame already carries, then the
// quality step (render::apply_quality: the tier, `r.halo`, the clamps) applies.
//
//   Halo  Authoring (all Save, clamped to the Range): `strength` (1, 0..1) scales the ring and the dogs, 0 = none;
//         `sun_dogs` (1, 0..1) the dogs' share; `moon` (0.5, 0..1) the moon ring's share; `dispersion` (1, 0..1) the
//         red-inside, blue-outside colour fringe (0 = white).
//
// The halo shows only through cirrus: see halo_level. A scene without a Halo (or a Cirrus, or a Weather) is unchanged,
// bit for bit.

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct Halo {
    f32 strength = 1.0f;
    f32 sun_dogs = 1.0f;
    f32 moon = 0.5f;
    f32 dispersion = 1.0f;
};
MCE_COMPONENT(Halo, (strength, Save, Range{0, 1}, Help{"scales the ice halo and sun dogs; 0 = none"}),
              (sun_dogs, Save, Range{0, 1}, Help{"strength of the two sun dogs"}),
              (moon, Save, Range{0, 1}, Help{"strength of the faint ring round the moon"}),
              (dispersion, Save, Range{0, 1}, Help{"colour fringe of the ring: 0 = white, 1 = red inside and blue outside"}))

// The cirrus cover under which no ring shows, and the cover from which it is full (a smoothstep between).
inline constexpr f32 kHaloCoverLo = 0.1f;
inline constexpr f32 kHaloCoverHi = 0.5f;

// The strength the extract hands the renderer, 0..1, a pure function of its arguments: `strength` (NaN falls back to
// the default, out-of-range clamps) times smoothstep(kHaloCoverLo, kHaloCoverHi, cirrus_coverage), so only a real veil
// of ice shows a ring. Exactly 0 for `strength` 0, for a cover at or under kHaloCoverLo and for a NaN or negative one.
f32 halo_level(const Halo& h, f32 cirrus_coverage);

}  // namespace mce::kit
