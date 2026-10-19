#include "mce/editor/asset_thumb.h"

#include <algorithm>
#include <cmath>

#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/asset/file_io.h"
#include "mce/cook/fs.h"
#include "mce/cook/json.h"
#include "mce/editor/project.h"
#include "mce/math/math.h"

namespace mce::editor {
namespace {

constexpr f32 kDegToRad = 0.017453292519943295f;

void fill(cook::Image& img, u32 size, u8 r, u8 g, u8 b) {
    img.width = size;
    img.height = size;
    img.rgba.resize(static_cast<usize>(size) * size * 4);
    for (usize i = 0; i < img.rgba.size(); i += 4) {
        img.rgba[i] = r;
        img.rgba[i + 1] = g;
        img.rgba[i + 2] = b;
        img.rgba[i + 3] = 255;
    }
}

void put(cook::Image& img, u32 x, u32 y, u8 r, u8 g, u8 b) {
    u8* p = &img.rgba[(static_cast<usize>(y) * img.width + x) * 4];
    p[0] = r;
    p[1] = g;
    p[2] = b;
}

u32 clamp_size(u32 size) { return std::min(std::max(size, kThumbMin), kThumbMax); }

u8 mix(u32 a, u32 b, u32 alpha) { return static_cast<u8>((a * alpha + b * (255 - alpha) + 127) / 255); }

}  // namespace

void thumb_from_image(const cook::Image& src, u32 size, cook::Image& out) {
    size = clamp_size(size);
    fill(out, size, 40, 40, 40);
    if (src.width == 0 || src.height == 0 || src.rgba.size() < static_cast<usize>(src.width) * src.height * 4) return;
    // Fit inside size x size, keeping the proportion.
    const u32 longest = std::max(src.width, src.height);
    const u32 w = std::max<u32>(1, static_cast<u32>(static_cast<u64>(src.width) * size / longest));
    const u32 h = std::max<u32>(1, static_cast<u32>(static_cast<u64>(src.height) * size / longest));
    const u32 x0 = (size - w) / 2, y0 = (size - h) / 2;
    for (u32 y = 0; y < h; ++y) {
        const u32 sy0 = static_cast<u32>(static_cast<u64>(y) * src.height / h);
        const u32 sy1 = std::max(sy0 + 1, static_cast<u32>(static_cast<u64>(y + 1) * src.height / h));
        for (u32 x = 0; x < w; ++x) {
            const u32 sx0 = static_cast<u32>(static_cast<u64>(x) * src.width / w);
            const u32 sx1 = std::max(sx0 + 1, static_cast<u32>(static_cast<u64>(x + 1) * src.width / w));
            u64 acc[4] = {};
            u32 n = 0;
            for (u32 sy = sy0; sy < sy1 && sy < src.height; ++sy)
                for (u32 sx = sx0; sx < sx1 && sx < src.width; ++sx) {
                    const u8* p = &src.rgba[(static_cast<usize>(sy) * src.width + sx) * 4];
                    // Premultiplied so transparent pixels do not bleed their colour.
                    acc[0] += static_cast<u64>(p[0]) * p[3];
                    acc[1] += static_cast<u64>(p[1]) * p[3];
                    acc[2] += static_cast<u64>(p[2]) * p[3];
                    acc[3] += p[3];
                    ++n;
                }
            const u32 alpha = n ? static_cast<u32>((acc[3] + n / 2) / n) : 0;
            const u32 r = acc[3] ? static_cast<u32>(acc[0] / acc[3]) : 0;
            const u32 g = acc[3] ? static_cast<u32>(acc[1] / acc[3]) : 0;
            const u32 b = acc[3] ? static_cast<u32>(acc[2] / acc[3]) : 0;
            const u32 check = (((x0 + x) / 8 + (y0 + y) / 8) & 1u) ? 150 : 100;
            put(out, x0 + x, y0 + y, mix(r, check, alpha), mix(g, check, alpha), mix(b, check, alpha));
        }
    }
}

void thumb_from_pcm(const cook::PcmClip& clip, u32 size, cook::Image& out) {
    size = clamp_size(size);
    fill(out, size, 24, 28, 36);
    const u64 frames = clip.frames();
    const u32 mid = size / 2;
    for (u32 x = 0; x < size; ++x) put(out, x, mid, 60, 66, 80);
    if (frames == 0) return;
    for (u32 x = 0; x < size; ++x) {
        const u64 f0 = frames * x / size;
        const u64 f1 = std::max(f0 + 1, frames * (x + 1) / size);
        f32 lo = 0.0f, hi = 0.0f;
        for (u64 f = f0; f < f1 && f < frames; ++f) {
            f32 v = 0.0f;
            for (u32 c = 0; c < clip.channels; ++c) v += clip.samples[f * clip.channels + c];
            v /= static_cast<f32>(clip.channels);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        const f32 half = static_cast<f32>(size) * 0.5f - 1.0f;
        const i32 top = static_cast<i32>(mid) - static_cast<i32>(std::lround(std::min(hi, 1.0f) * half));
        const i32 bottom = static_cast<i32>(mid) - static_cast<i32>(std::lround(std::max(lo, -1.0f) * half));
        for (i32 y = std::max(top, 0); y <= std::min(bottom, static_cast<i32>(size) - 1); ++y) put(out, x, static_cast<u32>(y), 90, 200, 140);
    }
}

bool thumb_from_mesh(const asset::MeshView& mesh, u32 size, cook::Image& out) {
    size = clamp_size(size);
    fill(out, size, 48, 52, 60);
    // Dequantised vertices of every submesh, in submesh space (node placement is ignored: a thumbnail).
    Array<math::vec3> pos;
    Array<u32> tris;
    for (u32 s = 0; s < mesh.submeshes.size(); ++s) {
        const asset::MeshSubmesh& sm = mesh.submeshes[s];
        const u32 base = static_cast<u32>(pos.size());
        for (u32 i = 0; i < sm.vertex_count; ++i) {
            const asset::MeshVertex& q = mesh.vertices[sm.base_vertex + i];
            pos.push_back({sm.center[0] + static_cast<f32>(q.position[0]) / 32767.0f * sm.extent[0],
                           sm.center[1] + static_cast<f32>(q.position[1]) / 32767.0f * sm.extent[1],
                           sm.center[2] + static_cast<f32>(q.position[2]) / 32767.0f * sm.extent[2]});
        }
        for (u32 i = 0; i + 2 < sm.index_count; i += 3) {
            tris.push_back(base + mesh.index(sm.first_index + i));
            tris.push_back(base + mesh.index(sm.first_index + i + 1));
            tris.push_back(base + mesh.index(sm.first_index + i + 2));
        }
    }
    if (tris.empty()) return false;
    // A fixed three-quarter view: yaw 35 degrees about Y, pitch 25 degrees about X.
    const math::quat view = math::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, 25.0f * kDegToRad) *
                            math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 35.0f * kDegToRad);
    for (usize i = 0; i < pos.size(); ++i) pos[i] = math::rotate(view, pos[i]);
    math::vec3 lo = pos[0], hi = pos[0];
    for (usize i = 1; i < pos.size(); ++i) {
        lo = {std::fmin(lo.x, pos[i].x), std::fmin(lo.y, pos[i].y), std::fmin(lo.z, pos[i].z)};
        hi = {std::fmax(hi.x, pos[i].x), std::fmax(hi.y, pos[i].y), std::fmax(hi.z, pos[i].z)};
    }
    const f32 span = std::fmax(std::fmax(hi.x - lo.x, hi.y - lo.y), 1e-6f);
    const f32 scale = (static_cast<f32>(size) - 8.0f) / span;
    const f32 cx = 0.5f * (lo.x + hi.x), cy = 0.5f * (lo.y + hi.y);
    const f32 half = 0.5f * static_cast<f32>(size);
    Array<f32> depth;
    depth.resize(static_cast<usize>(size) * size);
    for (usize i = 0; i < depth.size(); ++i) depth[i] = -1e30f;
    const math::vec3 light = math::vec3{-0.4f, 0.6f, 0.7f} * (1.0f / std::sqrt(0.16f + 0.36f + 0.49f));
    for (usize t = 0; t + 2 < tris.size(); t += 3) {
        const math::vec3 a = pos[tris[t]], b = pos[tris[t + 1]], c = pos[tris[t + 2]];
        math::vec3 n = math::cross(b - a, c - a);
        const f32 len = math::length(n);
        if (len < 1e-12f) continue;
        n = n * (1.0f / len);
        if (n.z < 0.0f) n = n * -1.0f;  // two-sided
        const f32 lit = 0.25f + 0.75f * std::fmax(math::dot(n, light), 0.0f);
        const u8 shade_r = static_cast<u8>(std::lround(170.0f * lit)), shade_g = static_cast<u8>(std::lround(190.0f * lit)),
                 shade_b = static_cast<u8>(std::lround(220.0f * lit));
        auto sx = [&](const math::vec3& p) { return half + (p.x - cx) * scale; };
        auto sy = [&](const math::vec3& p) { return half - (p.y - cy) * scale; };
        const f32 ax = sx(a), ay = sy(a), bx = sx(b), by = sy(b), cxp = sx(c), cyp = sy(c);
        const f32 denom = (by - cyp) * (ax - cxp) + (cxp - bx) * (ay - cyp);
        if (std::fabs(denom) < 1e-9f) continue;
        const i32 x0 = std::max(0, static_cast<i32>(std::floor(std::fmin(ax, std::fmin(bx, cxp)))));
        const i32 x1 = std::min(static_cast<i32>(size) - 1, static_cast<i32>(std::ceil(std::fmax(ax, std::fmax(bx, cxp)))));
        const i32 y0 = std::max(0, static_cast<i32>(std::floor(std::fmin(ay, std::fmin(by, cyp)))));
        const i32 y1 = std::min(static_cast<i32>(size) - 1, static_cast<i32>(std::ceil(std::fmax(ay, std::fmax(by, cyp)))));
        for (i32 y = y0; y <= y1; ++y)
            for (i32 x = x0; x <= x1; ++x) {
                const f32 px = static_cast<f32>(x) + 0.5f, py = static_cast<f32>(y) + 0.5f;
                const f32 w0 = ((by - cyp) * (px - cxp) + (cxp - bx) * (py - cyp)) / denom;
                const f32 w1 = ((cyp - ay) * (px - cxp) + (ax - cxp) * (py - cyp)) / denom;
                const f32 w2 = 1.0f - w0 - w1;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                const f32 z = w0 * a.z + w1 * b.z + w2 * c.z;
                f32& d = depth[static_cast<usize>(y) * size + static_cast<usize>(x)];
                if (z <= d) continue;
                d = z;
                put(out, static_cast<u32>(x), static_cast<u32>(y), shade_r, shade_g, shade_b);
            }
    }
    return true;
}

namespace {

f32 saturate(f32 v) { return std::fmin(std::fmax(v, 0.0f), 1.0f); }

u8 to_srgb8(f32 linear) { return static_cast<u8>(std::lround(std::pow(saturate(linear), 1.0f / 2.2f) * 255.0f)); }

}  // namespace

void thumb_from_material(const MaterialSwatch& m, u32 size, cook::Image& out) {
    size = clamp_size(size);
    fill(out, size, 40, 40, 40);
    const f32 half = static_cast<f32>(size) * 0.5f;
    const f32 radius = static_cast<f32>(size) * 0.42f;
    const f32 roughness = std::fmax(saturate(m.roughness), 0.045f);
    const f32 metallic = saturate(m.metallic);
    const math::vec3 light = math::normalize(math::vec3(-0.5f, 0.6f, 0.62f));
    const math::vec3 view(0.0f, 0.0f, 1.0f);
    const math::vec3 half_v = math::normalize(light + view);
    const f32 shininess = std::fmin(std::fmax(2.0f / (roughness * roughness * roughness * roughness) - 2.0f, 2.0f), 2000.0f);
    const f32 spec_norm = (shininess + 8.0f) / 25.1327f;  // (n + 8) / 8 pi
    const f32 alpha = m.blend ? saturate(m.base[3]) : 1.0f;
    for (u32 y = 0; y < size; ++y)
        for (u32 x = 0; x < size; ++x) {
            const f32 dx = (static_cast<f32>(x) + 0.5f - half) / radius;
            const f32 dy = (half - static_cast<f32>(y) - 0.5f) / radius;
            const f32 r2 = dx * dx + dy * dy;
            if (r2 > 1.0f) continue;
            const math::vec3 n(dx, dy, std::sqrt(1.0f - r2));
            const f32 ndl = std::fmax(math::dot(n, light), 0.0f);
            const f32 ndh = std::fmax(math::dot(n, half_v), 0.0f);
            const f32 ndv = std::fmax(n.z, 0.0f);
            f32 c[3];
            for (u32 k = 0; k < 3; ++k) {
                const f32 base = m.base[k];
                if (m.unlit) {
                    c[k] = base;
                    continue;
                }
                const f32 f0 = 0.04f + (base - 0.04f) * metallic;
                const f32 fres = f0 + (1.0f - f0) * std::pow(1.0f - ndv, 5.0f) * (1.0f - roughness);
                const f32 diffuse = base * (1.0f - metallic) * (ndl * 1.1f + 0.12f);
                const f32 spec = fres * std::pow(ndh, shininess) * spec_norm * ndl;
                // A tinted sky above, a dark ground below, reflected more by smooth and by metallic surfaces.
                const f32 sky = n.y * 0.5f + 0.5f;
                const f32 env = (0.18f + 0.55f * sky) * fres * (1.0f - roughness * 0.8f);
                c[k] = diffuse + spec * 2.0f + env + m.emissive[k];
            }
            const u32 check = (((x / 8) + (y / 8)) & 1u) ? 150 : 100;
            const u32 a = static_cast<u32>(std::lround(alpha * 255.0f));
            put(out, x, y, mix(to_srgb8(c[0]), check, a), mix(to_srgb8(c[1]), check, a), mix(to_srgb8(c[2]), check, a));
        }
}

void thumb_tile(asset::ChunkType type, u32 size, cook::Image& out) {
    size = clamp_size(size);
    const u32 t = static_cast<u32>(type);
    const u8 r = static_cast<u8>(70 + (t * 53u) % 120u), g = static_cast<u8>(70 + (t * 97u) % 120u), b = static_cast<u8>(70 + (t * 29u) % 120u);
    fill(out, size, r, g, b);
    for (u32 i = 0; i < size; ++i) {  // a one pixel frame
        put(out, i, 0, 20, 20, 20);
        put(out, i, size - 1, 20, 20, 20);
        put(out, 0, i, 20, 20, 20);
        put(out, size - 1, i, 20, 20, 20);
    }
}

namespace {

// The average colour of a texture's source PNG (alpha-weighted), in 0..1 sRGB; false when it does not read.
bool texture_average(EditorProject& project, asset::Guid guid, f32 out[3]) {
    const ProjectAsset* tex = project.find(guid);
    if (!tex || tex->type != asset::ChunkType::Texture) return false;
    Array<byte> bytes;
    if (!asset::read_file(project.asset_path(tex->path.view()).view(), bytes)) return false;
    Result<cook::Image> img = cook::decode_png(bytes.span());
    if (!img || img.value().width == 0 || img.value().height == 0) return false;
    f64 acc[3] = {};
    f64 weight = 0;
    const cook::Image& im = img.value();
    for (usize i = 0; i + 3 < im.rgba.size(); i += 4) {
        const f64 a = im.rgba[i + 3];
        for (u32 k = 0; k < 3; ++k) acc[k] += a * im.rgba[i + k];
        weight += a;
    }
    if (weight <= 0) return false;
    for (u32 k = 0; k < 3; ++k) out[k] = static_cast<f32>(acc[k] / weight / 255.0);
    return true;
}

f32 srgb_to_linear(f32 v) { return std::pow(saturate(v), 2.2f); }

// Reads the scalar look of a .mcmat source file; the base colour texture's average colour multiplies the base colour.
bool read_material_swatch(EditorProject& project, const ProjectAsset& asset, MaterialSwatch& m) {
    cook::String text;
    if (!cook::fs::read_text(project.asset_path(asset.path.view()).view(), text)) return false;
    cook::JsonDoc doc;
    cook::JsonError err;
    if (!doc.parse(text.view(), err) || !doc.root().is_object()) return false;
    const cook::JsonValue root = doc.root();
    auto number = [&](const char* key, f32& out) {
        const cook::JsonValue v = root.get(key);
        if (v.valid() && v.is_number()) out = static_cast<f32>(v.as_number());
    };
    auto vec = [&](const char* key, f32* out, u32 n) {
        const cook::JsonValue v = root.get(key);
        if (!v.valid() || !v.is_array() || v.size() != n) return;
        for (u32 i = 0; i < n; ++i)
            if (v.at(i).is_number()) out[i] = static_cast<f32>(v.at(i).as_number());
    };
    vec("base_color", m.base, 4);
    vec("emissive", m.emissive, 3);
    number("metallic", m.metallic);
    number("roughness", m.roughness);
    const cook::JsonValue mode = root.get("alpha_mode");
    m.blend = mode.valid() && mode.is_string() && mode.as_string() == "blend";
    const cook::JsonValue unlit = root.get("unlit");
    m.unlit = unlit.valid() && unlit.is_bool() && unlit.as_bool();
    const cook::JsonValue tex = root.get("textures").get("base_color");
    if (tex.valid() && tex.is_string()) {
        StringView ref = tex.as_string();
        if (ref.size() > 5 && ref.substr(0, 5) == "guid:") ref = ref.substr(5);
        const Result<asset::Guid> g = asset::parse_guid(ref);
        f32 avg[3];
        if (g && texture_average(project, g.value(), avg))
            for (u32 k = 0; k < 3; ++k) m.base[k] *= srgb_to_linear(avg[k]);
    }
    return true;
}

}  // namespace

bool render_thumbnail(EditorProject& project, const ProjectAsset& asset, u32 size, cook::Image& out) {
    size = clamp_size(size);
    if (asset.type == asset::ChunkType::Texture || asset.type == asset::ChunkType::Sfx) {
        const cook::String path = project.asset_path(asset.path.view());
        Array<byte> bytes;
        if (asset::read_file(path.view(), bytes)) {
            if (asset.type == asset::ChunkType::Texture) {
                if (Result<cook::Image> img = cook::decode_png(bytes.span())) {
                    thumb_from_image(img.value(), size, out);
                    return true;
                }
            } else {
                cook::PcmClip clip;
                FixedString<128> why;
                if (cook::decode_wav(bytes.span(), clip, why)) {
                    thumb_from_pcm(clip, size, out);
                    return true;
                }
            }
        }
    } else if (asset.type == asset::ChunkType::Material) {
        MaterialSwatch swatch;
        if (read_material_swatch(project, asset, swatch)) {
            thumb_from_material(swatch, size, out);
            return true;
        }
    } else if (asset.type == asset::ChunkType::Mesh) {
        asset::AssetSystem* assets = project.mounted();
        if (assets) {
            const asset::AssetKey key = assets->find(asset.guid);
            if (key != asset::kInvalidAssetKey) {
                Result<asset::AssetHandle<asset::Mesh>> h = assets->load_sync<asset::Mesh>(key);
                if (h) {
                    bool shown = false;
                    if (Result<asset::MeshView> v = asset::MeshView::read(assets->resolve(h.value()))) shown = thumb_from_mesh(v.value(), size, out);
                    assets->release(asset::AnyHandle(h.value()));
                    if (shown) return true;
                }
            }
        }
    }
    thumb_tile(asset.type, size, out);
    return false;
}

}  // namespace mce::editor
