// State-hash padding (found by M-WEATHER step 3): World::state_hash hashes whole component columns, so a byte of
// compiler padding that a copy leaves indeterminate makes the hash depend on what the stack held (a
// ParticleEmitter3D built as a local and set on an entity gave a flaky hash). kit3d.h therefore declares explicit
// pad fields (see its header comment). The first case counts, for every kit3d.h component, the bytes the
// reflected fields do not cover and compares them with the size of its explicit pad fields: a new hole (a field
// that adds alignment padding) makes the count larger and fails here. The second case is the bug itself, a
// default-initialised emitter over dirty memory.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <vector>

#include "mce/kit/kit3d.h"
#include "mce/kit/rain_ripples.h"
#include "mce/kit/cloud_layer.h"
#include "mce/kit/rain_splash.h"
#include "mce/kit/snow_cover.h"
#include "mce/kit/thunder_delay.h"
#include "mce/kit/weather_audio.h"
#include "mce/kit/wet_surfaces.h"
#include "mce/kit/wind_direction.h"
#include "mce/kit/wind_sway.h"
#include "mce/refl/field_traits.h"

using namespace mce;
using namespace mce::kit;

namespace {

template <class T>
u32 uncovered_bytes() {
    const refl::TypeRecord& t = refl::type_of<T>();
    std::vector<std::pair<u32, u32>> spans;
    for (const refl::FieldRecord& r : t.fields) spans.push_back({r.offset, r.offset + r.size * r.count});
    std::sort(spans.begin(), spans.end());
    u32 pos = 0, gap = 0;
    for (const auto& s : spans) {
        if (s.first > pos) gap += s.first - pos;
        pos = std::max(pos, s.second);
    }
    if (pos < t.size) gap += t.size - pos;
    return gap;
}

template <class T>
void expect_pad(const char* name, u32 explicit_pad_bytes) {
    INFO(name << " has " << uncovered_bytes<T>() << " unreflected bytes; its explicit pad fields cover " << explicit_pad_bytes
              << ". Add an explicit `u32 padN_ = 0;` for each hole (kit3d.h header comment).");
    CHECK(uncovered_bytes<T>() == explicit_pad_bytes);
}

}  // namespace

TEST_CASE("padding: every unreflected byte of a kit3d.h component is an explicit pad field") {
    expect_pad<MeshRef>("MeshRef", 0);
    expect_pad<PointLight3D>("PointLight3D", 0);
    expect_pad<SpotLight3D>("SpotLight3D", 0);
    expect_pad<ReflectionProbe3D>("ReflectionProbe3D", 0);
    expect_pad<GiVolume3D>("GiVolume3D", 0);
    expect_pad<Camera3DComponent>("Camera3DComponent", 0);
    expect_pad<Animator>("Animator", sizeof(Animator::pad2_));
    expect_pad<Environment3D>("Environment3D", sizeof(Environment3D::ibl_pad_) + sizeof(Environment3D::lens_pad_));
    expect_pad<BoxCollider3D>("BoxCollider3D", sizeof(BoxCollider3D::pad_));
    expect_pad<MeshCollider3D>("MeshCollider3D", 0);
    expect_pad<Foliage3D>("Foliage3D", sizeof(Foliage3D::mesh_pad_));
    expect_pad<Terrain3D>("Terrain3D", 0);
    expect_pad<TerrainCollider3D>("TerrainCollider3D", 0);
    expect_pad<TerrainFoliage3D>("TerrainFoliage3D", sizeof(TerrainFoliage3D::pad_) + sizeof(TerrainFoliage3D::pad2_));
    expect_pad<Decal3D>("Decal3D", 0);
    expect_pad<ParticleEmitter3D>("ParticleEmitter3D", sizeof(ParticleEmitter3D::pad_) + sizeof(ParticleEmitter3D::pad2_));
    expect_pad<WetSurfaces>("WetSurfaces", sizeof(WetSurfaces::pad_));  // wet_surfaces.h (M-WETSURFACE 5c)
    expect_pad<WindSway>("WindSway", sizeof(WindSway::pad_));  // wind_sway.h (M-WINDSWAY 1)
    expect_pad<SnowCover>("SnowCover", sizeof(SnowCover::pad_));  // snow_cover.h (M-SNOWCOVER 6c)
    expect_pad<WindDirection>("WindDirection", sizeof(WindDirection::pad_));  // wind_direction.h (M-WINDDIR 8a)
    expect_pad<RainSplash>("RainSplash", sizeof(RainSplash::pad_));  // rain_splash.h (M-RAINFX 7f)
    expect_pad<ThunderDelay>("ThunderDelay", sizeof(ThunderDelay::pad_));  // thunder_delay.h (M-RAINFX 7e)
    expect_pad<RainRipples>("RainRipples", 0);  // rain_ripples.h (M-RAINFX 7c): four f32, no padding
    expect_pad<WeatherAudio>("WeatherAudio", sizeof(WeatherAudio::pad_));  // weather_audio.h (M-WEATHERAUDIO 10a)
    expect_pad<CloudLayer>("CloudLayer", 0);  // cloud_layer.h (M-CLOUDS 9c): six f32, no padding
}

TEST_CASE("padding: ParticleEmitter3D copied from a dirty stack hashes the same as from a clean one") {
    alignas(ParticleEmitter3D) unsigned char dirty[sizeof(ParticleEmitter3D)];
    alignas(ParticleEmitter3D) unsigned char clean[sizeof(ParticleEmitter3D)];
    std::memset(dirty, 0xA5, sizeof(dirty));
    std::memset(clean, 0x00, sizeof(clean));
    // Field-by-field assignment over existing memory keeps whatever padding the memory held.
    auto* d = new (dirty) ParticleEmitter3D;
    auto* c = new (clean) ParticleEmitter3D;
    d->rate = 123.0f;
    c->rate = 123.0f;
    usize differing = 0;
    for (usize i = 0; i < sizeof(ParticleEmitter3D); ++i) differing += dirty[i] != clean[i] ? 1u : 0u;
    CHECK(differing == 0);
}
