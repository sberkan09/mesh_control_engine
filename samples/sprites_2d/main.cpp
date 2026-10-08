// sprites_2d — the M3 2D render path end to end: 10k sprites (FR-R2D-010 budget) bouncing
// around a tilemap arena under a pixel-snapped Camera2D with screen shake, plus HUD text through
// the same batch. Art is generated at startup (the cook pipeline is not wired into samples yet):
// a 4-layer array atlas and a 5×7 bitmap font baked into an R8 coverage atlas.
//
//   sprites_2d                               run until the window closes (Esc quits, Space shakes)
//   sprites_2d -sprites 10000 -frames 300    CI smoke / budget run, prints per-stage timings
//   sprites_2d -frames 60 -capture out.ppm   also write the last frame as a PPM image

#include <cstdio>

#include "mce/core.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/render/renderer.h"
#include "mce/rhi_gl/gl_device.h"

namespace {

using namespace mce;
using namespace mce::math;
using namespace mce::render;

MCE_CVAR(cv_frames, "frames", 0, "Quit after N frames (0 = run until the window closes)", CVarFlags::None);
MCE_CVAR(cv_capture, "capture", "", "Write the last frame (with -frames) to this .ppm path", CVarFlags::None);
MCE_CVAR(cv_sprites, "sprites", 10000, "Number of bouncing sprites", CVarFlags::None);
MCE_CVAR(cv_gl_debug, "r.gl_debug", true, "Enable the GL_KHR_debug validation callback", CVarFlags::RequiresRestart);

constexpr f32 kPpm = 16.0f;        // art texels per meter
constexpr u32 kTile = 16;          // atlas layer size in texels
constexpr u32 kArenaW = 60, kArenaH = 34;  // tiles (1 m each)

// ---- generated art ---------------------------------------------------------------------------

// Layer 0 circle, 1 diamond, 2 ring, 3 brick tile. White art; color comes from the tint.
Array<u8> make_atlas() {
    Array<u8> px;
    px.resize(usize(kTile) * kTile * 4 * 4);
    for (u32 layer = 0; layer < 4; ++layer) {
        for (u32 y = 0; y < kTile; ++y) {
            for (u32 x = 0; x < kTile; ++x) {
                const f32 dx = static_cast<f32>(x) + 0.5f - 8.0f, dy = static_cast<f32>(y) + 0.5f - 8.0f;
                const f32 r = std::sqrt(dx * dx + dy * dy);
                bool on = false;
                u8 shade = 255;
                switch (layer) {
                    case 0: on = r < 7.0f; break;
                    case 1: on = std::fabs(dx) + std::fabs(dy) < 7.5f; break;
                    case 2: on = r < 7.5f && r > 4.0f; break;
                    default:  // 3: brick
                        on = true;
                        shade = (y % 8 == 0 || (x + (y / 8) * 8) % 16 == 0) ? 90 : 200;
                        break;
                }
                u8* p = px.data() + ((usize(layer) * kTile + y) * kTile + x) * 4;
                p[0] = p[1] = p[2] = shade;
                p[3] = on ? 255 : 0;
            }
        }
    }
    return px;
}

// A 5×7 bitmap font for the HUD: rows top to bottom, bit 4 = leftmost column.
struct BitmapGlyph {
    char c;
    u8 rows[7];
};
constexpr BitmapGlyph kBitmapFont[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}}, {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
};

// Bakes the bitmap font at 2× (10×14 px glyphs) into an R8 atlas with the cooked-font tables.
struct HudFont {
    FontMetrics metrics;
    Array<FontGlyph> glyphs;
    Array<u8> atlas;

    HudFont() {
        constexpr u32 kScale = 2, kCell = 16, kCols = 16;
        metrics.pixel_height = 16;
        metrics.ascent = 14;
        metrics.descent = -2;
        metrics.line_gap = 4;
        metrics.atlas_width = kCell * kCols;
        metrics.atlas_height = kCell * 4;
        atlas.resize(usize(metrics.atlas_width) * metrics.atlas_height);
        Array<BitmapGlyph> sorted;
        sorted.push_back(BitmapGlyph{' ', {}});
        for (const BitmapGlyph& g : kBitmapFont) sorted.push_back(g);
        // Glyph tables are sorted by codepoint.
        for (usize i = 1; i < sorted.size(); ++i)
            for (usize j = i; j > 0 && sorted[j - 1].c > sorted[j].c; --j) {
                const BitmapGlyph t = sorted[j];
                sorted[j] = sorted[j - 1];
                sorted[j - 1] = t;
            }
        for (usize i = 0; i < sorted.size(); ++i) {
            const u32 cx = static_cast<u32>(i % kCols) * kCell, cy = static_cast<u32>(i / kCols) * kCell;
            for (u32 row = 0; row < 7; ++row)
                for (u32 col = 0; col < 5; ++col) {
                    if (!(sorted[i].rows[row] & (0x10u >> col))) continue;
                    for (u32 sy = 0; sy < kScale; ++sy)
                        for (u32 sx = 0; sx < kScale; ++sx)
                            atlas[usize(cy + row * kScale + sy) * metrics.atlas_width + cx + col * kScale + sx] = 255;
                }
            FontGlyph g{};
            g.codepoint = static_cast<u8>(sorted[i].c);
            g.x0 = static_cast<u16>(cx);
            g.y0 = static_cast<u16>(cy);
            g.x1 = static_cast<u16>(sorted[i].c == ' ' ? cx : cx + 5 * kScale);
            g.y1 = static_cast<u16>(sorted[i].c == ' ' ? cy : cy + 7 * kScale);
            g.yoff = -14;
            g.xoff2 = 5 * kScale;
            g.xadvance = 6 * kScale;
            glyphs.push_back(g);
        }
    }
};

// ---- the app ---------------------------------------------------------------------------------

struct Critter {
    vec2 pos, prev, vel;
    f32 spin;
    f32 angle, prev_angle;
    u32 region;
    Color tint;
    i32 order;
};

f64 now_ms() { return platform::now_seconds() * 1000.0; }

class SpritesApp final : public app::Application, public app::IPlatformHooks {
public:
    SpritesApp(platform::Window& window, rhi::RenderDevice& device) : window_(window), device_(device) {}

    ResultV on_init(app::Context&) override {
        MCE_TRY(renderer_.init(device_));

        const Array<u8> atlas = make_atlas();
        const TextureMipUpload mip{kTile, kTile, std::as_bytes(atlas.span())};
        TextureUpload up;
        up.format = rhi::TextureFormat::RGBA8_SRGB;
        up.layers = 4;
        up.mips = Span<const TextureMipUpload>(&mip, 1);
        up.debug_name = "sprites_2d.atlas";
        const u32 atlas_key = renderer_.textures().allocate_key();
        MCE_TRY(renderer_.textures().upload(atlas_key, 1, up));
        SpriteRegion regions[4];
        for (u16 l = 0; l < 4; ++l) regions[l] = SpriteRegion{l, 0, 0, 1, 1, 1, 1};
        SpriteSetDesc sd;
        sd.texture_key = atlas_key;
        sd.regions = regions;
        sd.pixels_per_meter = kPpm;
        set_ = renderer_.create_sprite_set(sd);

        HudFont hud;
        MCE_TRY_ASSIGN(font_, renderer_.create_font(hud.metrics, hud.glyphs.span(), {}, std::as_bytes(hud.atlas.span()),
                                                    "sprites_2d.hud_font"));

        // Arena: brick border plus a few platforms.
        Array<u32> tiles;
        tiles.resize(usize(kArenaW) * kArenaH);
        for (u32 y = 0; y < kArenaH; ++y)
            for (u32 x = 0; x < kArenaW; ++x) {
                const bool border = x == 0 || y == 0 || x == kArenaW - 1 || y == kArenaH - 1;
                const bool platform = (y == 8 && x > 10 && x < 22) || (y == 16 && x > 34 && x < 50) || (y == 24 && x > 6 && x < 18);
                tiles[usize(y) * kArenaW + x] = border || platform ? 4 : 0;  // region 3 = brick
            }
        TilemapDesc td;
        td.set = set_;
        td.width = kArenaW;
        td.height = kArenaH;
        td.origin = vec2(-0.5f * kArenaW, -0.5f * kArenaH);
        td.tiles = tiles.span();
        td.tint = Color{0.55f, 0.35f, 0.25f, 1};
        td.sort_layer = -1;
        MCE_TRY_ASSIGN(tilemap_, renderer_.create_tilemap(td));

        u64 s = 0x2D2D;
        auto rnd = [&s] {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<f32>((s >> 40) & 0xFFFFFF) / static_cast<f32>(0xFFFFFF);
        };
        const i64 n = cv_sprites.get_int() > 0 ? cv_sprites.get_int() : 0;
        critters_.reserve(static_cast<usize>(n));
        for (i64 i = 0; i < n; ++i) {
            Critter c;
            c.pos = c.prev = vec2((rnd() - 0.5f) * (kArenaW - 4), (rnd() - 0.5f) * (kArenaH - 4));
            c.vel = vec2(rnd() - 0.5f, rnd() - 0.5f) * 12.0f;
            c.spin = (rnd() - 0.5f) * 6.0f;
            c.angle = c.prev_angle = 0;
            c.region = static_cast<u32>(rnd() * 2.99f);
            c.tint = Color{0.3f + 0.7f * rnd(), 0.3f + 0.7f * rnd(), 0.3f + 0.7f * rnd(), 0.85f};
            c.order = static_cast<i32>(i);
            critters_.push_back(c);
        }
        window_.set_event_handler(&SpritesApp::on_event, this);
        return ok();
    }

    void on_tick(const app::TickContext& t) override {
        const f32 dt = static_cast<f32>(t.dt);
        const vec2 half(0.5f * kArenaW - 1.5f, 0.5f * kArenaH - 1.5f);
        for (Critter& c : critters_) {
            c.prev = c.pos;
            c.prev_angle = c.angle;
            c.pos = c.pos + c.vel * dt;
            c.angle += c.spin * dt;
            if (c.pos.x < -half.x || c.pos.x > half.x) c.vel.x = -c.vel.x;
            if (c.pos.y < -half.y || c.pos.y > half.y) c.vel.y = -c.vel.y;
        }
        // Slow camera drift + shake on the fixed tick (deterministic).
        time_ += dt;
        camera_.position = vec2(std::sin(time_ * 0.3f) * 3.0f, std::cos(time_ * 0.2f) * 1.5f);
        if (t.tick_index % 240 == 0) shake_.add_trauma(0.6f);
        shake_.tick(dt);
    }

    void on_render(const app::RenderContext& rc) override {
        device_.begin_frame(rc.frame_index);
        const rhi::Extent2D bb = device_.backbuffer_size();
        camera_.zoom = static_cast<f32>(bb.height) / (static_cast<f32>(kArenaH) * kPpm);  // fit the arena height

        const f64 t0 = now_ms();
        SpriteBatch world(renderer_.sprite_sets(), mem::frame());
        world.begin(compute_view_2d(camera_, bb, kPpm, shake_.offset()));
        world.reserve(critters_.size());
        renderer_.add_tilemap(world, tilemap_);
        for (const Critter& c : critters_) {
            SpriteInstance s;
            s.set = set_;
            s.region = c.region;
            s.position = lerp(c.prev, c.pos, rc.alpha);
            s.rotation = math::lerp(c.prev_angle, c.angle, rc.alpha);
            s.scale = vec2(0.75f, 0.75f);
            s.tint = c.tint;
            s.sort_order = c.order;
            world.add(s);
        }
        world.build();
        const f64 t1 = now_ms();

        // HUD in pixel space, last frame's numbers.
        char line[160];
        std::snprintf(line, sizeof(line), "SPRITES %u  CULLED %u  MDI %u  BUILD %.2f MS\nTILE QUADS %u  GPU DRAWS %u",
                      world.stats().quads, world.stats().culled, world.stats().groups, build_ms_, world.stats().tile_quads,
                      device_.last_frame_stats().draws);
        TextLayout layout(mem::frame());
        layout_text(font_.face, line, {}, layout);
        SpriteBatch hud(renderer_.sprite_sets(), mem::frame());
        hud.begin(compute_screen_view(bb), PassId::Screen2D);
        TextPlacement p;
        p.origin = vec2(12, 12);
        p.tint = Color{1.0f, 0.9f, 0.4f, 1};
        hud.add_text(font_, layout, p);
        hud.build();

        rhi::PassAttachment color;  // null texture = backbuffer
        color.clear.color[0] = 0.05f;
        color.clear.color[1] = 0.06f;
        color.clear.color[2] = 0.09f;
        rhi::RenderPassDesc pass;
        pass.colors = Span<const rhi::PassAttachment>(&color, 1);
        pass.debug_name = "main2d";
        rhi::CommandBuffer cmd(mem::frame());
        cmd.begin_pass(pass);
        renderer_.record_2d(cmd, world);
        renderer_.record_2d(cmd, hud);
        cmd.end_pass();
        const f64 t2 = now_ms();
        device_.submit(cmd);
        const f64 t3 = now_ms();

        build_ms_ = static_cast<f32>(t1 - t0);
        accumulate(t1 - t0, t2 - t1, t3 - t2, world.stats());

        const bool last = cv_frames.get_int() > 0 && rc.frame_index + 1 >= static_cast<u64>(cv_frames.get_int());
        if (last && !cv_capture.get_string().empty()) capture(cv_capture.get_string().data(), bb);
        device_.end_frame();
        if (last) quit(0);
    }

    void on_shutdown() override {
        report();
        renderer_.shutdown();
    }

    bool pump_events() override { return platform::pump_events(); }
    void present(bool vsync) override { device_.present(vsync); }

private:
    struct Timing {
        f64 build = 0, record = 0, submit = 0;
        u64 frames = 0;
        u32 quads = 0, groups = 0, commands = 0;
    };

    void accumulate(f64 build, f64 record, f64 submit, const BatchStats& st) {
        if (frames_seen_++ < 10) return;  // skip warm-up
        timing_.build += build;
        timing_.record += record;
        timing_.submit += submit;
        ++timing_.frames;
        timing_.quads = st.quads;
        timing_.groups = st.groups;
        timing_.commands = st.commands;
    }

    void report() const {
        if (timing_.frames == 0) return;
        const f64 n = static_cast<f64>(timing_.frames);
        std::printf("sprites_2d: %u quads in %u MDI groups (%u commands); avg CPU ms: batch build %.3f, record %.3f, "
                    "translate+submit %.3f over %llu frames\n",
                    timing_.quads, timing_.groups, timing_.commands, timing_.build / n, timing_.record / n,
                    timing_.submit / n, static_cast<unsigned long long>(timing_.frames));
    }

    void capture(const char* path, rhi::Extent2D size) {
        Array<u8> px;
        px.resize(usize(size.width) * size.height * 4);
        if (!device_.read_pixels({}, rhi::Rect2D{0, 0, size.width, size.height}, px.span())) return;
        std::FILE* f = std::fopen(path, "wb");
        if (!f) {
            MCE_LOG_ERROR("sprites_2d: cannot write '{}'", path);
            return;
        }
        std::fprintf(f, "P6\n%u %u\n255\n", size.width, size.height);
        for (usize i = 0; i < px.size(); i += 4) std::fwrite(&px[i], 1, 3, f);
        std::fclose(f);
        MCE_LOG_INFO("sprites_2d: wrote {}", path);
    }

    static void on_event(const platform::Event& e, void* user) {
        auto* self = static_cast<SpritesApp*>(user);
        if (e.type != platform::EventType::KeyDown) return;
        if (e.code == static_cast<u32>(platform::Key::Escape)) self->quit(0);
        if (e.code == static_cast<u32>(platform::Key::Space)) self->shake_.add_trauma(0.5f);
    }

    platform::Window& window_;
    rhi::RenderDevice& device_;
    Renderer renderer_;
    SpriteSetId set_;
    Font font_;
    TilemapId tilemap_;
    Array<Critter> critters_;
    Camera2D camera_;
    ScreenShake shake_;
    f32 time_ = 0.0f;
    f32 build_ms_ = 0.0f;
    u64 frames_seen_ = 0;
    Timing timing_;
};

int fail(const char* what, const Error& e) {
    FixedString<512> msg;
    format_error(e, msg);
    FixedString<640> text;
    text.append(what);
    text.append(": ");
    text.append(msg.view());
    platform::show_error_box("sprites_2d", text.c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    log::LogDesc log_desc;
    log_desc.app_name = "sprites_2d";
    log::init(log_desc);
    console::parse_command_line(argc, argv);

    int rc = 1;
    if (ResultV r = platform::init(); !r) {
        rc = fail("platform init failed", r.error());
    } else {
        platform::WindowDesc wd;
        wd.title = "MCE - sprites_2d";
        wd.width = 1280;
        wd.height = 720;
        auto window = platform::Window::create(wd);
        if (!window) {
            rc = fail("window creation failed", window.error());
        } else {
            rhi::DeviceDesc dd;
            dd.debug_layer = cv_gl_debug.get_bool();
            auto device = rhi::gl::create_device(*window.value(), dd);
            if (!device) {
                rc = fail("OpenGL 4.5 device creation failed", device.error());
            } else {
                SpritesApp app(*window.value(), *device.value());
                app::AppDesc desc;
                desc.name = "sprites_2d";
                desc.platform = &app;
                desc.fences = device.value().get();
                app::RunStats stats;
                rc = app::run(app, desc, &stats);
                const u32 gl_errors = device.value()->validation_error_count();
                std::printf("sprites_2d: %llu frames, %llu ticks, %u GL errors\n",
                            static_cast<unsigned long long>(stats.frames), static_cast<unsigned long long>(stats.ticks),
                            gl_errors);
                if (rc == 0 && gl_errors != 0) rc = 2;
            }
        }
    }
    platform::shutdown();
    log::shutdown();
    return rc;
}
