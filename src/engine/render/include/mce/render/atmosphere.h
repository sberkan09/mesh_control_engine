#pragma once
// Physically based sky (M-VISUAL 7): single scattering through an Earth-like atmosphere of air
// (Rayleigh), aerosols (Mie) and ozone (absorption), after Bruneton & Neyret 2008 and Hillaire 2020
// (constants from the latter). Everything here runs on the CPU: Scene3D::build reads the sun's
// transmittance and the sky's irradiance (the light's color and ambient), Forward3D bakes the sky
// into an equirect texture whenever the sun moves. Radiance is per unit of solar irradiance; the
// caller multiplies by the sun's (π · DirectionalLight color · intensity, the engine's convention
// that a white Lambert surface facing a light of intensity I reflects I).

#include "mce/types.h"
#include "mce/math/math.h"

namespace mce::render {

struct Atmosphere {
    bool enabled = false;
    f32 intensity = 1.0f;             // scale of the sky's radiance (the sun's light is not scaled)
    f32 rayleigh = 1.0f;              // scale of the air's scattering: deeper blue skies, redder sunsets above 1
    f32 mie = 1.0f;                   // haze: scale of the aerosols (whiter skies, a wider glow around the sun)
    f32 mie_anisotropy = 0.8f;        // the aerosols' forward scattering (Henyey-Greenstein g)
    f32 ozone = 1.0f;                 // scale of the ozone absorption (the zenith's blue at dusk)
    math::vec3 ground_albedo{0.3f, 0.3f, 0.3f};
    f32 altitude = 1.0f;              // the viewer's height above the ground, meters (world y is not used)
    // Scene3D::build tints DirectionalLight::color by the air between the ground and the sun (a low
    // sun turns orange, a sun below the horizon goes dark) ...
    bool sun_transmittance = true;
    // ... and sets its ambient_sky / ambient_ground to the sky's and the lit ground's light.
    bool ambient = true;
    f32 rebake_degrees = 0.25f;       // how far the sun moves before Forward3D re-bakes the sky texture
};

// Air between a point at `altitude` meters and space along `dir` (unit, y up): 0 where the ray
// meets the ground.
math::vec3 atmosphere_transmittance(const Atmosphere& a, f32 altitude, math::vec3 dir);

// Sky radiance per unit solar irradiance seen along `view` (unit, y up) with the sun toward `sun`
// (unit; the opposite of DirectionalLight::direction). Below the horizon: the lit ground through the
// air in front of it. The sun's disk is not included.
math::vec3 atmosphere_sky(const Atmosphere& a, math::vec3 sun, math::vec3 view);

// The sky as an equirect RGBA32F image (rows top first: row 0 looks up, as equirect_uv in the 3D
// shaders), radiance per unit solar irradiance times Atmosphere::intensity; alpha 1. `out` holds
// width * height * 4 floats.
void bake_atmosphere_sky(const Atmosphere& a, math::vec3 sun, u32 width, u32 height, Span<f32> out);

// The sky's irradiance on an upward-facing surface divided by π, per unit solar irradiance (times
// Atmosphere::intensity): the ambient_sky of a white sky-lit Lambert surface. Sun excluded.
math::vec3 atmosphere_sky_ambient(const Atmosphere& a, math::vec3 sun);

// `sun` (unit, toward the sun) snapped to a grid of Atmosphere::rebake_degrees in elevation and
// azimuth: what the scene's light and sky are computed for, so a slowly moving sun re-bakes in steps.
math::vec3 atmosphere_quantized_sun(const Atmosphere& a, math::vec3 sun);

// Where the sun stands at `hours` (0..24) of a simple day: it rises in the east (+x) at 6, culminates
// `noon_elevation_deg` above the southern (-z) horizon at 12 and sets in the west at 18, the whole
// path turned `azimuth_deg` about y. Returns the way its light travels (DirectionalLight::direction).
math::vec3 sun_direction_at(f32 hours, f32 noon_elevation_deg = 60.0f, f32 azimuth_deg = 0.0f);

}  // namespace mce::render
