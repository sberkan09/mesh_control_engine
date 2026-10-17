// net_arena (M-NET3 step 5): a 2-4 player top-down arena on mce_net. The arena is scene data
// (assets/scenes/arena.mcscene: NetReplicated walls and spawn pads); the server makes the players
// (NetOwner, ArenaHealth with Replicated fields); clients predict their own player, interpolate the
// others, and shoot with lag compensation. Players find each other through mce_relay's quick play.
//
//   sample_net_arena -headless 1 -clients 4 -latency 150 -jitter 20 -loss 0.03 -ticks 900
//                                   a whole match over SimNetwork; prints the verdict and a hash
//   sample_net_arena -serve 1 -port 27015     a dedicated server (UDP)
//   sample_net_arena -connect 127.0.0.1:27015 one windowed player; start one process per player
//   sample_net_arena                          windowed, local: you (WASD + mouse, Space/click fires)
//                                             against three bots over a simulated 150 ms link
//   -frames N -capture out.ppm                run N frames, write the last one as an image

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "hud_font.h"
#include "mce/core.h"
#include "mce/net/host.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/render/renderer.h"
#include "mce/rhi/default_device.h"
#include "session.h"
#include "ship_paths.h"

namespace {

using namespace mce;
using namespace mce::math;
using namespace mce::render;
using namespace mce::samples;

MCE_CVAR(cv_headless, "headless", false, "Run a whole match over SimNetwork without a window", CVarFlags::None);
MCE_CVAR(cv_serve, "serve", false, "Run a dedicated server on UDP", CVarFlags::None);
MCE_CVAR(cv_connect, "connect", "", "host:port of a server to play on (windowed)", CVarFlags::None);
MCE_CVAR(cv_port, "port", 27015, "-serve: the UDP port", CVarFlags::None);
MCE_CVAR(cv_ticks, "ticks", 900, "Ticks to simulate (-headless; -serve: 0 = forever)", CVarFlags::None);
MCE_CVAR(cv_clients, "clients", 4, "Players in the match (2-4; the local window counts as one)", CVarFlags::None);
MCE_CVAR(cv_latency, "latency", 150, "One-way latency in ms on every simulated link", CVarFlags::None);
MCE_CVAR(cv_jitter, "jitter", 20, "Extra one-way delay, up to this many ms", CVarFlags::None);
MCE_CVAR(cv_loss, "loss", 0.03f, "Chance a sequenced message is lost (0..1)", CVarFlags::None);
MCE_CVAR(cv_seed, "seed", 1, "Seed of the latency, jitter and loss rolls", CVarFlags::None);
MCE_CVAR(cv_pack, "pack", "", "Cooked pack with the arena scene (default: the sample's pack)", CVarFlags::None);
MCE_CVAR(cv_frames, "frames", 0, "Quit after N frames (0 = run until the window closes)", CVarFlags::None);
MCE_CVAR(cv_capture, "capture", "", "Write the last frame (with -frames) to this .ppm path", CVarFlags::None);
MCE_CVAR(cv_capture_every, "capture_every", 0, "With -capture: also write every Nth frame as <name>_<frame>.ppm", CVarFlags::None);
MCE_CVAR(cv_autoplay, "autoplay", false, "Local play: a bot drives the window's player too (demo recordings)", CVarFlags::None);
MCE_CVAR(cv_gl_debug, "r.gl_debug", true, "Enable the GL_KHR_debug validation callback", CVarFlags::RequiresRestart);

constexpr const char* kScene = "scenes/arena.mcscene";

Result<void> mount_pack(asset::AssetSystem& assets) {
    FixedString<1024> pack;
    resolve_shipped_path(cv_pack.get_string(), "assets.mce", MCE_SAMPLE_DEV_PACK, pack);
    const StringView roots[1] = {pack.view()};
    return assets.mount(Span<const StringView>(roots, 1));
}

arena::SessionDesc session_desc() {
    arena::SessionDesc d;
    d.clients = static_cast<u32>(std::clamp<i64>(cv_clients.get_int(), 1, arena::kMaxPlayers));
    d.latency_ms = static_cast<u32>(std::max<i64>(cv_latency.get_int(), 0));
    d.jitter_ms = static_cast<u32>(std::max<i64>(cv_jitter.get_int(), 0));
    d.loss = cv_loss.get_float();
    d.seed = static_cast<u64>(cv_seed.get_int());
    return d;
}

// ---- headless: the whole match over SimNetwork -----------------------------------------------

int run_sim() {
    asset::AssetSystem assets;
    if (Result<void> r = mount_pack(assets); !r) {
        FixedString<512> msg;
        format_error(r.error(), msg);
        std::printf("net_arena: FAILED, cannot mount the pack: %s\n", msg.c_str());
        return 2;
    }
    arena::Session session;
    if (Result<void> r = session.start(session_desc(), assets, kScene); !r) {
        FixedString<512> msg;
        format_error(r.error(), msg);
        std::printf("net_arena: FAILED, cannot start the match: %s\n", msg.c_str());
        return 2;
    }
    const i64 ticks = std::max<i64>(cv_ticks.get_int(), 1);
    for (i64 i = 0; i < ticks; ++i) session.tick();
    const bool settled = session.settle();
    bool ok = session.report(stdout) && settled;
    std::printf("  settled: %s\n", settled ? "yes" : "NO");
    if (ok) std::printf("net_arena: OK, hash %016llx\n", static_cast<unsigned long long>(session.fingerprint()));
    else std::printf("net_arena: FAILED\n");
    return ok ? 0 : 1;
}

// ---- dedicated server -------------------------------------------------------------------------

int run_serve() {
    asset::AssetSystem assets;
    if (Result<void> r = mount_pack(assets); !r) return 2;
    net::HostDesc hd;
    hd.port = static_cast<u16>(cv_port.get_int());
    hd.channels = arena::kChannels;
    hd.max_peers = arena::kMaxPlayers;
    net::Host host;
    if (!host.listen(hd)) {
        std::printf("net_arena: cannot listen on port %u\n", hd.port);
        return 2;
    }
    arena::Server server;
    if (Result<void> r = server.start(host, assets, kScene); !r) return 2;
    std::printf("net_arena: serving on UDP port %u\n", host.bound_port());
    std::fflush(stdout);
    const i64 ticks = cv_ticks.get_int();
    auto next = std::chrono::steady_clock::now();
    for (i64 i = 0; ticks <= 0 || i < ticks; ++i) {
        server.step();
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
    }
    std::printf("net_arena: served %llu ticks, %u joins, %u shots, %u kills\n", static_cast<unsigned long long>(server.tick),
                server.stats.joins, server.stats.shots, server.stats.kills);
    return 0;
}

// ---- windowed ----------------------------------------------------------------------------------

constexpr u32 kTile = 16;

Array<u8> make_atlas() {  // layer 0 disc, 1 solid square, 2 ring
    Array<u8> px;
    px.resize(usize(kTile) * kTile * 3 * 4);
    for (u32 layer = 0; layer < 3; ++layer)
        for (u32 y = 0; y < kTile; ++y)
            for (u32 x = 0; x < kTile; ++x) {
                const f32 dx = static_cast<f32>(x) + 0.5f - 8.0f, dy = static_cast<f32>(y) + 0.5f - 8.0f;
                const f32 r = std::sqrt(dx * dx + dy * dy);
                const bool on = layer == 0 ? r < 7.5f : (layer == 1 ? true : (r < 7.5f && r > 5.5f));
                u8* p = px.data() + ((usize(layer) * kTile + y) * kTile + x) * 4;
                p[0] = p[1] = p[2] = 255;
                p[3] = on ? 255 : 0;
            }
    return px;
}

Color player_color(u32 id) {
    static const Color colors[4] = {{0.35f, 0.75f, 1.0f, 1}, {1.0f, 0.55f, 0.3f, 1}, {0.5f, 0.9f, 0.45f, 1}, {0.9f, 0.5f, 0.9f, 1}};
    return colors[(id - 1) % 4];
}

class ArenaApp final : public app::Application, public app::IPlatformHooks {
public:
    ArenaApp(platform::Window& window, rhi::RenderDevice& device) : window_(window), device_(device) {}

    ResultV on_init(app::Context&) override {
        MCE_TRY(mount_pack(assets_));
        const StringView target = cv_connect.get_string();
        if (target.empty()) {
            arena::SessionDesc d = session_desc();
            d.clients = std::max<u32>(d.clients, 2);
            MCE_TRY(session_.start(d, assets_, kScene));
            session_.human = !cv_autoplay.get_bool();
            client_ = &session_.clients[0];
        } else {
            const usize colon = target.find(':');
            FixedString<128> host(colon == StringView::npos ? target : target.substr(0, colon));
            const u16 port = colon == StringView::npos ? u16(27015) : static_cast<u16>(std::atoi(target.substr(colon + 1).data()));
            net::HostDesc cd = net::Host::client_desc();
            cd.channels = arena::kChannels;
            if (!link_.open_client(cd)) return Error(Errc::IoError, "net_arena: cannot open a UDP socket");
            const net::PeerId peer = link_.connect(host.view(), port);
            if (peer == net::kInvalidPeer) return Error(Errc::IoError, "net_arena: cannot connect");
            remote_.start(link_, peer);
            client_ = &remote_;
        }
        MCE_TRY(renderer_.init(device_));
        const Array<u8> atlas = make_atlas();
        const TextureMipUpload mip{kTile, kTile, std::as_bytes(atlas.span())};
        TextureUpload up;
        up.format = rhi::TextureFormat::RGBA8_SRGB;
        up.layers = 3;
        up.mips = Span<const TextureMipUpload>(&mip, 1);
        up.debug_name = "net_arena.atlas";
        const u32 key = renderer_.textures().allocate_key();
        MCE_TRY(renderer_.textures().upload(key, 1, up));
        SpriteRegion regions[3];
        for (u16 l = 0; l < 3; ++l) regions[l] = SpriteRegion{l, 0, 0, 1, 1, 1, 1};
        SpriteSetDesc sd;
        sd.texture_key = key;
        sd.regions = regions;
        sd.pixels_per_meter = 16.0f;
        set_ = renderer_.create_sprite_set(sd);
        HudFont hud;
        MCE_TRY_ASSIGN(font_, renderer_.create_font(hud.metrics, hud.glyphs.span(), {}, std::as_bytes(hud.atlas.span()),
                                                    "net_arena.hud_font"));
        window_.set_event_handler(&ArenaApp::on_event, this);
        return ok();
    }

    void on_tick(const app::TickContext&) override {
        arena::Input in;
        in.move_x = (keys_[3] ? 1.0f : 0.0f) - (keys_[1] ? 1.0f : 0.0f);
        in.move_y = (keys_[0] ? 1.0f : 0.0f) - (keys_[2] ? 1.0f : 0.0f);
        in.aim = aim_;
        if (const LocalTransform* me = client_->own_transform()) {
            const f32 dx = mouse_world_.x - me->position.x, dy = mouse_world_.y - me->position.y;
            if (dx != 0.0f || dy != 0.0f) aim_ = in.aim = std::atan2(dy, dx);
        }
        if (cv_connect.get_string().empty()) {
            session_.human_input = in;
            session_.human_fire = fire_;
            session_.tick();
        } else {
            remote_.step(in, fire_);
        }
    }

    void on_render(const app::RenderContext& rc) override {
        device_.begin_frame(rc.frame_index);
        const rhi::Extent2D bb = device_.backbuffer_size();
        camera_.fit_width = 34.0f;
        camera_.fit_height = 22.0f;
        const View2D view = compute_view_2d(camera_, bb, 16.0f, {});
        view_ = view;
        bb_ = bb;
        SpriteBatch world(renderer_.sprite_sets(), mem::frame());
        world.begin(view);
        draw(world);
        world.build();

        TextLayout layout(mem::frame());
        layout_text(font_.face, hud_text().c_str(), {}, layout);
        SpriteBatch hud(renderer_.sprite_sets(), mem::frame());
        hud.begin(compute_screen_view(bb), PassId::Screen2D);
        TextPlacement p;
        p.origin = vec2(12, 12);
        p.tint = Color{1.0f, 0.9f, 0.4f, 1};
        hud.add_text(font_, layout, p);
        hud.build();

        rhi::PassAttachment color;
        color.clear.color[0] = 0.04f;
        color.clear.color[1] = 0.05f;
        color.clear.color[2] = 0.08f;
        rhi::RenderPassDesc pass;
        pass.colors = Span<const rhi::PassAttachment>(&color, 1);
        pass.debug_name = "net_arena";
        rhi::CommandBuffer cmd(mem::frame());
        cmd.begin_pass(pass);
        renderer_.record_2d(cmd, world);
        renderer_.record_2d(cmd, hud);
        cmd.end_pass();
        device_.submit(cmd);
        const bool last = cv_frames.get_int() > 0 && rc.frame_index + 1 >= static_cast<u64>(cv_frames.get_int());
        if (!cv_capture.get_string().empty()) {
            const i64 every = cv_capture_every.get_int();
            if (every > 0 && (rc.frame_index + 1) % static_cast<u64>(every) == 0) {
                StringView base = cv_capture.get_string();
                if (base.ends_with(".ppm")) base.remove_suffix(4);
                FixedString<1100> name(base);
                char suffix[32];
                std::snprintf(suffix, sizeof(suffix), "_%05llu", static_cast<unsigned long long>(rc.frame_index) + 1);
                name.append(suffix);
                name.append(".ppm");
                capture(name.c_str(), bb);
            }
            if (last) capture(cv_capture.get_string().data(), bb);
        }
        device_.end_frame();
        if (last) quit(0);
    }

    void on_shutdown() override {
        arena::Client& c = *client_;
        std::printf("net_arena: %u snapshots (%u deltas), %u shots, %u corrections, own player %s\n", c.snapshots, c.deltas,
                    c.shots_sent, c.predictor.stats().corrections, c.own_id ? "seen" : "NOT seen");
        renderer_.shutdown();
    }

    bool pump_events() override { return platform::pump_events(); }
    void present(bool vsync) override { device_.present(vsync); }

private:
    void draw(SpriteBatch& batch) {
        arena::Client& c = *client_;
        auto add = [&](u16 region, vec2 pos, vec2 scale, f32 rot, Color tint, i32 order) {
            SpriteInstance s;
            s.set = set_;
            s.region = region;
            s.position = pos;
            s.rotation = rot;
            s.scale = scale;
            s.tint = tint;
            s.sort_order = order;
            batch.add(s);
        };
        c.world.query<const net::NetId, const LocalTransform>().each([&](const net::NetId& id, const LocalTransform& t) {
            const vec2 pos(t.position.x, t.position.y);
            if (id.kind == arena::kKindWall) add(1, pos, vec2(t.scale.x, t.scale.y), 0.0f, Color{0.35f, 0.38f, 0.45f, 1}, 0);
            else if (id.kind == arena::kKindPad) add(2, pos, vec2(1.6f, 1.6f), 0.0f, Color{0.3f, 0.5f, 0.35f, 1}, 1);
        });
        c.world.query<const net::NetId, const arena::ArenaHealth, const LocalTransform>().each(
            [&](const net::NetId& id, const arena::ArenaHealth& h, const LocalTransform& t) {
                if (id.kind != arena::kKindPlayer) return;
                const vec2 pos(t.position.x, t.position.y);
                Color col = player_color(id.id);
                if (h.hp == 0) col.a = 0.25f;
                const f32 yaw = arena::yaw_of(t.rotation);
                add(0, pos, vec2(0.9f, 0.9f), 0.0f, col, 10);
                add(1, pos + vec2(std::cos(yaw), std::sin(yaw)) * 0.6f, vec2(0.5f, 0.14f), yaw, col, 11);
                const f32 hp = static_cast<f32>(h.hp) / static_cast<f32>(arena::kMaxHealth);  // a health bar above
                add(1, pos + vec2(0, 0.85f), vec2(1.0f, 0.12f), 0.0f, Color{0.15f, 0.15f, 0.15f, 0.8f}, 12);
                add(1, pos + vec2(-0.5f + 0.5f * hp, 0.85f), vec2(hp, 0.12f), 0.0f, Color{0.3f, 0.9f, 0.3f, 1}, 13);
            });
        for (const arena::Beam& b : c.beams) {
            const vec2 d = b.to - b.from;
            const f32 len = std::sqrt(d.x * d.x + d.y * d.y);
            if (len < 0.01f) continue;
            add(1, (b.from + b.to) * 0.5f, vec2(len, 0.07f), std::atan2(d.y, d.x), Color{1.0f, 0.95f, 0.5f, 0.9f}, 20);
        }
    }

    std::string hud_text() {
        arena::Client& c = *client_;
        char buf[64];
        std::string text = "NET ARENA   WASD MOVE  MOUSE AIM  SPACE FIRE\n";
        c.world.query<const net::NetId, const arena::ArenaHealth>().each([&](const net::NetId& id, const arena::ArenaHealth& h) {
            if (id.kind != arena::kKindPlayer) return;
            std::snprintf(buf, sizeof(buf), "%sP%u HP %u FRAGS %u DEATHS %u\n", id.id == c.own_id ? "> " : "  ", id.id, h.hp,
                          h.frags, h.deaths);
            text += buf;
        });
        std::snprintf(buf, sizeof(buf), "SNAPSHOTS %u  CORRECTIONS %u", c.snapshots, c.predictor.stats().corrections);
        text += buf;
        return text;
    }

    void capture(const char* path, rhi::Extent2D size) {
        Array<u8> px;
        px.resize(usize(size.width) * size.height * 4);
        if (!device_.read_pixels({}, rhi::Rect2D{0, 0, size.width, size.height}, px.span())) return;
        std::FILE* f = std::fopen(path, "wb");
        if (!f) return;
        std::fprintf(f, "P6\n%u %u\n255\n", size.width, size.height);
        for (usize i = 0; i < px.size(); i += 4) std::fwrite(&px[i], 1, 3, f);
        std::fclose(f);
    }

    static void on_event(const platform::Event& e, void* user) {
        auto* self = static_cast<ArenaApp*>(user);
        const bool down = e.type == platform::EventType::KeyDown, up = e.type == platform::EventType::KeyUp;
        if (down || up) {
            const platform::Key k = static_cast<platform::Key>(e.code);
            if (down && k == platform::Key::Escape) self->quit(0);
            if (k == platform::Key::W) self->keys_[0] = down;
            if (k == platform::Key::A) self->keys_[1] = down;
            if (k == platform::Key::S) self->keys_[2] = down;
            if (k == platform::Key::D) self->keys_[3] = down;
            if (k == platform::Key::Space) self->fire_ = down;
        } else if (e.type == platform::EventType::MouseMove) {
            self->mouse_world_ = self->view_.center + vec2((e.x - 0.5f * static_cast<f32>(self->bb_.width)) / self->view_.pixels_per_meter,
                                                           -(e.y - 0.5f * static_cast<f32>(self->bb_.height)) / self->view_.pixels_per_meter);
        } else if (e.type == platform::EventType::MouseButtonDown || e.type == platform::EventType::MouseButtonUp) {
            self->fire_ = e.type == platform::EventType::MouseButtonDown;
        }
    }

    platform::Window& window_;
    rhi::RenderDevice& device_;
    asset::AssetSystem assets_;
    Renderer renderer_;
    SpriteSetId set_;
    Font font_;
    Camera2D camera_;
    View2D view_;
    rhi::Extent2D bb_;
    arena::Session session_;  // local play
    arena::Client remote_;    // -connect
    net::Host link_;
    arena::Client* client_ = nullptr;
    bool keys_[4] = {};
    bool fire_ = false;
    f32 aim_ = 0.0f;
    vec2 mouse_world_;
};

int fail(const char* what, const Error& e) {
    FixedString<512> msg;
    format_error(e, msg);
    FixedString<640> text;
    text.append(what);
    text.append(": ");
    text.append(msg.view());
    platform::show_error_box("net_arena", text.c_str());
    return 1;
}

int run_window() {
    int rc = 1;
    if (ResultV r = platform::init(); !r) {
        rc = fail("platform init failed", r.error());
    } else {
        platform::WindowDesc wd;
        wd.title = "MCE - net_arena";
        wd.width = 1280;
        wd.height = 720;
        auto window = platform::Window::create(wd);
        if (!window) {
            rc = fail("window creation failed", window.error());
        } else {
            rhi::DeviceDesc dd;
            dd.debug_layer = cv_gl_debug.get_bool();
            auto device = rhi::create_default_device(*window.value(), dd);
            if (!device) {
                rc = fail("render device creation failed", device.error());
            } else {
                ArenaApp app(*window.value(), *device.value());
                app::AppDesc desc;
                desc.name = "net_arena";
                desc.platform = &app;
                desc.fences = device.value().get();
                app::RunStats stats;
                rc = app::run(app, desc, &stats);
                const u32 gl_errors = device.value()->validation_error_count();
                std::printf("net_arena: %llu frames, %llu ticks, %u GL errors\n", static_cast<unsigned long long>(stats.frames),
                            static_cast<unsigned long long>(stats.ticks), gl_errors);
                if (rc == 0 && gl_errors != 0) rc = 2;
            }
        }
    }
    platform::shutdown();
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    log::LogDesc log_desc;
    log_desc.app_name = "net_arena";
    log::init(log_desc);
    console::parse_command_line(argc, argv);
    int rc = 0;
    if (cv_headless.get_bool()) rc = run_sim();
    else if (cv_serve.get_bool()) rc = run_serve();
    else rc = run_window();
    log::shutdown();
    return rc;
}
