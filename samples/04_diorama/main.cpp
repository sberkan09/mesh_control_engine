// 04_diorama — Artifact B (ROADMAP M4): a lit 3D city block kit-assembled from Kenney's CC0 3D
// starter kits, cooked by mce_cook into a pack at build time and loaded through the asset system.
// Orbit/zoom camera, one directional light with a shadow map, exp² fog, a walking character (glTF
// node clip), a spinning coin and drifting clouds; HUD through the 2D batch on top.
//
//   04_diorama                              run until the window closes
//   04_diorama -frames 120 -capture a.ppm   CI smoke: also write the last frame as a PPM image
//   04_diorama -headless 1 -ticks 300       determinism run: no GL, CPU mesh store, prints the hash
//
// Controls: drag (left mouse) orbits, wheel or Z/X zooms, arrows orbit, Space toggles the auto
// orbit, F toggles fog, Esc quits.
//
// The pack: -pack <path>, else assets.mce next to the executable (a shipped build), else the build
// tree's cooked pack (MCE_SAMPLE_DEV_PACK).

#include <cmath>
#include <cstdio>

#include "hud_font.h"
#include "mce/asset/asset_system.h"
#include "mce/core.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/render/asset_upload.h"
#include "mce/render/renderer.h"
#include "mce/rhi/default_device.h"
#include "ship_paths.h"

namespace {

using namespace mce;
using namespace mce::math;
using namespace mce::render;
using namespace mce::samples;

MCE_CVAR(cv_frames, "frames", 0, "Quit after N frames (0 = run until the window closes)", CVarFlags::None);
MCE_CVAR(cv_capture, "capture", "", "Write the last frame (with -frames) to this .ppm path", CVarFlags::None);
MCE_CVAR(cv_ticks, "ticks", 300, "Headless: ticks to simulate", CVarFlags::None);
MCE_CVAR(cv_pack, "pack", "", "Cooked pack to mount (default: assets.mce next to the executable)", CVarFlags::None);
MCE_CVAR(cv_gl_debug, "r.gl_debug", true, "Enable the GL_KHR_debug validation callback", CVarFlags::RequiresRestart);
MCE_CVAR(cv_msaa, "r.msaa", 4, "3D MSAA samples (1 = off)", CVarFlags::RequiresRestart);
MCE_CVAR(cv_yaw, "cam.yaw", 0.6f, "Initial orbit yaw (radians)", CVarFlags::None);
MCE_CVAR(cv_pitch, "cam.pitch", 0.62f, "Initial orbit pitch (radians, down from the horizon)", CVarFlags::None);
MCE_CVAR(cv_dist, "cam.dist", 17.0f, "Initial orbit distance (meters)", CVarFlags::None);
MCE_CVAR(cv_orbit, "cam.auto", true, "Slow automatic orbit", CVarFlags::None);

f64 g_start_s = 0.0;  // process clock at main() entry: first-frame time (NFR-M-02)

// ---- the kit -----------------------------------------------------------------------------------

enum Model : u32 {
    RoadStraight, RoadLights, RoadCorner, RoadSplit, RoadCross, Pavement, Fountain, Grass, Trees, TreesTall,
    BuildingA, BuildingB, BuildingC, BuildingD, Garage, Character, Coin, Cloud, Flag, kModelCount
};
constexpr const char* kModelPaths[kModelCount] = {
    "models/road-straight.glb", "models/road-straight-lightposts.glb", "models/road-corner.glb",
    "models/road-split.glb",    "models/road-intersection.glb",       "models/pavement.glb",
    "models/pavement-fountain.glb", "models/grass.glb",               "models/grass-trees.glb",
    "models/grass-trees-tall.glb", "models/building-small-a.glb",     "models/building-small-b.glb",
    "models/building-small-c.glb", "models/building-small-d.glb",     "models/building-garage.glb",
    "models/character.glb",     "models/coin.glb",                    "models/cloud.glb",
    "models/flag.glb",
};

// 13×13 one-meter tiles centred on the origin: a road every 4 tiles, nine 3×3 blocks between them.
constexpr i32 kGrid = 13;
constexpr f32 kHalfTurn = kPi;

struct Placement {
    Model model;
    vec3 position;
    f32 yaw;  // about +Y
    f32 scale = 1.0f;
};

u32 tile_hash(i32 i, i32 j) {
    u64 h = hash_combine(static_cast<u64>(i) * 0x9E37u, static_cast<u64>(j) + 0x51u);
    return static_cast<u32>(h >> 32);
}

vec3 tile_center(i32 i, i32 j) {
    constexpr i32 kHalf = kGrid / 2;  // the centre tile sits on the origin
    return {static_cast<f32>(i - kHalf), 0.0f, static_cast<f32>(j - kHalf)};
}

// The kit's road pieces run along Z at yaw 0; corner and T pieces open towards +X/+Z at yaw 0.
void build_city(Array<Placement>& out) {
    const i32 last = kGrid - 1;
    for (i32 j = 0; j < kGrid; ++j) {
        for (i32 i = 0; i < kGrid; ++i) {
            const bool road_i = i % 4 == 0, road_j = j % 4 == 0;  // a road column / row
            const vec3 p = tile_center(i, j);
            if (road_i && road_j) {
                const bool edge_i = i == 0 || i == last, edge_j = j == 0 || j == last;
                if (edge_i && edge_j) {
                    // Corner: rotate so it opens towards the grid interior.
                    const f32 yaw = i == 0 ? (j == 0 ? 0.0f : 0.5f * kHalfTurn) : (j == 0 ? -0.5f * kHalfTurn : kHalfTurn);
                    out.push_back({RoadCorner, p, yaw});
                } else if (edge_i || edge_j) {
                    f32 yaw = 0.0f;  // T: the stem points into the grid
                    if (i == 0) yaw = 0.0f;
                    else if (i == last) yaw = kHalfTurn;
                    else if (j == 0) yaw = -0.5f * kHalfTurn;
                    else yaw = 0.5f * kHalfTurn;
                    out.push_back({RoadSplit, p, yaw});
                } else {
                    out.push_back({RoadCross, p, 0.0f});
                }
            } else if (road_i) {
                out.push_back({(j % 2) ? RoadLights : RoadStraight, p, 0.0f});
            } else if (road_j) {
                out.push_back({(i % 2) ? RoadLights : RoadStraight, p, 0.5f * kHalfTurn});
            } else {
                const i32 bi = i / 4, bj = j / 4;           // block
                const i32 u = i % 4 - 1, v = j % 4 - 1;     // 0..2 inside the block
                const u32 h = tile_hash(i, j);
                if (bi == 1 && bj == 1) {  // the plaza
                    out.push_back({u == 1 && v == 1 ? Fountain : Pavement, p, 0.0f});
                    continue;
                }
                if (u == 1 && v == 1) {
                    const Model park[3] = {Trees, TreesTall, Grass};
                    out.push_back({park[h % 3], p, static_cast<f32>(h % 4) * 0.5f * kHalfTurn});
                    continue;
                }
                // Facing the nearest road: v == 0 -> -Z, v == 2 -> +Z, u == 0 -> -X, u == 2 -> +X.
                f32 yaw = 0.0f;
                if (v == 2) yaw = 0.0f;
                else if (v == 0) yaw = kHalfTurn;
                else if (u == 2) yaw = 0.5f * kHalfTurn;
                else yaw = -0.5f * kHalfTurn;
                if ((u == 0 || u == 2) && (v == 0 || v == 2) && (h & 1)) yaw = u == 2 ? 0.5f * kHalfTurn : -0.5f * kHalfTurn;
                const Model houses[6] = {BuildingA, BuildingB, BuildingC, BuildingD, Garage, TreesTall};
                out.push_back({houses[(h >> 3) % 6], p, yaw});
            }
        }
    }
    // A two-tile park border so the block does not end in the void.
    for (i32 j = -2; j < kGrid + 2; ++j)
        for (i32 i = -2; i < kGrid + 2; ++i) {
            if (i >= 0 && i < kGrid && j >= 0 && j < kGrid) continue;
            const u32 h = tile_hash(i + 100, j + 100);
            out.push_back({h % 7 == 0 ? Trees : (h % 11 == 0 ? TreesTall : Grass), tile_center(i, j),
                           static_cast<f32>(h % 4) * 0.5f * kHalfTurn});
        }
    out.push_back({Flag, vec3{1.25f, 0.06f, -1.25f}, 0.3f, 0.6f});
}

// ---- the app ----------------------------------------------------------------------------------

f64 now_ms() { return platform::now_seconds() * 1000.0; }

class DioramaApp final : public app::Application, public app::IPlatformHooks {
public:
    // window/device null = headless (no GL anywhere: the CPU half of the mesh store).
    DioramaApp(platform::Window* window, rhi::RenderDevice* device)
        : window_(window), device_(device), sink_(renderer_, assets_) {}

    ResultV on_init(app::Context&) override {
        const f64 t0 = now_ms();
        if (device_) {
            MCE_TRY(renderer_.init(*device_));
            Forward3DDesc fd;
            fd.msaa = static_cast<u32>(math::clamp<i64>(cv_msaa.get_int(), 1, 8));
            MCE_TRY(renderer_.init_3d(fd));
            assets_.set_upload_sink(&sink_);
            meshes_ = &renderer_.meshes();
        } else {
            cpu_meshes_.init(nullptr);
            meshes_ = &cpu_meshes_;
        }
        MCE_TRY(mount_pack());

        // Textures first: a mesh upload resolves its materials' image GUIDs to resident textures.
        for (asset::AssetKey k = 0; k < assets_.asset_count(); ++k) {
            if (assets_.info(k).type != asset::ChunkType::Texture) continue;
            MCE_TRY_ASSIGN(asset::AssetHandle<asset::Texture> h, assets_.load_sync<asset::Texture>(k));
            handles_.push_back(asset::AnyHandle(h));
        }
        for (u32 m = 0; m < kModelCount; ++m) {
            const asset::AssetKey k = find_path(kModelPaths[m]);
            if (k == ~0u) return Error(Errc::NotFound, "04_diorama: model missing from the pack", kModelPaths[m]);
            MCE_TRY_ASSIGN(asset::AssetHandle<asset::Mesh> h, assets_.load_sync<asset::Mesh>(k));
            handles_.push_back(asset::AnyHandle(h));
            if (!device_) MCE_TRY(cpu_meshes_.upload(k, assets_.generation(k), assets_.resolve(h)));
            keys_[m] = k;
        }
        const GpuMesh* character = meshes_->get(keys_[Character]);
        if (!character) return Error(Errc::NotFound, "04_diorama: character mesh not resident");
        walk_clip_ = character->find_clip("walk");
        char_pose_.resize(character->nodes.size());

        Array<Placement> city;
        build_city(city);
        for (const Placement& p : city) {
            MeshInstance inst;
            inst.mesh = keys_[p.model];
            inst.world = trs(p.position, quat_from_axis_angle({0, 1, 0}, p.yaw), vec3(p.scale, p.scale, p.scale));
            statics_.push_back(inst);
        }
        for (u32 c = 0; c < 4; ++c) {
            CloudState cl;
            cl.position = vec3(-8.0f + 4.6f * static_cast<f32>(c), 4.6f + 0.4f * static_cast<f32>(c % 3),
                               -5.5f + 3.4f * static_cast<f32>((c * 3) % 4));
            cl.scale = 0.7f + 0.15f * static_cast<f32>(c % 3);
            clouds_.push_back(cl);
        }

        yaw_ = cv_yaw.get_float();
        pitch_ = cv_pitch.get_float();
        distance_ = cv_dist.get_float();
        auto_orbit_ = cv_orbit.get_bool();
        if (device_) {
            HudFont hud;
            MCE_TRY_ASSIGN(font_, renderer_.create_font(hud.metrics, hud.glyphs.span(), {}, std::as_bytes(hud.atlas.span()),
                                                        "04_diorama.hud_font"));
            window_->set_event_handler(&DioramaApp::on_event, this);
            apply_window_icon(*window_, assets_, "icon.png");
        }
        load_ms_ = static_cast<f32>(now_ms() - t0);
        MCE_LOG_INFO("04_diorama: {} assets loaded in {} ms", assets_.asset_count(), load_ms_);
        return ok();
    }

    void on_tick(const app::TickContext& t) override {
        const f32 dt = static_cast<f32>(t.dt);
        prev_time_ = time_;
        time_ += dt;
        prev_yaw_ = yaw_;
        if (auto_orbit_) yaw_ += 0.08f * dt;
        yaw_ += static_cast<f32>(key_yaw_) * 1.2f * dt;
        pitch_ = clamp(pitch_ + static_cast<f32>(key_pitch_) * 0.9f * dt, 0.12f, 1.45f);
        for (CloudState& c : clouds_) {
            c.prev = c.position;
            c.position.x += 0.3f * dt;
            if (c.position.x > 10.0f) c.position.x -= 20.0f;
            if (c.prev.x > c.position.x) c.prev = c.position;  // no interpolation across the wrap
        }
        if (!device_) {
            // Headless: the full CPU frame (animation, culling, sort, shadow fit) feeds the hash.
            Scene3D scene(*meshes_, mem::heap());
            fill_scene(scene, 1.0f, {1280, 720});
            fold(scene);
        }
        ++ticks_;
    }

    void on_render(const app::RenderContext& rc) override {
        if (!device_) return;  // headless: the tick already ran the CPU frame
        if (rc.frame_index == 1) {  // frame 0 is on screen: boot -> first frame (NFR-M-02 < 1 s)
            first_frame_ms_ = static_cast<f32>(platform::now_seconds() * 1000.0 - g_start_s * 1000.0);
            std::printf("04_diorama: first frame %.0f ms (asset load %.1f ms)\n", static_cast<f64>(first_frame_ms_),
                        static_cast<f64>(load_ms_));
        }
        device_->begin_frame(rc.frame_index);
        const rhi::Extent2D bb = device_->backbuffer_size();
        const f64 t0 = now_ms();
        Scene3D scene(*meshes_, mem::frame());
        fill_scene(scene, static_cast<f32>(rc.alpha), bb);
        const f64 t1 = now_ms();

        rhi::CommandBuffer cmd(mem::frame());
        renderer_.record_3d(cmd, scene);
        record_hud(cmd, scene, bb);
        const f64 t2 = now_ms();
        device_->submit(cmd);
        const f64 t3 = now_ms();
        build_ms_ = static_cast<f32>(t1 - t0);
        if (rc.frame_index >= 10) {  // skip warm-up
            timing_.build += t1 - t0;
            timing_.record += t2 - t1;
            timing_.submit += t3 - t2;
            ++timing_.frames;
        }

        const bool last = cv_frames.get_int() > 0 && rc.frame_index + 1 >= static_cast<u64>(cv_frames.get_int());
        if (last && !cv_capture.get_string().empty()) capture(cv_capture.get_string().data(), bb);
        device_->end_frame();
        assets_.end_frame();
        last_stats_ = scene.stats();
        if (last) quit(0);
    }

    void on_shutdown() override {
        for (asset::AnyHandle h : handles_) assets_.release(h);
        handles_.clear();
        if (device_) renderer_.shutdown();
    }

    bool pump_events() override { return platform::pump_events(); }
    void present(bool vsync) override { device_->present(vsync); }

    // Per-stage CPU means over the run (NFR-F-01 table shape); empty when nothing rendered.
    void report_timing() const {
        if (timing_.frames == 0) return;
        const f64 n = static_cast<f64>(timing_.frames);
        std::printf("04_diorama: avg CPU ms over %llu frames: scene build (anim+cull+sort+shadow fit) %.3f, "
                    "record %.3f, GL translate+submit %.3f\n",
                    static_cast<unsigned long long>(timing_.frames), timing_.build / n, timing_.record / n, timing_.submit / n);
    }

    u64 state_hash() const { return hash_; }
    u64 ticks() const { return ticks_; }
    const Scene3D::Stats& last_stats() const { return last_stats_; }

private:
    struct CloudState {
        vec3 position, prev;
        f32 scale = 1.0f;
    };

    ResultV mount_pack() {
        FixedString<1024> path;
        resolve_shipped_path(cv_pack.get_string(), "assets.mce", MCE_SAMPLE_DEV_PACK, path);
        const StringView roots[1] = {path.view()};
        asset::MountOptions mo;
        mo.verify_payload_crc = false;  // a pack verifies once at mount (assets.md §3)
        MCE_TRY(assets_.mount(Span<const StringView>(roots, 1), mo));
        MCE_LOG_INFO("04_diorama: mounted {}", path.view());
        return ok();
    }

    asset::AssetKey find_path(StringView path) const {
        for (asset::AssetKey k = 0; k < assets_.asset_count(); ++k)
            if (assets_.info(k).path == path) return k;
        return ~0u;
    }

    Camera3D camera(f32 alpha) const {
        const f32 yaw = math::lerp(prev_yaw_, yaw_, alpha);
        return orbit_camera({0.0f, 0.3f, 0.0f}, yaw, pitch_, distance_, radians(40.0f));
    }

    // Everything animated is a pure function of the interpolated time.
    void fill_scene(Scene3D& scene, f32 alpha, rhi::Extent2D viewport) {
        const f32 t = math::lerp(prev_time_, time_, alpha);
        scene.begin(camera(alpha), viewport);
        DirectionalLight sun;
        sun.direction = normalize(vec3{-0.6f, -0.85f, -0.4f});
        sun.color = {1.0f, 0.96f, 0.88f};
        scene.set_light(sun);
        Fog fog;
        fog.color = {0.62f, 0.74f, 0.88f};
        fog.density = fog_ ? 0.028f : 0.0f;
        scene.set_fog(fog);
        for (const MeshInstance& i : statics_) scene.add(i);

        // The walker circles the fountain on the walk clip, facing along its path.
        const GpuMesh& ch = *meshes_->get(keys_[Character]);
        pose_nodes(ch, walk_clip_, t, char_pose_.span());
        const f32 a = t * 0.45f;
        MeshInstance walker;
        walker.mesh = keys_[Character];
        walker.world = trs(vec3{std::cos(a) * 1.05f, 0.06f, -std::sin(a) * 1.05f},
                           quat_from_axis_angle({0, 1, 0}, a + kHalfTurn), vec3(0.5f, 0.5f, 0.5f));
        walker.pose = char_pose_.span();
        scene.add(walker);

        // The spinning prop: a coin above the fountain.
        MeshInstance coin;
        coin.mesh = keys_[Coin];
        coin.world = trs(vec3{0.0f, 0.95f + 0.08f * std::sin(t * 2.0f), 0.0f}, quat_from_axis_angle({0, 1, 0}, t * 2.2f),
                         vec3(0.9f, 0.9f, 0.9f));
        scene.add(coin);

        for (const CloudState& c : clouds_) {
            MeshInstance cl;
            cl.mesh = keys_[Cloud];
            cl.world = trs(lerp(c.prev, c.position, alpha), quat::identity(), vec3(c.scale, c.scale * 0.6f, c.scale));
            cl.tint = {1.0f, 1.0f, 1.0f, 1.0f};
            scene.add(cl);
        }
        scene.build();
    }

    void fold(const Scene3D& scene) {
        u64 h = hash_;
        for (const Scene3D::Draw& d : scene.draws()) {
            h = hash_combine(h, (static_cast<u64>(d.instance) << 32) | d.submesh);
            h = fnv1a64(&d.model, sizeof(d.model), h);
        }
        h = hash_combine(h, scene.shadow_draws().size());
        h = fnv1a64(char_pose_.data(), char_pose_.size() * sizeof(mat4), h);
        const mat4 lvp = scene.light_view_proj();
        hash_ = fnv1a64(&lvp, sizeof(lvp), h);
        last_stats_ = scene.stats();
    }

    void record_hud(rhi::CommandBuffer& cmd, const Scene3D& scene, rhi::Extent2D bb) {
        const Scene3D::Stats& st = scene.stats();
        const Forward3DStats& fs = renderer_.forward_3d().last_stats();
        const rhi::FrameStats& gpu = device_->last_frame_stats();
        char line[320];
        std::snprintf(line, sizeof(line),
                      "MCE 3D DIORAMA\nTRIS %u  DRAWS %u  SHADOW DRAWS %u  CULLED %u\nGPU DRAWS %u  SCENE BUILD %.2f MS  "
                      "FIRST FRAME %.0f MS\nDRAG OR ARROWS ORBIT  WHEEL OR Z X ZOOM  SPACE AUTO  F FOG %s",
                      st.triangles, fs.main_draws, fs.shadow_draws, st.submeshes - st.visible, gpu.draws,
                      static_cast<f64>(build_ms_), static_cast<f64>(first_frame_ms_), fog_ ? "ON" : "OFF");
        TextLayout layout(mem::frame());
        layout_text(font_.face, line, {}, layout);
        SpriteBatch hud(renderer_.sprite_sets(), mem::frame());
        hud.begin(compute_screen_view(bb), PassId::Screen2D);
        TextPlacement p;  // a drop shadow, then the text: readable over sky and rooftops alike
        p.origin = vec2(14, 14);
        p.tint = Color{0.02f, 0.03f, 0.06f, 0.85f};
        hud.add_text(font_, layout, p);
        p.origin = vec2(12, 12);
        p.tint = Color{1.0f, 0.97f, 0.88f, 1};
        hud.add_text(font_, layout, p);
        hud.build();

        rhi::PassAttachment color;  // backbuffer, keeping the tonemapped frame
        color.load = rhi::LoadOp::Load;
        rhi::RenderPassDesc pass;
        pass.colors = Span<const rhi::PassAttachment>(&color, 1);
        pass.debug_name = "hud";
        cmd.begin_pass(pass);
        renderer_.record_2d(cmd, hud);
        cmd.end_pass();
    }

    void capture(const char* path, rhi::Extent2D size) {
        Array<u8> px;
        px.resize(usize(size.width) * size.height * 4);
        if (!device_->read_pixels({}, rhi::Rect2D{0, 0, size.width, size.height}, px.span())) return;
        std::FILE* f = std::fopen(path, "wb");
        if (!f) {
            MCE_LOG_ERROR("04_diorama: cannot write '{}'", path);
            return;
        }
        std::fprintf(f, "P6\n%u %u\n255\n", size.width, size.height);
        for (usize i = 0; i < px.size(); i += 4) std::fwrite(&px[i], 1, 3, f);
        std::fclose(f);
        MCE_LOG_INFO("04_diorama: wrote {}", path);
    }

    static void on_event(const platform::Event& e, void* user) {
        auto* self = static_cast<DioramaApp*>(user);
        using platform::EventType;
        using platform::Key;
        switch (e.type) {
            case EventType::KeyDown:
            case EventType::KeyUp: {
                const bool down = e.type == EventType::KeyDown;
                const i32 v = down ? 1 : 0;
                switch (static_cast<Key>(e.code)) {
                    case Key::Escape: if (down) self->quit(0); break;
                    case Key::Space: if (down) self->auto_orbit_ = !self->auto_orbit_; break;
                    case Key::F: if (down) self->fog_ = !self->fog_; break;
                    case Key::Left: self->key_yaw_ = -v; break;
                    case Key::Right: self->key_yaw_ = v; break;
                    case Key::Up: self->key_pitch_ = v; break;
                    case Key::Down: self->key_pitch_ = -v; break;
                    case Key::Z: if (down) self->zoom(1.0f); break;
                    case Key::X: if (down) self->zoom(-1.0f); break;
                    default: break;
                }
                break;
            }
            case EventType::MouseButtonDown:
                if (e.code == static_cast<u32>(platform::MouseButton::Left)) {
                    self->dragging_ = true;
                    self->mouse_ = vec2(e.x, e.y);
                }
                break;
            case EventType::MouseButtonUp:
                if (e.code == static_cast<u32>(platform::MouseButton::Left)) self->dragging_ = false;
                break;
            case EventType::MouseMove:
                if (self->dragging_) {
                    const vec2 m(e.x, e.y);
                    self->yaw_ -= (m.x - self->mouse_.x) * 0.006f;
                    self->prev_yaw_ = self->yaw_;
                    self->pitch_ = clamp(self->pitch_ + (m.y - self->mouse_.y) * 0.004f, 0.12f, 1.45f);
                    self->mouse_ = m;
                }
                break;
            case EventType::MouseWheel: self->zoom(e.y); break;
            default: break;
        }
    }

    void zoom(f32 steps) { distance_ = clamp(distance_ * std::pow(0.9f, steps), 5.0f, 32.0f); }

    platform::Window* window_;
    rhi::RenderDevice* device_;
    Renderer renderer_;
    asset::AssetSystem assets_;
    AssetUploadSink sink_;
    MeshStore cpu_meshes_;
    MeshStore* meshes_ = nullptr;
    Array<asset::AnyHandle> handles_;
    u32 keys_[kModelCount] = {};
    u32 walk_clip_ = 0;
    Array<mat4> char_pose_;
    Array<MeshInstance> statics_;
    Array<CloudState> clouds_;
    Font font_;

    f32 time_ = 0.0f, prev_time_ = 0.0f;
    f32 yaw_ = 0.0f, prev_yaw_ = 0.0f, pitch_ = 0.6f, distance_ = 17.0f;
    bool auto_orbit_ = true, fog_ = true, dragging_ = false;
    i32 key_yaw_ = 0, key_pitch_ = 0;
    vec2 mouse_;
    f32 load_ms_ = 0.0f, build_ms_ = 0.0f, first_frame_ms_ = 0.0f;
    Scene3D::Stats last_stats_;
    struct {
        f64 build = 0, record = 0, submit = 0;
        u64 frames = 0;
    } timing_;
    u64 hash_ = kFnv64Offset;
    u64 ticks_ = 0;
};

int fail(const char* what, const Error& e) {
    FixedString<512> msg;
    format_error(e, msg);
    FixedString<640> text;
    text.append(what);
    text.append(": ");
    text.append(msg.view());
    platform::show_error_box("04_diorama", text.c_str());
    return 1;
}

int run_headless() {
    DioramaApp app(nullptr, nullptr);
    app::AppDesc desc;
    desc.name = "04_diorama";
    desc.headless = true;
    desc.max_ticks = static_cast<u64>(cv_ticks.get_int() > 0 ? cv_ticks.get_int() : 300);
    const int rc = app::run(app, desc);
    const Scene3D::Stats& st = app.last_stats();
    std::printf("04_diorama: %llu ticks | %u instances, %u visible submeshes, %u tris | hash %016llx\n",
                static_cast<unsigned long long>(app.ticks()), st.instances, st.visible, st.triangles,
                static_cast<unsigned long long>(app.state_hash()));
    return rc;
}

int run_windowed() {
    platform::WindowDesc wd;
    wd.title = "MCE - 04_diorama";
    wd.width = 1280;
    wd.height = 720;
    auto window = platform::Window::create(wd);
    if (!window) return fail("window creation failed", window.error());
    rhi::DeviceDesc dd;
    dd.debug_layer = cv_gl_debug.get_bool();
    auto device = rhi::create_default_device(*window.value(), dd);
    if (!device) return fail("render device creation failed", device.error());

    DioramaApp app(window.value().get(), device.value().get());
    app::AppDesc desc;
    desc.name = "04_diorama";
    desc.platform = &app;
    desc.fences = device.value().get();
    app::RunStats stats;
    int rc = app::run(app, desc, &stats);
    app.report_timing();
    const u32 gl_errors = device.value()->validation_error_count();
    const Scene3D::Stats& st = app.last_stats();
    std::printf("04_diorama: %llu frames, %u visible tris, %u GL errors\n", static_cast<unsigned long long>(stats.frames),
                st.triangles, gl_errors);
    if (rc == 0 && gl_errors != 0) rc = 2;
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    g_start_s = platform::now_seconds();
    log::LogDesc log_desc;
    log_desc.app_name = "04_diorama";
    log::init(log_desc);
    console::parse_command_line(argc, argv);

    const ConVar* headless = ConVar::find(StringId("headless"));
    const bool is_headless = headless && headless->get_bool();
    int rc = 1;
    if (is_headless) {
        rc = run_headless();
    } else if (ResultV r = platform::init(); !r) {
        rc = fail("platform init failed", r.error());
    } else {
        rc = run_windowed();
        platform::shutdown();
    }
    log::shutdown();
    return rc;
}
