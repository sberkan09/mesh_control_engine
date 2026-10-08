// Sample 03_platformer (ROADMAP M3): the playable 2D platformer. The game itself lives in
// game/platformer.cpp (ECS + physics2d + input actions + audio events, no host knowledge); this
// file is the host: window, GL renderer, input system, audio device, camera and HUD.
//
// The player runs and jumps on AABB physics, rides moving platforms (velocity accumulation),
// collects coins (sensor trigger -> SFX + screen flash), stomps or dodges enemies whose FSM
// patrols, chases and returns, avoids spikes and reaches the flag. Music loops gaplessly.
//
//   sample_03_platformer                         play (arrows/WASD move, Space jump, Esc pause)
//   sample_03_platformer -autoplay 1             attract mode: a scripted bot plays
//   sample_03_platformer -headless 1 -ticks N    determinism run: bot input, prints the state hash
//   sample_03_platformer -frames 600 -autoplay 1 -capture shot.ppm
//
// Where game modules are supported the game is loaded from libplatformer.so (-module <path>
// overrides the build-tree path); elsewhere it is linked in.
//
// The determinism hash folds the ECS state, the physics world, the game state, every tick's
// input snapshot and the audio command hash (ROADMAP M3: physics + input + audio).

#include <cmath>
#include <cstdio>

#include "art.h"
#include "game/level.h"
#include "game/platformer.h"
#include "hud_font.h"
#include "mce/audio/components.h"
#include "mce/core.h"
#include "mce/ecs/transform.h"
#include "mce/input/default_map.h"
#include "mce/input/ecs.h"
#include "mce/input/input_system.h"
#include "mce/physics/module.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/render/renderer.h"
#include "mce/render/sprite_animation.h"
#include "mce/rhi_gl/gl_device.h"
#if defined(MCE_PLATFORMER_MODULE)
#include "mce/module_host.h"
#endif

namespace {

using namespace mce;
using namespace mce::math;
using namespace mce::render;
using namespace platformer;

MCE_CVAR(cv_ticks, "ticks", 0, "Headless: ticks to simulate", CVarFlags::None);
MCE_CVAR(cv_frames, "frames", 0, "Quit after N frames (0 = run until the window closes)", CVarFlags::None);
MCE_CVAR(cv_capture, "capture", "", "Write the last frame (with -frames) to this .ppm path", CVarFlags::None);
MCE_CVAR(cv_autoplay, "autoplay", false, "A scripted bot plays (always on when headless)", CVarFlags::None);
#if defined(MCE_PLATFORMER_MODULE)
MCE_CVAR(cv_module, "module", MCE_PLATFORMER_MODULE, "The game module to load", CVarFlags::RequiresRestart);
#endif
MCE_CVAR(cv_trace, "trace", 0, "Print the player's state every N ticks", CVarFlags::None);
MCE_CVAR(cv_gl_debug, "r.gl_debug", true, "Enable the GL_KHR_debug validation callback", CVarFlags::RequiresRestart);

constexpr f32 kPpm = 16.0f;
constexpr f32 kViewHeightTiles = 14.0f;

// ---- the attract-mode bot ----------------------------------------------------------------------
// A pure function of the sim state (the previous tick's world), so its input stream is as
// deterministic as the game. It runs right and jumps at walls, enemies and spikes; at a drop it
// steps down when there is floor below, and at a pit it jumps only when solid ground is in reach,
// otherwise it waits at the edge for a moving platform to come by and walks onto it.
class Bot {
public:
    input::InputState sample(World& world, u64 tick) {
        input::InputState s;
        s.tick = tick;
        const GameState& gs = world.resource<GameState>();
        const physics::World2D& pw = world.resource<physics::World2D>();
        physics::BodyState ps;
        if (gs.won || !pw.body_state(gs.player, ps)) return s;
        const Player& pl = *world.try_get<Player>(gs.player);
        const vec2 at = ps.position;

        physics::QueryFilter solid;
        solid.layer_mask = 1u << Layer_World;
        auto floor_at = [&](f32 x, f32 depth) { return pw.raycast({x, at.y}, {0, -1}, depth, solid); };

        // A wall gets a short hop (a step up), everything else a full jump.
        const bool wall = pw.raycast({at.x, at.y - 0.3f}, {1, 0}, 1.1f, solid).has_value();
        bool jump = wall && pw.raycast({at.x, at.y + 0.8f}, {1, 0}, 1.1f, solid).has_value();  // tall wall
        bool hazard_near = false, hazard_below = false;
        world.query<const Hazard&, const LocalTransform&>().each([&](const Hazard&, const LocalTransform& t) {
            const f32 dx = t.position.x - at.x, dy = t.position.y - at.y;
            if (dx > 0.3f && dx < 2.2f && std::fabs(dy) < 1.5f) hazard_near = true;
            if (dx > 0.0f && dx < 4.0f && dy < 0.0f && dy > -6.0f) hazard_below = true;
        });
        world.query<const Enemy&>().each([&](Entity e, const Enemy&) {
            physics::BodyState es;
            if (!pw.body_state(e, es)) return;
            const f32 dx = es.position.x - at.x;
            if (dx > 0.0f && dx < 2.6f && std::fabs(es.position.y - at.y) < 1.0f) jump = true;
        });
        jump = jump || hazard_near;

        f32 move = 1.0f;
        if (!floor_at(at.x + 0.9f, 1.2f)) {  // the floor ends just ahead
            if (hazard_below) {
                jump = true;
            } else if (!floor_at(at.x + 0.9f, 30.0f)) {  // a pit, not a step down
                bool landing = false;
                for (f32 dx = 2.0f; dx <= 5.0f && !landing; dx += 0.5f) {
                    const auto hit = pw.raycast({at.x + dx, at.y + 1.0f}, {0, -1}, 4.0f, solid);
                    landing = hit && !world.has<Mover>(hit->entity);
                }
                if (landing) jump = true;
                else move = 0.0f;
            }
        }

        // Airborne over a pit with floor behind: steer back rather than fall in.
        if (!pl.grounded && !floor_at(at.x + 0.5f, 30.0f) && floor_at(at.x - 1.0f, 30.0f)) move = -1.0f;

        s.move.x = move;
        if ((jump || wall) && pl.grounded && hold_ == 0) {
            hold_ = jump ? 18u : 7u;
            s.jump_pressed = true;
        }
        s.jump_held = hold_ > 0;
        if (hold_ > 0) --hold_;
        return s;
    }

private:
    u32 hold_ = 0;
};

// ---- host ------------------------------------------------------------------------------------------

class PlatformerApp final : public app::Application, public app::IPlatformHooks {
public:
    PlatformerApp(platform::Window* window, rhi::RenderDevice* device, audio::AudioDevice& audio, bool autoplay)
        : window_(window), device_(device), audio_(audio), autoplay_(autoplay) {}

    ResultV on_init(app::Context&) override {
        art::make_clips(clips_);
        Sounds sounds;
        sounds.jump = register_clip(clips_.jump);
        sounds.pickup = register_clip(clips_.pickup);
        sounds.hurt = register_clip(clips_.hurt);
        sounds.stomp = register_clip(clips_.stomp);
        sounds.win = register_clip(clips_.win);

        // Engine runtime first, then the game on top (the game-module contract).
        input::install(world_);
        physics::install_physics2d(world_);
        audio::install(world_, audio_);
        world_.set_resource(sounds);
#if defined(MCE_PLATFORMER_MODULE)
        MCE_TRY_ASSIGN(module_.module, load_game_module(cv_module.get_string(), world_));
        MCE_LOG_INFO("03_platformer: game module '{}' v{} from {}", module_.module.info->name,
                     module_.module.info->module_version, module_.module.loaded_path.c_str());
#else
        MCE_TRY(platformer::install(world_));
#endif

        // Music: a streamed clip looping on the music bus through the AudioSource bridge.
        audio::ClipPayload music;
        music.frames = clips_.music.data();
        music.frame_count = static_cast<u32>(clips_.music.size());
        if (auto h = audio_.register_stream(music)) {
            audio::AudioSource src;
            src.clip_handle = h.value();
            src.loop = true;
            src.play_on_start = true;
            src.bus = audio::Bus_Music;
            src.gain_db = -8.0f;
            src.priority = 0;
            world_.spawn_with(src);
        }

        if (window_) {
            Array<byte> map;
            input::default_map_chunk(map);
            MCE_TRY(input_.load_map(map.span()));
            window_->set_event_handler(&PlatformerApp::on_event, this);
        }
        if (device_) MCE_TRY(init_render());
        return ok();
    }

    void on_tick(const app::TickContext& t) override {
        const input::InputState in = (autoplay_ || !t.actions) ? bot_.sample(world_, t.tick_index) : *t.actions;
        input_hash_ = hash_combine(input_hash_, input::hash_state(in));
        input::publish(world_, in);
        world_.tick(t.dt, t.tick_index);
        ++ticks_;
        if (cv_trace.get_int() > 0 && t.tick_index % static_cast<u64>(cv_trace.get_int()) == 0) trace(t.tick_index);

        GameState& gs = world_.resource<GameState>();
        if (gs.shake > 0.0f) shake_.add_trauma(gs.shake);
        gs.shake = 0.0f;
        shake_.tick(static_cast<f32>(t.dt));
        if (!device_) audio_.update();  // headless: the Silent backend mixes one block per call
    }

    void on_render(const app::RenderContext& rc) override {
        audio_.update();
        if (!device_) return;
        render(rc);
    }

    void on_shutdown() override {
        if (device_) renderer_.shutdown();
    }

    bool pump_events() override {
        const bool quit_requested = platform::pump_events();
        return quit_requested;
    }
    void present(bool vsync) override { device_->present(vsync); }

    u64 state_hash() { return hash_combine(hash_game(world_, kFnv64Offset), hash_combine(input_hash_, audio_.command_hash())); }
    const GameState& game() { return world_.resource<GameState>(); }
    // fn(crate, body state, distance from the crate's bottom to the nearest solid below it).
    template <class Fn>
    void each_crate(Fn&& fn) {
        const auto& pw = world_.resource<physics::World2D>();
        world_.query<const Crate&>().each([&](Entity e, const Crate& c) {
            physics::BodyState s;
            if (!pw.body_state(e, s)) return;
            physics::QueryFilter below;
            const Entity self[] = {e};
            below.exclude = self;
            const auto hit = pw.raycast(s.position, {0, -1}, 5.0f, below);
            fn(c, s, hit ? hit->t - 0.5f : -1.0f);
        });
    }

    u64 ticks() const { return ticks_; }

private:
    void trace(u64 tick) {
        const GameState& gs = world_.resource<GameState>();
        physics::BodyState ps;
        const Player* p = world_.try_get<Player>(gs.player);
        if (!p || !world_.resource<physics::World2D>().body_state(gs.player, ps)) return;
        std::printf("tick %llu: pos %.2f %.2f vel %.2f %.2f grounded %u coins %u deaths %u\n",
                    static_cast<unsigned long long>(tick), static_cast<double>(ps.position.x),
                    static_cast<double>(ps.position.y), static_cast<double>(p->vel.x), static_cast<double>(p->vel.y),
                    p->grounded, gs.coins, gs.deaths);
    }

    audio::ClipHandle register_clip(const Array<f32>& pcm) {
        audio::ClipPayload p;
        p.frames = pcm.data();
        p.frame_count = static_cast<u32>(pcm.size());
        auto h = audio_.register_clip(p);
        return h ? h.value() : audio::ClipHandle{};
    }

    // ---- rendering --------------------------------------------------------------------------------

    ResultV init_render() {
        MCE_TRY(renderer_.init(*device_));

        Array<u8> atlas;
        art::make_atlas(atlas);
        const TextureMipUpload mip{art::kTexels, art::kTexels, std::as_bytes(atlas.span())};
        TextureUpload up;
        up.format = rhi::TextureFormat::RGBA8_SRGB;
        up.layers = art::LayerCount;
        up.mips = Span<const TextureMipUpload>(&mip, 1);
        up.debug_name = "03_platformer.atlas";
        const u32 atlas_key = renderer_.textures().allocate_key();
        MCE_TRY(renderer_.textures().upload(atlas_key, 1, up));

        SpriteRegion regions[art::LayerCount];
        for (u16 l = 0; l < art::LayerCount; ++l) regions[l] = SpriteRegion{l, 0, 0, 1, 1, 1, 1};
        SpriteSetDesc sd;
        sd.texture_key = atlas_key;
        sd.regions = regions;
        sd.pixels_per_meter = kPpm;
        sd.debug_name = "03_platformer.sprites";
        sprites_ = renderer_.create_sprite_set(sd);

        // Tile set: TileGrid value n draws region n-1.
        const SpriteRegion tile_regions[] = {regions[art::GroundTop], regions[art::GroundFill], regions[art::Plank]};
        sd.regions = tile_regions;
        sd.debug_name = "03_platformer.tiles";
        tiles_set_ = renderer_.create_sprite_set(sd);

        TileGrid grid;
        level_tiles(grid);
        TilemapDesc td;
        td.set = tiles_set_;
        td.width = grid.width;
        td.height = grid.height;
        td.tiles = grid.tiles.span();
        td.sort_layer = -1;
        MCE_TRY_ASSIGN(tilemap_, renderer_.create_tilemap(td));

        samples::HudFont hud;
        MCE_TRY_ASSIGN(font_, renderer_.create_font(hud.metrics, hud.glyphs.span(), {}, std::as_bytes(hud.atlas.span()),
                                                    "03_platformer.hud_font"));
        return ok();
    }

    vec2 player_position(f32 alpha) {
        const GameState& gs = world_.resource<GameState>();
        const LocalTransform* t = world_.try_get<LocalTransform>(gs.player);
        const PrevPos* p = world_.try_get<PrevPos>(gs.player);
        if (!t || !p) return gs.level_size * 0.5f;
        return lerp(p->p, vec2{t->position.x, t->position.y}, alpha);
    }

    void render(const app::RenderContext& rc) {
        device_->begin_frame(rc.frame_index);
        const rhi::Extent2D bb = device_->backbuffer_size();
        const GameState& gs = world_.resource<GameState>();

        // Camera: follow the player with a little look-ahead, clamped to the level.
        camera_.zoom = static_cast<f32>(bb.height) / (kViewHeightTiles * kPpm);
        View2D view = compute_view_2d(camera_, bb, kPpm);
        const vec2 half{0.5f * static_cast<f32>(bb.width) / view.pixels_per_meter,
                        0.5f * static_cast<f32>(bb.height) / view.pixels_per_meter};
        vec2 target = player_position(rc.alpha) + vec2{2.0f, 1.5f};
        target.x = clamp(target.x, half.x, math::max(half.x, gs.level_size.x - half.x));
        target.y = clamp(target.y, half.y, math::max(half.y, gs.level_size.y - half.y));
        if (!camera_init_) {
            cam_pos_ = target;
            camera_init_ = true;
        }
        const f32 k = 1.0f - std::exp(-8.0f * static_cast<f32>(rc.frame_dt));
        cam_pos_ = lerp(cam_pos_, target, k);
        camera_.position = cam_pos_;
        view = compute_view_2d(camera_, bb, kPpm, shake_.offset());

        SpriteBatch world(renderer_.sprite_sets(), mem::frame());
        world.begin(view);
        add_background(world, half);
        renderer_.add_tilemap(world, tilemap_);
        add_entities(world, rc.alpha);
        world.build();

        SpriteBatch hud(renderer_.sprite_sets(), mem::frame());
        hud.begin(compute_screen_view(bb), PassId::Screen2D);
        add_hud(hud, bb);
        hud.build();

        rhi::PassAttachment color;
        color.clear.color[0] = 0.36f;
        color.clear.color[1] = 0.62f;
        color.clear.color[2] = 0.92f;
        rhi::RenderPassDesc pass;
        pass.colors = Span<const rhi::PassAttachment>(&color, 1);
        pass.debug_name = "03_platformer";
        rhi::CommandBuffer cmd(mem::frame());
        cmd.begin_pass(pass);
        renderer_.record_2d(cmd, world);
        renderer_.record_2d(cmd, hud);
        cmd.end_pass();
        device_->submit(cmd);

        const bool last = cv_frames.get_int() > 0 && rc.frame_index + 1 >= static_cast<u64>(cv_frames.get_int());
        if (last && !cv_capture.get_string().empty()) capture(cv_capture.get_string().data(), bb);
        device_->end_frame();
        if (last) quit(0);
    }

    void add_background(SpriteBatch& b, vec2 half) {
        // Parallax: hills at 0.3, clouds at 0.55 of the camera's motion.
        for (i32 i = 0; i < 12; ++i) {
            SpriteInstance s;
            s.set = sprites_;
            s.region = art::Hill;
            s.scale = vec2{14.0f, 7.0f};
            s.pivot = vec2{0.5f, 0.0f};
            s.position = vec2{static_cast<f32>(i) * 11.0f - 6.0f, 0.0f} + vec2{cam_pos_.x * 0.3f, cam_pos_.y - half.y - 1.0f};
            s.tint = Color{0.75f, 0.9f, 0.8f, 1};
            s.sort_layer = -3;
            s.sort_order = i;
            b.add(s);
        }
        for (i32 i = 0; i < 14; ++i) {
            SpriteInstance s;
            s.set = sprites_;
            s.region = art::Cloud;
            s.scale = vec2{4.0f + static_cast<f32>(i % 3), 2.5f};
            s.position = vec2{static_cast<f32>(i) * 9.0f + static_cast<f32>((i * 5) % 7), 12.0f + static_cast<f32>((i * 3) % 4)} +
                         vec2{cam_pos_.x * 0.55f, 0.0f};
            s.sort_layer = -2;
            s.sort_order = i;
            b.add(s);
        }
    }

    void add_entities(SpriteBatch& b, f32 alpha) {
        const GameState& gs = world_.resource<GameState>();
        const f64 t = static_cast<f64>(gs.level_ticks) / 60.0;
        static constexpr u32 kCoinFrames[] = {art::Coin0, art::Coin1, art::Coin2, art::Coin3};
        static constexpr u32 kRunFrames[] = {art::PlayerRunA, art::PlayerRunB, art::PlayerRunA, art::PlayerIdle};
        SpriteClip coin_clip;
        coin_clip.frames = kCoinFrames;
        coin_clip.fps = 8.0f;
        SpriteClip run_clip;
        run_clip.frames = kRunFrames;
        run_clip.fps = 10.0f;

        world_.query<const Look&, const PrevPos&, const LocalTransform&>().each(
            [&](Entity e, const Look& l, const PrevPos& prev, const LocalTransform& lt) {
                SpriteInstance s;
                s.set = sprites_;
                s.position = lerp(prev.p, vec2{lt.position.x, lt.position.y}, alpha);
                s.scale = l.size;
                s.sort_order = l.order;
                switch (l.sprite) {
                    case Sprite::Player: {
                        const Player& p = *world_.try_get<Player>(e);
                        s.region = !p.grounded ? art::PlayerJump : p.running ? clip_region(run_clip, t) : art::PlayerIdle;
                        s.flip_x = p.facing < 0;
                        s.position.y += 0.5f - 0.45f;  // art feet sit on the collider's bottom
                        if (p.invuln > 0 && (p.invuln / 4) % 2 == 0) s.tint.a = 0.35f;
                        break;
                    }
                    case Sprite::Enemy: {
                        const Enemy& en = *world_.try_get<Enemy>(e);
                        s.region = en.state == EnemyState::Chase ? art::EnemyAngry : art::EnemyCalm;
                        s.flip_x = en.dir < 0.0f;
                        s.position.y += 0.5f - 0.4f;
                        break;
                    }
                    case Sprite::Coin: {
                        s.region = clip_region(coin_clip, t + static_cast<f64>(s.position.x) * 0.1);
                        s.position.y += 0.08f * std::sin(static_cast<f32>(t) * 3.0f + s.position.x);
                        break;
                    }
                    case Sprite::Spike: s.region = art::Spike; break;
                    case Sprite::Flag: s.region = art::Flag; break;
                    case Sprite::Platform: s.region = art::Platform; break;
                    case Sprite::Crate: {
                        // Crates rotate: draw the physics angle (no interpolation needed at 60 Hz).
                        s.region = art::Crate;
                        s.rotation = angle_z(lt.rotation);
                        break;
                    }
                    case Sprite::Count: return;
                }
                b.add(s);
            });
    }

    static u32 clip_region(const SpriteClip& clip, f64 time) {
        SpriteAnimState st;
        st.frame = clip_frame_at(clip, time);
        return current_region(clip, st);
    }

    void add_hud(SpriteBatch& b, rhi::Extent2D bb) {
        const GameState& gs = world_.resource<GameState>();
        const f32 w = static_cast<f32>(bb.width), h = static_cast<f32>(bb.height);
        if (gs.flash_ticks > 0) {
            SpriteInstance s;
            s.set = sprites_;
            s.region = art::White;
            s.position = vec2{0.5f * w, 0.5f * h};
            s.scale = vec2{w, h};
            s.tint = gs.won ? Color{1, 0.95f, 0.6f, 0.03f * static_cast<f32>(gs.flash_ticks)}
                            : Color{1, 1, 0.8f, 0.05f * static_cast<f32>(gs.flash_ticks)};
            s.sort_layer = 10;
            b.add(s);
        }

        const f64 secs = static_cast<f64>(gs.won ? gs.won_at : gs.level_ticks) / 60.0;
        char line[160];
        std::snprintf(line, sizeof(line), "COINS %u/%u   STOMPS %u   DEATHS %u   TIME %.1f", gs.coins, gs.coins_total,
                      gs.stomps, gs.deaths, secs);
        text(b, line, vec2{14, 12}, Color{1, 1, 1, 1});
        text(b, autoplay_ ? "AUTOPLAY" : "ARROWS/WASD MOVE   SPACE JUMP   ESC PAUSE", vec2{14, h - 30},
             Color{1, 1, 1, 0.8f});
        if (gs.paused) text(b, "PAUSED", vec2{0.5f * w - 36, 0.5f * h - 10}, Color{1, 0.9f, 0.3f, 1});
        if (gs.won) {
            std::snprintf(line, sizeof(line), "YOU WIN!  %u/%u COINS IN %.1f S", gs.coins, gs.coins_total, secs);
            text(b, line, vec2{0.5f * w - 190, 0.5f * h - 10}, Color{1, 0.9f, 0.3f, 1});
        }
    }

    void text(SpriteBatch& b, const char* s, vec2 at, Color tint) {
        TextLayout layout(mem::frame());
        layout_text(font_.face, s, {}, layout);
        TextPlacement shadow;
        shadow.origin = at + vec2{2, 2};
        shadow.tint = Color{0, 0, 0, 0.6f * tint.a};
        shadow.sort_layer = 11;
        b.add_text(font_, layout, shadow);
        TextPlacement p;
        p.origin = at;
        p.tint = tint;
        p.sort_layer = 12;
        b.add_text(font_, layout, p);
    }

    void capture(const char* path, rhi::Extent2D size) {
        Array<u8> px;
        px.resize(usize(size.width) * size.height * 4);
        if (!device_->read_pixels({}, rhi::Rect2D{0, 0, size.width, size.height}, px.span())) return;
        std::FILE* f = std::fopen(path, "wb");
        if (!f) {
            MCE_LOG_ERROR("03_platformer: cannot write '{}'", path);
            return;
        }
        std::fprintf(f, "P6\n%u %u\n255\n", size.width, size.height);
        for (usize i = 0; i < px.size(); i += 4) std::fwrite(&px[i], 1, 3, f);
        std::fclose(f);
        MCE_LOG_INFO("03_platformer: wrote {}", path);
    }

    static void on_event(const platform::Event& e, void* user) { static_cast<PlatformerApp*>(user)->input_.push_event(e); }

public:
    input::InputSystem input_;

private:
    platform::Window* window_;
    rhi::RenderDevice* device_;
    audio::AudioDevice& audio_;
    bool autoplay_;
#if defined(MCE_PLATFORMER_MODULE)
    // Declared before the World so it is destroyed after it: the World holds the module's code.
    struct ModuleGuard {
        LoadedModule module;
        ~ModuleGuard() { unload_game_module(module); }
    } module_;
#endif
    World world_;
    Bot bot_;
    art::Clips clips_;
    u64 input_hash_ = kFnv64Offset;
    u64 ticks_ = 0;

    Renderer renderer_;
    SpriteSetId sprites_;
    SpriteSetId tiles_set_;
    TilemapId tilemap_;
    Font font_;
    Camera2D camera_;
    vec2 cam_pos_;
    bool camera_init_ = false;
    ScreenShake shake_;
};

void report(PlatformerApp& app) {
    const GameState& gs = app.game();
    std::printf("03_platformer: %llu ticks | coins %u/%u stomps %u deaths %u won %d | hash %016llx\n",
                static_cast<unsigned long long>(app.ticks()), gs.coins, gs.coins_total, gs.stomps, gs.deaths,
                gs.won ? 1 : 0, static_cast<unsigned long long>(app.state_hash()));
    // Crates (M3.5 demo): where the solver left them, and the gap to whatever they stand on.
    app.each_crate([](const Crate& c, const physics::BodyState& s, f32 support_gap) {
        std::printf("  crate %u at (%.3f, %.3f) angle %.4f speed %.4f | gap below %.4f\n", c.id, s.position.x,
                    s.position.y, s.angle, length(s.linear_velocity), support_gap);
    });
}

int fail(const char* what, const Error& e) {
    FixedString<512> msg;
    format_error(e, msg);
    FixedString<640> text;
    text.append(what);
    text.append(": ");
    text.append(msg.view());
    platform::show_error_box("03_platformer", text.c_str());
    return 1;
}

int run_headless() {
    audio::AudioDesc ad;
    ad.backend = audio::Backend::Silent;
    auto audio = audio::AudioDevice::create(ad);
    if (!audio) return 1;
    PlatformerApp app(nullptr, nullptr, *audio.value(), true);
    app::AppDesc desc;
    desc.name = "03_platformer";
    desc.headless = true;
    desc.max_ticks = static_cast<u64>(cv_ticks.get_int() > 0 ? cv_ticks.get_int() : 1800);
    const int rc = app::run(app, desc);
    report(app);
    return rc;
}

int run_windowed() {
    int rc = 1;
    platform::WindowDesc wd;
    wd.title = "MCE - 03_platformer";
    wd.width = 1280;
    wd.height = 720;
    auto window = platform::Window::create(wd);
    if (!window) return fail("window creation failed", window.error());
    rhi::DeviceDesc dd;
    dd.debug_layer = cv_gl_debug.get_bool();
    auto device = rhi::gl::create_device(*window.value(), dd);
    if (!device) return fail("OpenGL 4.5 device creation failed", device.error());
    auto audio = audio::AudioDevice::create({});  // falls back to Silent when no output device opens
    if (!audio) return fail("audio init failed", audio.error());

    PlatformerApp app(window.value().get(), device.value().get(), *audio.value(), cv_autoplay.get_bool());
    app::AppDesc desc;
    desc.name = "03_platformer";
    desc.platform = &app;
    desc.fences = device.value().get();
    desc.input_source = &app.input_;
    app::RunStats stats;
    rc = app::run(app, desc, &stats);
    report(app);
    const u32 gl_errors = device.value()->validation_error_count();
    std::printf("03_platformer: %llu frames, %u GL errors\n", static_cast<unsigned long long>(stats.frames), gl_errors);
    if (rc == 0 && gl_errors != 0) rc = 2;
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    log::LogDesc log_desc;
    log_desc.app_name = "03_platformer";
    log::init(log_desc);
    console::parse_command_line(argc, argv);

    const ConVar* headless = ConVar::find(StringId("headless"));
    int rc = 1;
    if (headless && headless->get_bool()) {
        rc = run_headless();
    } else if (ResultV r = platform::init(); !r) {
        rc = fail("platform init failed", r.error());
    } else {
        rc = run_windowed();
    }
    if (!(headless && headless->get_bool())) platform::shutdown();
    log::shutdown();
    return rc;
}
