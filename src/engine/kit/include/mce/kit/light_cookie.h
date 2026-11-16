#pragma once
// Light cookies as data (M-LIGHTCOOKIE step 40g). Two optional components; install_kit3d registers them next to
// LensFlare3D. There is no system and nothing is stored between ticks: Kit3DExtract copies the fields into
// render::PointLight::cookie* (spot lights) and render::DirectionalLight::cookie (the sun) after running them through
// render::apply_quality (tier, r.light_cookies, Range clamps), so a cookie never feeds the simulation or the state hash.
// The stored fields are the authored base values; roll_speed_deg and scroll are applied in the extract from the render
// clock and never written back.
//
//   LightCookie3D  A spot light's cookie; put it on the entity that has the SpotLight3D (the way LightFlicker pairs with a
//                  light). `texture` (an image: rgb multiplies the light's colour, a is ignored; empty = no cookie);
//                  `strength` (1, 0..1; 0 = the plain light); `roll_deg` (0, -180..180) turns the projection about the cone
//                  axis; `roll_speed_deg` (0, -360..360, degrees per second) a slowly turning gobo; `edge` (0, 0..1) fades
//                  the cookie toward the frustum border; `enabled` (1). A cookie on a PointLight3D is ignored.
//   SunCookie3D    The sun's cookie, one per scene (the first in ECS order is read, like LensFlare3D): `texture`, `strength`
//                  (1, 0..1), `size` (20, 0.5..1000 metres one repeat covers), `offset` (0 0, metres the pattern
//                  is shifted along the plane's two axes), `scroll` (0 0, metres per second), `roll_deg` (0, -180..180), `enabled`
//                  (1). The texture tiles over the ground plane (each point carried there along the sun's direction) and
//                  multiplies the direct sun only.
//
// A texture that is missing, not loaded or not a texture asset gives no cookie (the plain light) and one warning.

#include "mce/asset/handle.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct LightCookie3D {
    asset::AssetRef<asset::Texture> texture;
    f32 strength = 1.0f;
    f32 roll_deg = 0.0f;
    f32 roll_speed_deg = 0.0f;
    f32 edge = 0.0f;
    u8 enabled = 1;
    u8 pad_[7] = {};  // explicit pad up to the 8-byte alignment of the AssetRef (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(LightCookie3D, (enabled, Save, Range{0, 1}, Help{"0: the spot light is plain"}),
              (texture, Save, Help{"image projected through the cone: rgb multiplies the light colour; empty = no cookie"}),
              (strength, Save, Range{0, 1}, Help{"0 = the plain light, 1 = fully shaped"}),
              (roll_deg, Save, Range{-180, 180}, Help{"turns the projection about the cone axis, degrees"}),
              (roll_speed_deg, Save, Range{-360, 360}, Help{"degrees per second the projection turns (a slowly turning gobo)"}),
              (edge, Save, Range{0, 1}, Help{"fades the cookie toward the frustum border, 0 = off. Only a SpotLight3D uses a cookie; on a PointLight3D it is ignored"}))

struct SunCookie3D {
    asset::AssetRef<asset::Texture> texture;
    f32 strength = 1.0f;
    f32 size = 20.0f;
    math::vec2 offset{0.0f, 0.0f};
    math::vec2 scroll{0.0f, 0.0f};
    f32 roll_deg = 0.0f;
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(SunCookie3D, (enabled, Save, Range{0, 1}, Help{"0: the sun is plain"}),
              (texture, Save, Help{"image the sun's light is tiled through: rgb multiplies the sun's light; empty = no cookie"}),
              (strength, Save, Range{0, 1}, Help{"0 = the plain sun, 1 = fully shaped"}),
              (size, Save, Range{0.5f, 1000}, Help{"metres one repeat of the texture covers"}),
              (offset, Save, Help{"metres the pattern is shifted along the plane's two axes"}),
              (scroll, Save, Help{"metres per second the pattern scrolls along the plane's two axes (cloud shade)"}),
              (roll_deg, Save, Range{-180, 180}, Help{"turns the pattern in the plane, degrees"}))

}  // namespace mce::kit
