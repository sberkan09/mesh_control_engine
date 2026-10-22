#include "mce/kit/lightning_bolt.h"

#include <algorithm>
#include <cmath>

#include "kit3d_internal.h"
#include "mce/asset/formats.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/precipitation.h"
#include "mce/kit/weather.h"
#include "mce/log.h"
#include "mce/render/scene3d.h"

namespace mce::kit {
namespace {

constexpr f64 kPi = 3.14159265358979323846;
constexpr f64 kNominalSpeed = 343.0;
constexpr f64 kMaxTicks = 2147483648.0;  // 2^31

// The same mixer as precipitation.cpp (kept private there): the strike streams share one seed.
u32 hash_mix(u32 seed, u32 counter) {
    u64 z = (static_cast<u64>(seed) << 32 | counter) + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<u32>(z >> 32);
}

f64 range_of(f32 v, f64 fallback) {
    if (std::isnan(v)) return fallback;
    return std::clamp(static_cast<f64>(v), 10.0, 5000.0);
}

f64 unit_of(u32 h) { return static_cast<f64>(h) / 4294967296.0; }

struct P3 {
    f64 v[3];
};

f64 dot3(const P3& a, const P3& b) { return a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2]; }

// 2^n + 1 points must fit `cap`: n is lowered until they do; 0 when even n = 1 does not.
u32 fit_rounds(u32 want, u32 cap) {
    u32 n = std::min(want, 7u);
    while (n > 1 && (1u << n) + 1u > cap) --n;
    return (1u << n) + 1u <= cap ? n : 0u;
}

// Midpoint displacement of pts[0] .. pts[2^rounds] (the two ends set): round r moves each midpoint by
// jitter * segment length * 0.5^r * (2h - 1) along two perpendiculars (the horizontal axis `h` made orthogonal to the
// segment, and their cross product). The hashes are keyed by (salt, round, segment), nothing else.
void displace(P3* pts, u32 rounds, f64 jitter, u32 salt, const P3& h) {
    const u32 n = 1u << rounds;
    for (u32 r = 0; r < rounds; ++r) {
        const u32 step = n >> r, half = step >> 1;
        const f64 scale = jitter * std::pow(0.5, static_cast<f64>(r));
        for (u32 i = 0, seg = 0; i < n; i += step, ++seg) {
            const P3& a = pts[i];
            const P3& b = pts[i + step];
            P3 d{{b.v[0] - a.v[0], b.v[1] - a.v[1], b.v[2] - a.v[2]}};
            const f64 len = std::sqrt(dot3(d, d));
            P3 m{{(a.v[0] + b.v[0]) * 0.5, (a.v[1] + b.v[1]) * 0.5, (a.v[2] + b.v[2]) * 0.5}};
            if (len > 0.0 && scale > 0.0) {
                const P3 u{{d.v[0] / len, d.v[1] / len, d.v[2] / len}};
                const f64 hu = dot3(h, u);
                P3 e1{{h.v[0] - u.v[0] * hu, h.v[1] - u.v[1] * hu, h.v[2] - u.v[2] * hu}};
                f64 l1 = std::sqrt(dot3(e1, e1));
                if (l1 < 1e-9) {  // a segment along h: any perpendicular
                    e1 = P3{{u.v[1], -u.v[0], 0.0}};
                    l1 = std::sqrt(dot3(e1, e1));
                    if (l1 < 1e-9) {
                        e1 = P3{{1.0, 0.0, 0.0}};
                        l1 = 1.0;
                    }
                }
                for (f64& c : e1.v) c /= l1;
                const P3 e2{{u.v[1] * e1.v[2] - u.v[2] * e1.v[1], u.v[2] * e1.v[0] - u.v[0] * e1.v[2],
                             u.v[0] * e1.v[1] - u.v[1] * e1.v[0]}};
                const u32 key = (r << 8) | seg;
                const f64 s = scale * len;
                const f64 k1 = s * (2.0 * unit_of(hash_mix(salt, key)) - 1.0);
                const f64 k2 = s * (2.0 * unit_of(hash_mix(salt, key | 0x10000u)) - 1.0);
                for (int c = 0; c < 3; ++c) m.v[c] += e1.v[c] * k1 + e2.v[c] * k2;
            }
            pts[i + half] = m;
        }
    }
}

P3 horizontal_axis(u32 id) {
    const f64 a = 2.0 * kPi * unit_of(hash_mix(id, 0xA11CEu));
    return P3{{std::cos(a), 0.0, std::sin(a)}};
}

f64 finite_or(f32 v, f64 fallback) { return std::isnan(v) ? fallback : static_cast<f64>(v); }

// The unit box (half 0.5, one node, one submesh) every bolt segment is an instance of; its material is the bolt's.
void unit_box_blob(Array<byte>& out) {
    Array<asset::MeshVertex> verts;
    Array<u16> idx;
    const auto snorm = [](f64 v) { return static_cast<i16>(std::lround(std::clamp(v, -1.0, 1.0) * 32767.0)); };
    const f64 axes[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    u16 local = 0;
    for (const auto& n : axes) {
        const P3 nn{{n[0], n[1], n[2]}};
        P3 u = std::fabs(nn.v[1]) > 0.5 ? P3{{1, 0, 0}} : P3{{0, 1, 0}};
        const P3 v{{nn.v[1] * u.v[2] - nn.v[2] * u.v[1], nn.v[2] * u.v[0] - nn.v[0] * u.v[2], nn.v[0] * u.v[1] - nn.v[1] * u.v[0]}};
        u = P3{{v.v[1] * nn.v[2] - v.v[2] * nn.v[1], v.v[2] * nn.v[0] - v.v[0] * nn.v[2], v.v[0] * nn.v[1] - v.v[1] * nn.v[0]}};
        const f64 su[4] = {-1, 1, 1, -1}, sv[4] = {-1, -1, 1, 1};
        for (int c = 0; c < 4; ++c) {
            asset::MeshVertex q{};
            q.position[0] = snorm(nn.v[0] + u.v[0] * su[c] + v.v[0] * sv[c]);
            q.position[1] = snorm(nn.v[1] + u.v[1] * su[c] + v.v[1] * sv[c]);
            q.position[2] = snorm(nn.v[2] + u.v[2] * su[c] + v.v[2] * sv[c]);
            f64 ox = nn.v[0], oy = nn.v[1];
            if (nn.v[2] < 0) {
                ox = (1.0 - std::fabs(nn.v[1])) * (nn.v[0] >= 0 ? 1.0 : -1.0);
                oy = (1.0 - std::fabs(nn.v[0])) * (nn.v[1] >= 0 ? 1.0 : -1.0);
            }
            q.normal[0] = snorm(ox);
            q.normal[1] = snorm(oy);
            verts.push_back(q);
        }
        for (int i : {0, 1, 2, 2, 3, 0}) idx.push_back(static_cast<u16>(local + i));
        local = static_cast<u16>(local + 4);
    }
    asset::MeshNode node{};
    node.parent = -1;
    node.scale[0] = node.scale[1] = node.scale[2] = 1.0f;
    node.rotation[3] = 1.0f;
    asset::MeshSubmesh sm{};
    sm.index_count = 36;
    sm.vertex_count = local;
    sm.extent[0] = sm.extent[1] = sm.extent[2] = 0.5f;
    sm.uv_scale[0] = sm.uv_scale[1] = 1.0f;
    sm.material.base_color[3] = 1.0f;
    sm.material.emissive[0] = sm.material.emissive[1] = sm.material.emissive[2] = 1.0f;
    sm.material.roughness = 1.0f;
    asset::MeshData d;
    d.vertices = verts.span();
    d.indices = std::as_bytes(idx.span());
    d.submeshes = Span<const asset::MeshSubmesh>(&sm, 1);
    d.nodes = Span<const asset::MeshNode>(&node, 1);
    for (int k = 0; k < 3; ++k) {
        d.bounds_min[k] = -0.5f;
        d.bounds_max[k] = 0.5f;
    }
    asset::write_mesh(d, out);
}

constexpr math::vec3 kBoltRadiance{6.0f, 7.5f, 12.0f};  // HDR blue-white (pushes into bloom) at intensity 1

}  // namespace

BoltStrike bolt_strike_position(const LightningBolt& b, u32 seed, u32 counter, const math::vec3& origin) {
    f64 lo = range_of(b.min_range_m, 150.0), hi = range_of(b.max_range_m, 900.0);
    if (lo > hi) std::swap(lo, hi);
    const f64 ub = static_cast<f64>(hash_mix(seed, counter * 4u + 2u)) / 4294967296.0;
    const f64 ur = static_cast<f64>(hash_mix(seed, counter * 4u + 3u)) / 4294967296.0;
    const f64 bearing = 2.0 * kPi * ub;
    const f64 range = lo + (hi - lo) * ur;
    const f64 gy = std::isnan(b.ground_y) ? 0.0 : static_cast<f64>(b.ground_y);
    const f64 x = static_cast<f64>(origin.x) + range * std::sin(bearing);
    const f64 z = static_cast<f64>(origin.z) + range * std::cos(bearing);
    const f64 dx = x - static_cast<f64>(origin.x), dz = z - static_cast<f64>(origin.z);
    const f64 dy = gy - static_cast<f64>(origin.y);
    BoltStrike s;
    s.x = static_cast<f32>(x);
    s.z = static_cast<f32>(z);
    s.distance_m = static_cast<f32>(std::sqrt(dx * dx + dy * dy + dz * dz));
    return s;
}

u32 bolt_delay_ticks(f32 distance_m, f64 dt) {
    if (!(dt > 0.0) || !(distance_m > 0.0f)) return 1;
    const f64 n = std::ceil(static_cast<f64>(distance_m) / kNominalSpeed / dt);
    if (!(n >= 1.0)) return 1;
    return static_cast<u32>(std::min(n, kMaxTicks));
}

u32 build_bolt(const LightningBolt& b, f32 sx, f32 sz, u32 id, math::vec3* out, u32 cap) {
    if (!out) return 0;
    const u32 rounds = fit_rounds(std::clamp(b.segments_log2, 2u, 7u), cap);
    if (rounds == 0) return 0;
    const u32 n = 1u << rounds;
    const f64 gy = finite_or(b.ground_y, 0.0);
    const f64 height = std::clamp(finite_or(b.height_m, 600.0), 50.0, 3000.0);
    const f64 jitter = std::clamp(finite_or(b.jitter, 0.0), 0.0, 0.6);
    const P3 h = horizontal_axis(id);
    const f64 off = (2.0 * unit_of(hash_mix(id, 0x57A27u)) - 1.0) * 3.0;  // a few metres off the strike's vertical
    P3 pts[kBoltMaxPoints];
    pts[0] = P3{{static_cast<f64>(sx) + h.v[0] * off, gy + height, static_cast<f64>(sz) + h.v[2] * off}};
    pts[n] = P3{{static_cast<f64>(sx), gy, static_cast<f64>(sz)}};
    displace(pts, rounds, jitter, id, h);
    for (u32 i = 0; i <= n; ++i)
        out[i] = math::vec3{static_cast<f32>(pts[i].v[0]), static_cast<f32>(pts[i].v[1]), static_cast<f32>(pts[i].v[2])};
    out[n] = math::vec3{sx, static_cast<f32>(gy), sz};  // exactly the strike point
    return n + 1u;
}

u32 build_bolt_fork(const LightningBolt& b, u32 id, const math::vec3* bolt, u32 bolt_count, math::vec3* out, u32 cap) {
    if (!out || !bolt || !b.fork || bolt_count < 3) return 0;
    const u32 main_n = bolt_count - 1u;
    u32 want = 1;
    while ((1u << (want + 1)) < main_n && want < 6) ++want;  // log2(main segments) - 1
    const u32 rounds = fit_rounds(std::max(want, 1u), cap);
    if (rounds == 0) return 0;
    const u32 n = 1u << rounds;
    const f64 jitter = std::clamp(finite_or(b.jitter, 0.0), 0.0, 0.6);
    const f64 gy = finite_or(b.ground_y, 0.0);
    const u32 at = main_n / 4u + hash_mix(id, 0xF04Cu) % std::max(main_n / 2u, 1u);  // 1/4 .. 3/4 of the bolt
    const math::vec3 s = bolt[std::min(at, main_n)];
    const P3 h = horizontal_axis(id);
    const f64 sign = (hash_mix(id, 0xF04Du) & 1u) ? 1.0 : -1.0;
    const f64 drop = std::max(static_cast<f64>(s.y) - gy, 1.0) * 0.5;  // half of what is left below the branch point
    P3 pts[kBoltMaxForkPoints];
    pts[0] = P3{{s.x, s.y, s.z}};
    pts[n] = P3{{s.x + sign * h.v[0] * drop, s.y - drop, s.z + sign * h.v[2] * drop}};
    displace(pts, rounds, jitter, id ^ 0xF04C0000u, h);
    for (u32 i = 0; i <= n; ++i)
        out[i] = math::vec3{static_cast<f32>(pts[i].v[0]), static_cast<f32>(pts[i].v[1]), static_cast<f32>(pts[i].v[2])};
    out[0] = s;
    return n + 1u;
}

f32 bolt_intensity(u32 id, u32 active, u32 flash_ticks) {
    if (active == 0 || flash_ticks == 0 || active > flash_ticks) return 0.0f;
    if (active == flash_ticks) return 1.0f;
    if (active + 1u == flash_ticks && (hash_mix(id, 0x5E57u) & 1u)) return 1.0f;  // the re-strike
    const f32 t = static_cast<f32>(active) / static_cast<f32>(flash_ticks);
    return t * t;
}

// Path (1) of the milestone: one unit box mesh in the writable store, one MeshInstance per segment, an emissive
// material, nothing written to the world.
void Kit3DExtract::release_bolt_mesh() {
    if (writable_ && bolt_key_ != ~0u) writable_->release(bolt_key_);
    bolt_key_ = ~0u;
    bolt_failed_ = false;
}

void Kit3DExtract::extract_lightning_bolt(World& world, render::Scene3D& scene) {
    if (bake_mode_ || !quality_.clouds) return;  // Low turns the weather looks off (the clouds switch)
    const LightningBolt* bolt = nullptr;
    world.query<const LightningBolt&>().each([&](const LightningBolt& c) {
        if (!bolt) bolt = &c;
    });
    if (!bolt || !bolt->enabled || bolt->active == 0) return;
    const Weather* weather = nullptr;
    world.query<const Weather&>().each([&](const Weather& c) {
        if (!weather) weather = &c;
    });
    if (!weather || weather->current != WeatherKind::Storm) return;
    u32 flash_ticks = 0;
    bool have_lightning = false;
    world.query<const Lightning&>().each([&](const Lightning& l) {
        if (have_lightning) return;
        have_lightning = true;
        flash_ticks = l.flash_ticks;
    });
    if (!have_lightning) return;
    const f32 inten = bolt_intensity(bolt->strike_id, bolt->active, flash_ticks);
    if (!(inten > 0.0f) || !writable_) return;
    if (bolt_key_ == ~0u) {
        if (bolt_failed_) return;
        Array<byte> blob;
        unit_box_blob(blob);
        const u32 key = writable_->allocate_key();
        if (!writable_->upload(key, 1, blob.span())) {
            writable_->release(key);
            bolt_failed_ = true;
            MCE_LOG_WARN("kit3d: the lightning bolt mesh did not upload; bolts are not drawn");
            return;
        }
        bolt_key_ = key;
        bolt_material_ = render::MaterialOverride{};
        bolt_material_.params.base_color[3] = 1.0f;
        for (f32& e : bolt_material_.params.emissive) e = 1.0f;
        bolt_material_.params.roughness = 1.0f;
    }
    math::vec3 pts[kBoltMaxPoints], fork[kBoltMaxForkPoints];
    const u32 count = build_bolt(*bolt, bolt->strike_x, bolt->strike_z, bolt->strike_id, pts, kBoltMaxPoints);
    if (count < 2) return;
    const u32 fork_count = build_bolt_fork(*bolt, bolt->strike_id, pts, count, fork, kBoltMaxForkPoints);
    // The width is given at 100 m: scaled with the distance so a far bolt keeps a readable angular width.
    const math::vec3 strike{bolt->strike_x, std::isnan(bolt->ground_y) ? 0.0f : bolt->ground_y, bolt->strike_z};
    const f32 dist = math::length(strike - scene.view().position);
    const f32 width = std::clamp(std::isnan(bolt->width_m) ? 1.5f : bolt->width_m, 0.05f, 20.0f) *
                      std::clamp(dist / 100.0f, 0.1f, 100.0f);
    const auto draw = [&](const math::vec3* p, u32 n, f32 w, f32 gain) {
        for (u32 i = 0; i + 1 < n; ++i) {
            const math::vec3 d = p[i + 1] - p[i];
            const f32 len = math::length(d);
            if (!(len > 1e-4f)) continue;
            const math::vec3 z = d / len;
            const math::vec3 ref = std::fabs(z.y) < 0.9f ? math::vec3{0.0f, 1.0f, 0.0f} : math::vec3{1.0f, 0.0f, 0.0f};
            const math::vec3 x = math::normalize(math::cross(ref, z));
            const math::vec3 y = math::cross(z, x);
            render::MeshInstance inst;
            inst.mesh = bolt_key_;
            inst.world = math::trs((p[i] + p[i + 1]) * 0.5f, quat_from_basis(x, y, z), {w, w, len + w * 0.5f});
            inst.tint = {0.0f, 0.0f, 0.0f, 1.0f};  // only the emissive shows
            inst.emissive_tint = kBoltRadiance * (inten * gain);
            inst.cast_shadows = false;
            inst.skinning = false;
            inst.material = &bolt_material_;
            scene.add(inst);
        }
    };
    draw(pts, count, width, 1.0f);
    draw(fork, fork_count, width * 0.6f, 0.7f);
    if (bolt->light_intensity > 0.0f) {
        render::PointLight pl;
        pl.position = pts[count / 2];
        pl.color = {0.7f, 0.8f, 1.0f};
        pl.intensity = bolt->light_intensity * inten;
        pl.radius = std::clamp(std::isnan(bolt->light_radius_m) ? 400.0f : bolt->light_radius_m, 1.0f, 5000.0f);
        pl.cast_shadows = false;
        scene.add_point_light(pl);
    }
}

}  // namespace mce::kit
