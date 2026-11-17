#pragma once
// Traffic light lamps that show the signal (M-SIGNALHEADS step 56b). Opt-in per entity: a SignalHead next to a LocalTransform
// and a MeshRef makes that mesh one lamp (red, amber or green) of a traffic light; install_roads registers the component and
// the system kit_signal_heads (FixedUpdate_Pre, after roads_clock).
//
//   SignalHead  Authoring (all Save, never rewritten): `enabled` (1); `lamp` (Red) which colour this lamp is; `network` the
//               RoadNetwork3D entity (null: the first one that resolves); `approach` the world direction the traffic it
//               governs travels (flat, a zero vector disables the head).
//
// The light of an approach is read from the road lane nearest the entity (within `reach` metres, running within 90 degrees of
// `approach`, connectors left out): Green when any connector leaving it is green, else Amber when any is amber, else Red; Green
// when the lane's junction has no signal or the lane has no connector. kit_signal_heads writes only MeshRef::tint and
// MeshRef::emissive_tint of the head's own entity: the lamp whose colour matches the light is lit (bright tint, emissive
// above 1 so bloom picks it up), the others are dark with no emission. The material's emissive colour must be non-black for
// the glow to show. A head with no network, no lane or `enabled` 0 is left as authored; nothing is stored between ticks.

#include "mce/ecs/entity.h"
#include "mce/kit/roads.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

struct SignalHead {
    Entity network;
    math::vec3 approach{0.0f, 0.0f, -1.0f};
    RoadLight lamp = RoadLight::Red;
    u8 enabled = 1;
    u8 pad_[2] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(SignalHead, (enabled, Save, Range{0, 1}, Help{"0: the lamp is left as authored"}),
              (lamp, Save, Help{"the colour this lamp shows when lit"}),
              (network, Save, Help{"the RoadNetwork3D entity; null = the first one"}),
              (approach, Save, Help{"world direction the traffic this light governs travels (flat)"}))

// The light a driver arriving at `position` heading `approach` meets. False (and `out` Green) when no road lane runs to a
// junction within `reach` metres of `position` in that direction. Pure.
bool signal_approach_light(const RoadNet& net, math::vec3 position, math::vec3 approach, RoadLight& out, f32 reach = 16.0f);

// What a lamp of colour `lamp` looks like while the approach shows `state`. Same inputs, same bits.
struct SignalLampLook {
    math::vec4 tint;
    math::vec3 emissive_tint;
};
SignalLampLook signal_lamp_look(RoadLight lamp, RoadLight state);

// Registers SignalHead and adds kit_signal_heads; install_roads calls it.
void install_signal_head(World& world);

}  // namespace mce::kit
