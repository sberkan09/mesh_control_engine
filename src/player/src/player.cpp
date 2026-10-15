// The player runtime (mce/player/player.h): mce_player and every project host built from the
// project template run this. It plays one cooked scene (editor-tools.md §2.1: the run-in-player
// child process), with the project's game module installed on top of the engine systems.
//
//   mce_player -pack <assets.mce | cooked dir> [-scene <guid | source path>] [-module <game module>]
//   mce_player -pack p.mce -headless 1 -ticks 600      determinism: prints the World state hash
//   mce_player -pack p.mce -headless 1 -input moves.txt  scripted input (mce/input/script.h) instead of the keyboard
//   mce_player -pack p.mce -frames 120 -capture a.ppm  CI smoke: write the last frame
//   mce_player -pack p.mce -capture_tick 300 -capture a.ppm   the frame after sim tick 300, then quit
//   mce_player -pack p.mce -input moves.txt -capture_tick 300 -capture a.ppm   ... with the scripted input
//   mce_player -pack p.mce -capture_ticks 60,120 -capture shot_{tick}.ppm   one frame per listed tick, then quit
//   ... -view.entity 3 | -view.free 0,4,10,0,0,0   render from scene entity 3's camera, or a free one (render only)
//   mce_player -help                                   every convar with its help (the whole command line)
//
// Without -scene the first Scene asset of the pack (by source path) plays. F7 toggles 2D post, F3 the
// render stats line.
//
// 3D (M-KIT3D): a scene with a Camera3DComponent (mce/kit/kit3d.h) plays on the 3D path: the
// forward renderer starts (-r.msaa), every texture and mesh of the pack is uploaded (-r.preload3d),
// the kit3d components are drawn from the primary 3D camera every frame, and the 2D kit, the console
// and the pause overlay draw on top. A camera with mouse_look gets relative mouse mode while the game
// runs (-mouse_look 0 turns that off). Controls: Esc pauses
// and resumes (the pause overlay), Q quits while paused, ` or F1 opens the developer console (FR-UI-045: the log,
// convars and console functions; -console 0 removes it from a shipped build's command line), and F11, Alt+Enter or
// Cmd+Ctrl+F toggle full screen on a desktop (-fullscreen, saved).
//
// Lifecycle (FR-RUN-060): losing focus or minimizing pauses the sim (the pause overlay is up when the player comes
// back, so nothing moves before they choose to resume) and mutes the master bus until focus returns. With
// -save <slot>, a save of that name is loaded on top of the scene at start and written on every clean quit (window
// close, Q, -frames / -ticks reached) before anything shuts down, and once per focus loss or minimize
// (-save_on_focus_loss 0 turns that off) so a desktop kill or sleep keeps the session; the write is atomic with a
// .bak, so a kill during it keeps the last good save (ecs/save.h).
//
// Phones (FR-RUN-100, M-MOBILE): going to the background pauses the sim, pauses the audio device, releases every
// held input, writes the -save slot inside the OS callback and stops drawing until the app is back; the OS ending
// the process writes it too. Back in the foreground the pause overlay waits; a tap (or Esc) resumes, a touch in the
// top-right corner pauses. -lifecycle_test N,M rehearses the round trip at app tick N for M ticks (headless too).
//
// Purchases and ads (M-MONETIZE, PlayerDesc::monetize): off unless the host turns them on (-monetize 1 forces them on
// for testing). Then the pack's store catalog (-store_catalog) and the providers feed the Store and Ads resources
// (mce/monetize/monetize.h): by default the phone's own store and ad network (mce/monetize/platform_store.h,
// platform_ads.h), with -monetize.provider fake the fake ones (mce/monetize/fake.h). A fullscreen ad pauses the sim
// and audio until it closes, with no pause overlay. -monetize_test "10:buy:gems 40:show:x"
// makes requests at those sim ticks and prints every store and ad event (headless too), granting consumables.
//
// Live services (M-LIVEOPS, PlayerDesc::liveops): off unless the host turns them on (-liveops 1 forces them on).
// Then the host's analytics backend and crash uploader (mce/liveops/liveops.h) get what consent allows: analytics
// events and the crash reports earlier runs left in the crash folder. -liveops.provider local writes the events to
// <save folder>/analytics.jsonl and copies the reports to <save folder>/crash_outbox/; none drops them. Consent
// comes from the ad network's form (M-MONETIZE: personalized = granted, otherwise denied) unless the game sets it,
// or from -liveops.consent granted|denied. Remote config (mce/liveops/remote_config.h) comes with them: the pack's
// .mcconfig defaults, the values the last fetch saved, and a fetch at start (the host's fetcher,
// -remote_config.file <cooked pack>, else Firebase Remote Config with the Firebase vendor); the game applies fetched values with RemoteConfig::apply_fetched. Game
// services (mce/liveops/game_services.h) too, when the pack has a .mcservices: -liveops.provider local plays the
// platform with a folder (-game_services.dir, -game_services.offline 1), and -game_services_test rehearses them.
//
// Input: windowed, the starter-kit "Gameplay" map (mce/input/default_map.h: Move on WASD / arrows /
// left stick, Jump on Space / A, Fire on J / left mouse / X, Pause) is sampled once per tick and
// published as the InputState resource the game module reads. Headless runs publish an empty
// InputState every tick, so -headless hashes stay a function of the scene and the tick count.

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "hud_font.h"
#include "mce/asset/asset_system.h"
#include "mce/asset/file_io.h"
#include "mce/asset/mods.h"
#include "mce/audio/components.h"
#include "mce/core.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/input/default_map.h"
#include "mce/input/ecs.h"
#include "mce/input/input_system.h"
#include "mce/input/script.h"
#include "mce/kit/stock.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/particles.h"
#include "mce/kit/sequence.h"
#include "mce/kit/sprite_extract.h"
#include "mce/liveops/game_services.h"
#include "mce/liveops/liveops.h"
#include "mce/liveops/remote_config.h"
#include "mce/loc/loc.h"
#include "mce/monetize/fake.h"
#include "mce/monetize/platform_ads.h"
#include "mce/monetize/platform_store.h"
#include "mce/monetize/monetize.h"
#include "mce/physics/module.h"
#include "mce/platform/ads.h"
#include "mce/platform/firebase.h"
#include "mce/platform/haptics.h"
#include "mce/platform/library.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/render/asset_upload.h"
#include "mce/render/quality.h"
#include "mce/render/renderer.h"
#include "mce/rhi/default_device.h"
#include "mce/module_host.h"
#include "mce/player/player.h"
#include "mce/scene/scene.h"
#include "mce/scene/streaming.h"
#include "mce/ui/ui.h"
#include "ship_paths.h"
#include "ui_screens.h"

namespace {

using namespace mce;
using namespace mce::math;

MCE_CVAR(cv_pack, "pack", "", "Cooked pack (assets.mce) or cooked/ directory to mount (a host's default: assets.mce beside it)",
         CVarFlags::None);
MCE_CVAR(cv_module, "module", "", "Game module to install on top of the engine systems (a host's default: its own module)",
         CVarFlags::None);
MCE_CVAR(cv_mods, "mods", true, "Load the mods in the game's mods folder (only for games that take mods)",
         CVarFlags::None);
MCE_CVAR(cv_mods_dir, "mods_dir", "", "Mods folder override (only for games that take mods; default: the game's own)",
         CVarFlags::None);
MCE_CVAR(cv_scene, "scene", "", "Scene to play: GUID or source path (default: the first scene)", CVarFlags::None);
MCE_CVAR(cv_frames, "frames", 0, "Quit after N frames (0 = run until the window closes)", CVarFlags::None);
MCE_CVAR(cv_capture, "capture", "", "Write the last frame (with -frames or -capture_tick) to this .ppm path (relative to the working directory; parents are created)",
         CVarFlags::None);
MCE_CVAR(cv_capture_tick, "capture_tick", 0,
         "Windowed: quit (and -capture) on the first frame after this many sim ticks of this run, whatever the frame rate; "
         "a tick restored by -save does not count (0 = off)",
         CVarFlags::None);
MCE_CVAR(cv_capture_ticks, "capture_ticks", "",
         "Windowed: capture the first frame after each of these sim ticks (\"60,120,300\", ascending) and quit after the last; "
         "-capture's path takes {tick} (else _<tick> goes before the extension)",
         CVarFlags::None);
MCE_CVAR(cv_view_entity, "view.entity", -1,
         "Render from this scene entity's camera (index in file order, prefab instances flattened) instead of the primary one; "
         "render only, the sim and its hash are unchanged (-1 = off)",
         CVarFlags::None);
MCE_CVAR(cv_view_free, "view.free", "",
         "Render from a free camera instead of the primary one: \"x,y[,zoom]\" (2D) or \"x,y,z,tx,ty,tz[,fov]\" (3D: eye, "
         "target, vertical degrees); render only",
         CVarFlags::None);
MCE_CVAR(cv_safe_area, "safe_area", "",
         "Windowed: pretend the screen has these safe-area insets in pixels, \"left,top,right,bottom\" (a phone notch on a "
         "desktop); UI and ViewInfo lay out inside them",
         CVarFlags::None);
MCE_CVAR(cv_window, "window", "", "Windowed: initial window size as WxH in points, e.g. 1080x2400 to check a phone layout (default 1280x720)",
         CVarFlags::None);
MCE_CVAR(cv_ticks, "ticks", 600, "Headless: ticks to simulate (windowed runs use -frames)", CVarFlags::None);
MCE_CVAR(cv_input, "input", "",
         "Input script file: per-tick action states (\"10-70 Move=1,0 Jump\" per line) published to the game instead of the keyboard",
         CVarFlags::None);
MCE_CVAR(cv_input_map, "input_map", "",
         "Input map to load: GUID or source path of a cooked .mcinput (default: the first in the pack, else the built-in map)",
         CVarFlags::None);
MCE_CVAR(cv_post, "r.post2d", true, "Apply the camera's Post2DComponent effects (F7 toggles)", CVarFlags::None);
MCE_CVAR(cv_pause_on_focus_loss, "pause_on_focus_loss", true, "Pause and mute when the window loses focus or minimizes",
         CVarFlags::None);
MCE_CVAR(cv_save_on_focus_loss, "save_on_focus_loss", true,
         "With -save: also write the slot when the window loses focus or minimizes (once per focus loss)", CVarFlags::None);
MCE_CVAR(cv_fullscreen, "fullscreen", false, "Desktop: borderless full screen (F11, Alt+Enter or Cmd+Ctrl+F toggle it)",
         CVarFlags::Saved);
MCE_CVAR(cv_save, "save", "", "Save slot: loaded at start when it exists, written on clean quit (empty = off)",
         CVarFlags::None);
MCE_CVAR(cv_save_dir, "save_dir", "", "Directory for save slots (default: the per-user MCE save folder)", CVarFlags::None);
MCE_CVAR(cv_store_catalog, "store_catalog", "",
         "Games with purchases and ads: the store catalog, GUID or source path of a cooked .mcstore (default: the first in the pack)",
         CVarFlags::None);
MCE_CVAR(cv_liveops, "liveops", false, "Turn live services (analytics, crash reports) on even when the game's host does not",
         CVarFlags::None);
MCE_CVAR(cv_liveops_provider, "liveops.provider", "",
         "Live services: empty = the host's (else Firebase where the build has it), firebase, local = analytics.jsonl "
         "and crash_outbox/ in the save folder, none = neither",
         CVarFlags::None);
MCE_CVAR(cv_liveops_consent, "liveops.consent", "auto",
         "Live services: auto (the ad network's consent form, or the game), granted or denied", CVarFlags::None);
MCE_CVAR(cv_remote_config_file, "remote_config.file", "",
         "Remote config: fetch newer values from this cooked pack holding a .mcconfig (the static-file backend)",
         CVarFlags::None);
MCE_CVAR(cv_remote_config_dump, "remote_config.dump", false,
         "Remote config: print every value at start and after each apply", CVarFlags::None);
MCE_CVAR(cv_language, "language", "",
         "Play in this language (\"tr\", \"pt-BR\") for this run instead of the saved choice and the device languages",
         CVarFlags::None);
MCE_CVAR(cv_ui_scale, "ui_scale", 1.0f,
         "UI size on top of the display's own scale (1 = as the display asks; lowered when the screen is too small)",
         CVarFlags::Saved);
MCE_CVAR(cv_ui_preview, "ui_preview", "",
         "Show only this UI document (GUID) over the camera's clear color, no world: the MCP server's ui_preview",
         CVarFlags::None);
MCE_CVAR(cv_haptics, "haptics", true,
         "Haptic feedback: phone taps and gamepad rumble from platform::haptics::play (off: none; never in the state hash)",
         CVarFlags::Saved);
MCE_CVAR(cv_default_language, "default_language", "en",
         "The project's base language: the last fallback when neither the choice nor the device languages have a string table",
         CVarFlags::None);
MCE_CVAR(cv_monetize, "monetize", false, "Turn purchases and ads on even when the game's host does not (testing)",
         CVarFlags::None);
MCE_CVAR(cv_monetize_provider, "monetize.provider", "",
         "Purchases and ads: the store and ad network (empty: the platform's, fake: the fake ones, none: neither)",
         CVarFlags::None);
MCE_CVAR(cv_monetize_test, "monetize_test", "",
         "Rehearse purchases and ads: \"tick:action[:id] ...\" with query, buy, restore, load, show, banner, hide; prints every "
         "store and ad event",
         CVarFlags::None);
MCE_CVAR(cv_game_services_dir, "game_services.dir", "",
         "Game services with -liveops.provider local: the folder playing the platform (default: <save folder>/"
         "game_services_platform; share it between save folders to play two devices)",
         CVarFlags::None);
MCE_CVAR(cv_game_services_offline, "game_services.offline", false,
         "Game services with -liveops.provider local: the platform cannot be reached (sign-in and every call fail)",
         CVarFlags::None);
MCE_CVAR(cv_game_services_test, "game_services_test", "",
         "Rehearse game services: \"tick:action[:id[:value]] ...\" with sign_in, unlock, progress, score, write (a local "
         "save's text), cloud_save, cloud_load, keep_local, keep_cloud; prints every game services event",
         CVarFlags::None);
MCE_CVAR(cv_lifecycle_test, "lifecycle_test", "",
         "Rehearse the phone lifecycle: \"N,M\" goes to the background at app tick N and comes back and resumes M ticks later",
         CVarFlags::None);
MCE_CVAR(cv_console, "console", true, "Allow the developer console (` or F1)", CVarFlags::None);
MCE_CVAR(cv_console_open, "console_open", false, "Start with the developer console open", CVarFlags::None);
MCE_CVAR(cv_uniform_ring, "r.uniform_ring_mb", 4,
         "Per-frame uniform memory in MiB (a 3D frame with thousands of draws and shadow cascades needs more than 1)",
         CVarFlags::RequiresRestart);
MCE_CVAR(cv_msaa, "r.msaa", 4, "3D games: MSAA samples of the 3D frame (1 = off)", CVarFlags::RequiresRestart);
MCE_CVAR(cv_lod, "r.lod", true, "3D games: draw coarser mesh levels with distance (meshes cooked with LODs)", CVarFlags::None);
MCE_CVAR(cv_lod_pixels, "r.lod_pixels", 1.0f, "3D games: the largest LOD error allowed on screen, in pixels", CVarFlags::None);
MCE_CVAR(cv_lod_force, "r.lod_force", -1, "3D games: draw every mesh at this LOD level (-1 = by screen size)", CVarFlags::None);
MCE_CVAR(cv_preload3d, "r.preload3d", true, "3D games: upload every texture and mesh of the pack at start (0 = on first sight)",
         CVarFlags::None);
MCE_CVAR(cv_stats, "r.stats", false, "Draw the render stats line (F3 toggles)", CVarFlags::None);
MCE_CVAR(cv_frame_report, "r.frame_report", 0,
         "Windowed: log the average, p95 and worst frame time every N frames, and for the whole run at exit (0 = off)",
         CVarFlags::None);
MCE_CVAR(cv_mouse_look, "mouse_look", true, "3D games: let a Camera3DComponent with mouse_look capture the mouse", CVarFlags::None);
MCE_CVAR(cv_gl_debug, "r.gl_debug", true, "Enable the GL_KHR_debug validation callback", CVarFlags::RequiresRestart);
// Analytics events to Firebase (platform/firebase.h): the vendor backend on phones shipped with a
// Firebase config. Consent reaches the SDK through platform::firebase::set_collection.
class FirebaseAnalytics final : public liveops::analytics::IBackend {
public:
    const char* name() const override { return "firebase"; }
    void send(const liveops::analytics::Event& e) override {
        platform::firebase::Param params[liveops::analytics::kMaxParams];
        u32 n = 0;
        for (const liveops::analytics::Param& p : e.params.params()) {
            platform::firebase::Param& q = params[n++];
            q.key = p.key.view();
            q.i = p.i;
            q.f = p.f;
            q.s = p.s.view();
            q.kind = p.kind == liveops::analytics::Param::Kind::Int     ? platform::firebase::Param::Kind::Int
                     : p.kind == liveops::analytics::Param::Kind::Float ? platform::firebase::Param::Kind::Float
                                                                         : platform::firebase::Param::Kind::String;
        }
        platform::firebase::log_event(e.name.view(), Span<const platform::firebase::Param>(params, n));
    }
};

// Remote config from Firebase Remote Config (platform/firebase.h): the server fetcher on phones
// shipped with a Firebase config. The SDK answers with text on its own thread; each value is typed
// by the cooked defaults, and one the defaults do not know or that does not parse is dropped.
class FirebaseConfigFetcher final : public liveops::IConfigFetcher {
public:
    explicit FirebaseConfigFetcher(const liveops::ConfigValues& defaults) {
        for (const liveops::ConfigValue& v : defaults.values()) defaults_.add(v);
    }
    const char* name() const override { return "firebase"; }
    void fetch(liveops::ConfigFetchSink& sink) override {
        sink_ = &sink;
        values_.clear();
        if (!platform::firebase::fetch_remote_config(&FirebaseConfigFetcher::answer, this)) sink.fetch_failed();
    }

private:
    static void answer(void* user, StringView key, StringView value, bool done, bool ok) {
        auto* self = static_cast<FirebaseConfigFetcher*>(user);
        if (!done) {
            liveops::ConfigValue v;
            if (liveops::parse_config_value(self->defaults_, key, value, v)) self->values_.add(v);
            else MCE_LOG_WARN("player: remote config value '{}' dropped (not in the defaults, or not their type)", key);
            return;
        }
        if (ok) self->sink_->fetched(self->values_);
        else self->sink_->fetch_failed();
    }

    liveops::ConfigValues defaults_;
    liveops::ConfigValues values_;  // the running fetch's, filled on the SDK's thread
    liveops::ConfigFetchSink* sink_ = nullptr;
};

int fail(const char* what, const Error& e) {
    FixedString<512> msg;
    format_error(e, msg);
    FixedString<640> text;
    text.append(what);
    text.append(": ");
    text.append(msg.view());
    platform::show_error_box("mce_player", text.c_str());
    return 1;
}

class PlayerApp final : public app::Application, public app::IPlatformHooks {
public:
    PlayerApp(const player::PlayerDesc& desc, platform::Window* window, rhi::RenderDevice* device, audio::AudioDevice& audio)
        : desc_(desc), window_(window), device_(device), audio_(audio), sink_(renderer_, assets_), clips_(assets_, audio) {}

    ResultV on_init(app::Context&) override {
        FixedString<1024> pack;
        if (!cv_pack.get_string().empty() || desc_.pack)
            samples::resolve_shipped_path(cv_pack.get_string(), desc_.pack ? desc_.pack : "", desc_.pack_fallback ? desc_.pack_fallback : "",
                                          pack);
        if (pack.empty()) return Error(Errc::InvalidArgument, "mce_player: -pack <path> is required");
        MCE_TRY(parse_capture_and_view());
        mount_mods();
        const StringView roots[1] = {pack.view()};
        MCE_TRY(assets_.mount(Span<const StringView>(roots, 1)));
        const StringView want_scene = !cv_scene.get_string().empty() ? cv_scene.get_string()
                                      : desc_.start_scene                ? StringView(desc_.start_scene)
                                                                         : StringView();
        MCE_TRY_ASSIGN(const asset::Guid scene, pick_scene(want_scene));

        if (desc_.mods_dir) world_.set_resource(mods_);  // game code lists them: try_resource<asset::ModList>()
        input::install(world_);
        physics::install_physics2d(world_);
        audio::install(world_, audio_, &clips_);
        kit::install_stock(world_, &assets_);
        if (!cv_save_dir.get_string().empty()) save::set_root(cv_save_dir.get_string());  // the entitlements live there too
        monetize_on_ = desc_.monetize || cv_monetize.get_bool();
        if (monetize_on_) MCE_TRY(init_monetize(cv_store_catalog.get_string()));
        else if (!cv_monetize_test.get_string().empty())
            return Error(Errc::InvalidArgument, "player: -monetize_test needs purchases and ads on (-monetize 1)");
        liveops_on_ = desc_.liveops || cv_liveops.get_bool();
        if (liveops_on_) MCE_TRY(init_liveops());
        else if (!cv_game_services_test.get_string().empty())
            return Error(Errc::InvalidArgument, "player: -game_services_test needs live services on (-liveops 1)");
        MCE_TRY(init_localization());
        // The game on top of the engine systems (the game-module contract), before the scene so
        // its systems see the first tick.
        if (cv_module.get_string().empty() && desc_.static_module) {
            const ModuleInfo* info = desc_.static_module();
            if (!info || !info->install) return Error(Errc::ModuleMissingEntry, "player: the linked game has no install()");
            MCE_TRY(info->install(world_));
            MCE_LOG_INFO("{}: game '{}' v{} (linked in)", desc_.app_name, info->name, info->module_version);
        } else if (!cv_module.get_string().empty() || desc_.module) {
            FixedString<1024> path;
            samples::resolve_shipped_path(cv_module.get_string(), desc_.module ? desc_.module : "",
                                          desc_.module_fallback ? desc_.module_fallback : "", path);
            MCE_TRY_ASSIGN(module_.module, load_game_module(path.view(), world_));
            MCE_LOG_INFO("{}: game module '{}' v{} from {}", desc_.app_name, module_.module.info->name,
                         module_.module.info->module_version, module_.module.loaded_path.c_str());
        }
        console::apply_deferred_command_line();  // the module's convars exist now; the rest are typos
        if (device_) {
            MCE_TRY(renderer_.init(*device_));
            assets_.set_upload_sink(&sink_);
            MCE_TRY(extract_.init(renderer_, &assets_));
            samples::HudFont hud;
            MCE_TRY_ASSIGN(font_, renderer_.create_font(hud.metrics, hud.glyphs.span(), {}, std::as_bytes(hud.atlas.span()),
                                                        "player.hud_font"));
            MCE_TRY(ui::create_builtin_skin(renderer_, skin_));
            console_.init(skin_.skin, 0.5f);
            for (u32 i = 0; i < desc_.ui_layer_count; ++i) {
                const ui::Layer* l = desc_.ui_layers[i];
                if (l && l->init && !l->init(l->user, renderer_, skin_.skin))
                    MCE_LOG_ERROR("{}: game UI layer {} failed to initialize", desc_.app_name, i);
            }
            if (cv_console.get_bool() && cv_console_open.get_bool()) set_console(true);
            MCE_TRY(load_input_map(cv_input_map.get_string()));
            const platform::PixelSize fb = window_->framebuffer_size();
            input_.set_view_size(static_cast<f32>(fb.width), static_cast<f32>(fb.height));  // touch distances
            window_->set_event_handler(&PlayerApp::on_event, this);
        }
        // Scene streaming (FR-ENT-120) is inert unless the scene places SceneChunks. Headless runs
        // load chunks inside the pump that wants them, so -headless hashes stay deterministic.
        scene::StreamingConfig streaming;
        streaming.async = device_ != nullptr;
        scene::install_streaming(world_, assets_, streaming);
        MCE_TRY(scene::load(world_, assets_, scene, &entities_));
        scene::pump_streaming(world_, scene::PumpMode::Settle);
        MCE_TRY(load_ui_screens());
        if (device_ && kit::has_camera_3d(world_)) MCE_TRY(init_3d());
        MCE_LOG_INFO("{}: scene {} ({}) with {} entities", desc_.app_name, asset::to_string(scene).view(), scene_path_.view(),
                     entities_.size());
        scene_guid_ = scene;
        if (!parse_lifecycle_test(cv_lifecycle_test.get_string()))
            return Error(Errc::InvalidArgument, "player: -lifecycle_test wants \"N,M\" (app ticks, N >= 1)");
        if (window_) set_safe_insets(window_insets());
        if (const StringView slot = cv_save.get_string(); !slot.empty() && save::exists(slot)) {
            save::SaveInfo info;
            MCE_TRY_ASSIGN(const save::LoadStats st, save::load_game(world_, slot, &info));
            sim_ticks_ = info.tick;
            run_start_tick_ = info.tick;  // -capture_tick counts this run's ticks, not the restored ones
            std::printf("mce_player: save '%.*s' loaded (%u entities, tick %llu)\n", static_cast<int>(slot.size()), slot.data(),
                        st.entities, static_cast<unsigned long long>(info.tick));
        }
        return ok();
    }

    void on_tick(const app::TickContext& t) override {
        if (lifecycle_from_ != 0) rehearse_lifecycle(t.tick_index);
        if (monetize_on_) {
            monetize_.update();  // also while paused: a fullscreen ad must be able to close
            update_ad_pause();
        }
        if (liveops_on_) update_liveops();  // also while paused: nothing here touches the sim
        if (t.actions) {  // the UI screens navigate with the newest snapshot (edges once: ui_fresh_)
            ui_actions_ = *t.actions;
            ui_fresh_ = true;
        }
        if (paused_ || ad_paused_) return;  // sim_held() mirrors these two tests and the next
        // -capture_tick(s): the sim holds on the tick for the capture frame, however many ticks a slow
        // frame (Metal validation, a cold start) would otherwise catch up past it.
        if (capture_due()) return;
        tick_dt_ = t.dt;
        if (screens_.active()) {
            // The actions the scene's UI screens raised since the last tick ride in this tick's input.
            input::InputState in = t.actions ? *t.actions : input::InputState{};
            screens_.apply_actions(in, static_cast<f32>(t.dt));
            input::publish(world_, in);
        } else if (t.actions) {
            input::publish(world_, *t.actions);
        }
        publish_view_info();
        scene::pump_streaming(world_);
        if (!monetize_test_.empty()) monetize_test_requests();
        if (!game_services_test_.empty()) game_services_test_requests();
        world_.tick(t.dt, sim_ticks_);
        if (!monetize_test_.empty()) monetize_test_events();
        if (game_services_print_) game_services_test_events();
        ++sim_ticks_;
        apply_scene_change();
        if (!device_) audio_.update();  // headless: the Silent backend mixes one block per call
    }

    // kit::ViewInfo for the game's systems: the screen and the primary camera's view as of this tick.
    // Headless keeps the struct's fixed 1280 x 720 landscape defaults (deterministic hashes).
    void publish_view_info() {
        kit::ViewInfo v;
        if (window_ && device_) {
            const platform::PixelSize fb = window_->framebuffer_size();
            const platform::PixelSize pt = window_->size();
            v.framebuffer_width = fb.width;
            v.framebuffer_height = fb.height;
            v.window_width = pt.width;
            v.window_height = pt.height;
            v.display_scale = window_->display_scale();
            v.safe_left = safe_insets_.left;
            v.safe_top = safe_insets_.top;
            v.safe_right = safe_insets_.right;
            v.safe_bottom = safe_insets_.bottom;
            v.orientation = static_cast<u8>(window_->orientation());
            v.headless = false;
            v.ui_scale = ui_scale({fb.width, fb.height});
        }
        render::Camera2D cam;
        f32 ppm = 48.0f;
        kit::primary_camera(world_, cam, ppm);
        const render::View2D view = render::compute_view_2d(cam, {v.framebuffer_width, v.framebuffer_height}, ppm);
        v.view_center = view.center;
        v.view_pixels_per_meter = view.pixels_per_meter;
        v.view_rotation = cam.rotation;
        world_.set_resource(v);
    }

    void on_render(const app::RenderContext& rc) override {
        audio_.update();
        clips_.sync();
        sync_fullscreen();
        // The game's Render_Pre / Render_Submit systems (#215), once the first tick has run Startup.
        if (!os_background_ && sim_ticks_ > 0) world_.render(rc.alpha);
        meter_frame();
        if (device_) platform::haptics::set_enabled(cv_haptics.get_bool());  // the console or a settings menu may flip it
        if (!device_ || os_background_) return;  // no GPU work in the background (iOS ends apps that try)
        device_->begin_frame(rc.frame_index);
        const rhi::Extent2D bb = device_->backbuffer_size();
        render::Camera2D cam;
        f32 ppm = 48.0f;
        Color clear{0.08f, 0.09f, 0.12f, 1.0f};
        kit::primary_camera(world_, cam, ppm, &clear);
        view_override_2d(cam, ppm, clear);

        update_console(bb);
        update_ui_layers(bb);
        update_ui_screens(bb);
        const bool overlay =
            paused_ || console_.is_open() || desc_.ui_layer_count > 0 || screens_.active() || cv_stats.get_bool();

        // 3D: the kit3d scene first (it ends in the backbuffer); the 2D kit draws over it.
        if (!three_d_ && kit::has_camera_3d(world_)) {
            if (Result<void> r = init_3d(); !r) {
                FixedString<512> why;
                format_error(r.error(), why);
                MCE_LOG_ERROR("{}: the 3D path did not start: {}", desc_.app_name, why.view());
                three_d_failed_ = true;
            }
        }
        render::Camera3D cam3;
        kit::Camera3DComponent cam3_info;
        // A capture frame shows the held tick itself (no interpolation), so captures repeat exactly.
        const f32 alpha = capture_due() ? 1.0f : static_cast<f32>(rc.alpha);
        bool draw_3d = three_d_ && !ui_preview_ && extract3d_.camera(world_, alpha, cam3, &cam3_info);
        if (three_d_ && !ui_preview_) draw_3d = view_override_3d(alpha, cam3) || draw_3d;
        update_mouse_look(draw_3d && cam3_info.mouse_look != 0);
        rhi::CommandBuffer cmd(mem::frame());
        if (draw_3d) {
            render::Scene3D scene3(renderer_.meshes(), mem::frame());
            scene3.begin(cam3, bb);
            render::LodSettings lod;
            lod.enabled = cv_lod.get_bool();
            lod.pixel_error = math::max(cv_lod_pixels.get_float(), 0.01f);
            lod.force_level = cv_lod_force.get_int() < 0 ? ~0u : static_cast<u32>(cv_lod_force.get_int());
            scene3.set_lod(lod);
            extract3d_.extract(world_, scene3, alpha);
            scene3.build();
            renderer_.record_3d(cmd, scene3);
            stats3d_ = scene3.stats();
        }

        render::SpriteBatch batch(renderer_.sprite_sets(), mem::frame());
        batch.begin(render::compute_view_2d(cam, bb, ppm));
        if (!ui_preview_) extract_.extract(world_, batch);  // -ui_preview: the document alone
        batch.build();

        // With 2D post on, the world renders offscreen and the post pass writes the backbuffer (2D
        // games only: a 3D frame has its own post chain).
        render::Post2D post;
        const bool use_post = !draw_3d && !ui_preview_ && cv_post.get_bool() && extract_.post_2d(world_, post);
        if (use_post) ensure_post_target(bb);
        rhi::PassAttachment color;
        color.texture = use_post ? post_target_ : rhi::TextureHandle{};
        if (draw_3d) color.load = rhi::LoadOp::Load;
        color.clear.color[0] = clear.r;
        color.clear.color[1] = clear.g;
        color.clear.color[2] = clear.b;
        color.clear.color[3] = clear.a;
        rhi::RenderPassDesc pass;
        pass.colors = Span<const rhi::PassAttachment>(&color, 1);
        pass.debug_name = "player.world";
        cmd.begin_pass(pass);
        renderer_.record_2d(cmd, batch);
        if (use_post) {
            cmd.end_pass();
            renderer_.record_post_2d(cmd, post_target_, bb, post);
            if (overlay) {
                rhi::PassAttachment over;
                over.load = rhi::LoadOp::Load;
                rhi::RenderPassDesc op;
                op.colors = Span<const rhi::PassAttachment>(&over, 1);
                op.debug_name = "player.overlay";
                cmd.begin_pass(op);
                record_overlay(cmd, bb);
                cmd.end_pass();
            }
        } else {
            if (overlay) record_overlay(cmd, bb);
            cmd.end_pass();
        }
        device_->submit(cmd);

        // The run ends on the Nth frame (-frames) or on the first frame after the last capture tick
        // (-capture_tick / -capture_ticks): those catch moments of the simulation however fast or slow
        // the frames come.
        const bool frames_done = cv_frames.get_int() > 0 && rc.frame_index + 1 >= static_cast<u64>(cv_frames.get_int());
        bool last = frames_done;
        if (capture_due()) {
            const u64 tick = capture_ticks_[capture_next_++];
            if (!cv_capture.get_string().empty()) capture(capture_path(tick).c_str(), bb);
            last = last || capture_next_ == capture_ticks_.size();
        } else if (frames_done && capture_ticks_.empty() && !cv_capture.get_string().empty()) {
            capture(cv_capture.get_string().data(), bb);
        }
        device_->end_frame();
        assets_.end_frame();
        if (last) quit(0);
    }

    void on_shutdown() override {
        // A clean quit flushes the save before anything is torn down (FR-RUN-060).
        write_save();
        if (cv_frame_report.get_int() > 0) report_run_frames();
        if (liveops_on_) liveops_.shutdown();
        if (game_services_on_) game_services_.shutdown();
        if (three_d_) {  // the last frame's 3D load (tests and LOD measurements read it)
            const u32* lt = stats3d_.lod_triangles;
            std::printf("%s: 3D last frame %u triangles (LOD %u/%u/%u/%u), %u draws\n", desc_.app_name, stats3d_.triangles,
                        lt[0], lt[1], lt[2], lt[3], stats3d_.visible);
        }
        set_muted(false);
        if (window_) window_->set_event_handler(nullptr, nullptr);
        if (device_ && !post_target_.is_null()) device_->destroy_texture(post_target_);
        if (window_) window_->set_relative_mouse(false);
        clips_.release_all();
        extract3d_.shutdown();
        extract_.shutdown();
        if (device_) renderer_.shutdown();
    }

    bool pump_events() override {
        const bool quit_requested = platform::pump_events();
        if (os_background_) platform::sleep_ms(50);  // nothing to draw: do not spin the battery down
        return quit_requested;
    }
    void present(bool vsync) override {
        if (!os_background_) device_->present(vsync);
    }

    u64 state_hash() { return world_.state_hash(); }
    input::InputSystem& input() { return input_; }

    // -input's script, read by the sim's own tick count: a capture hold, a pause or an ad holds
    // the sim while the app's ticks run on, and sampling by app tick would then shift the rest of
    // the script (render_sheet's run would no longer play the run_headless one).
    class ScriptBySimTick final : public app::IInputSource {
    public:
        ScriptBySimTick(PlayerApp& app, input::InputScript* script) : app_(app), script_(script) {}
        const input::InputState* sample_for_tick(u64, u64 tick_close_us) override {
            if (app_.sim_held()) return &script_->state();  // not published: on_tick returns early
            return script_->sample_for_tick(app_.sim_ticks_ - app_.run_start_tick_ + 1, tick_close_us);
        }

    private:
        PlayerApp& app_;
        input::InputScript* script_;
    };
    // on_tick will not step the sim this tick (the same tests, in the same order).
    bool sim_held() const { return paused_ || ad_paused_ || capture_due(); }
    u64 ticks() const { return sim_ticks_; }
    usize entity_count() const { return entities_.size(); }
    const scene::StreamingStats* streaming_stats() {
        const scene::SceneStreaming* s = world_.try_resource<scene::SceneStreaming>();
        return s ? &s->stats() : nullptr;
    }

private:
    // Data mods (ADR-0034): resolves the mods folder and mounts the mods ahead of the game's pack,
    // the last in load order first, since AssetSystem lets earlier mounts win. A mod that cannot be
    // read or mounted is skipped with a warning; the game always starts.
    void mount_mods() {
        if (!desc_.mods_dir) return;
        if (!cv_mods.get_bool()) {
            MCE_LOG_INFO("{}: mods off (-mods 0)", desc_.app_name);
            return;
        }
        FixedString<1024> dir(cv_mods_dir.get_string().empty() ? StringView(desc_.mods_dir) : cv_mods_dir.get_string());
        const bool absolute = !dir.empty() && (dir.view()[0] == '/' || dir.view()[0] == '\\' || (dir.size() > 1 && dir.view()[1] == ':'));
        if (!absolute && cv_mods_dir.get_string().empty()) {  // the game's folder sits beside the executable
            FixedString<1024> full(platform::base_path());
            full.append(dir.view());
            dir = full;
        }
        if (dir.truncated()) return;
        if (dir.view().back() != '/' && dir.view().back() != '\\') dir.append("/");

        Array<FixedString<256>> names;
        platform::list_files(dir.view(), names);
        Array<FixedString<1024>> paths;
        bool has_order = false;
        for (const FixedString<256>& n : names) {
            if (n.view() == "load_order.txt") has_order = true;
            if (n.size() < 5 || n.view().substr(n.size() - 4) != ".mce") continue;
            FixedString<1024> p(dir.view());
            p.append(n.view());
            if (!p.truncated()) paths.push_back(p);
        }
        Array<StringView> views;
        for (const FixedString<1024>& p : paths) views.push_back(p.view());
        Array<byte> order_text;
        StringView order;
        if (has_order) {
            FixedString<1024> p(dir.view());
            p.append("load_order.txt");
            if (asset::read_file(p.view(), order_text)) order = StringView(reinterpret_cast<const char*>(order_text.data()), order_text.size());
        }
        asset::ModList found = asset::resolve_mods(views.span(), has_order ? &order : nullptr,
                                                   desc_.mods_game_id ? StringView(desc_.mods_game_id) : StringView());
        for (const asset::ModSkipped& s : found.skipped) MCE_LOG_WARN("{}: mod skipped: {} ({})", desc_.app_name, s.what.view(), s.reason.view());
        for (usize i = found.mods.size(); i-- > 0;) {
            const asset::ModEntry& m = found.mods[i];
            const StringView root[1] = {m.path.view()};
            if (Result<void> r = assets_.mount(Span<const StringView>(root, 1)); !r) {
                FixedString<512> msg;
                format_error(r.error(), msg);
                MCE_LOG_WARN("{}: mod skipped: {} (cannot mount: {})", desc_.app_name, m.path.view(), msg.view());
                asset::ModSkipped s;
                s.what.assign(m.path.view());
                s.reason.assign("cannot mount the pack");
                found.skipped.push_back(s);
                continue;
            }
            mods_.mods.push_back(m);
        }
        std::reverse(mods_.mods.begin(), mods_.mods.end());  // back to load order
        mods_.skipped = std::move(found.skipped);
        for (const asset::ModEntry& m : mods_.mods)
            MCE_LOG_INFO("{}: mod '{}' {} ({} assets) from {}", desc_.app_name, m.name.view(), m.version.view(), m.asset_count, m.path.view());
        if (!mods_.mods.empty()) std::printf("%s: %zu mod(s) loaded\n", desc_.app_name, mods_.mods.size());
    }

    // A scene change the game asked for this tick (scene::request_change): the new scene replaces
    // every entity and runs Startup again. A target the pack lacks ends the run (exit 1).
    void apply_scene_change() {
        Result<bool> changed = scene::apply_change(world_, assets_, &entities_);
        if (!changed) {
            FixedString<512> why;
            format_error(changed.error(), why);
            MCE_LOG_ERROR("{}: the scene change failed: {}", desc_.app_name, why.view());
            std::fprintf(stderr, "%s: the scene change failed: %s\n", desc_.app_name, why.c_str());
            quit(1);
            return;
        }
        if (!changed.value()) return;
        const scene::SceneChange& c = world_.resource<scene::SceneChange>();
        scene_guid_ = c.current_guid;
        scene_path_.assign(c.current.view());
        scene::pump_streaming(world_, scene::PumpMode::Settle);
        if (Result<void> r = load_ui_screens(); !r) {
            FixedString<512> why;
            format_error(r.error(), why);
            MCE_LOG_ERROR("{}: the new scene's UI screens did not load: {}", desc_.app_name, why.view());
        }
        if (device_ && kit::has_camera_3d(world_))
            if (Result<void> r = init_3d(); !r) MCE_LOG_ERROR("{}: 3D rendering did not start after the scene change", desc_.app_name);
    }

    // A GUID, a source path ("scenes/level.mcscene"), or empty for the first scene by path.
    Result<asset::Guid> pick_scene(StringView want) {
        if (!want.empty()) {
            if (Result<asset::Guid> g = asset::parse_guid(want)) {
                scene_path_.assign(assets_.info(assets_.find(g.value())).path);
                return g.value();
            }
        }
        asset::AssetKey best = asset::kInvalidAssetKey;
        for (asset::AssetKey k = 0; k < assets_.asset_count(); ++k) {
            const asset::AssetInfo info = assets_.info(k);
            if (info.type != asset::ChunkType::Scene) continue;
            if (!want.empty()) {
                if (info.path == want) best = k;
                continue;
            }
            if (best == asset::kInvalidAssetKey || info.path < assets_.info(best).path) best = k;
        }
        if (best == asset::kInvalidAssetKey) return Error(Errc::NotFound, "mce_player: no such scene in the pack", want);
        scene_path_.assign(assets_.info(best).path);
        return assets_.info(best).guid;
    }

    // The project's action maps (FR-INP-020): a GUID or source path, else the first cooked
    // .mcinput in the pack by path, else the built-in default map. A named map that is missing or
    // does not load is an error, never a silent fallback.
    Result<void> load_input_map(StringView want) {
        asset::AssetKey best = asset::kInvalidAssetKey;
        if (Result<asset::Guid> g = asset::parse_guid(want)) {
            best = assets_.find(g.value());
        } else {
            for (asset::AssetKey k = 0; k < assets_.asset_count(); ++k) {
                const asset::AssetInfo info = assets_.info(k);
                if (info.type != asset::ChunkType::InputMap) continue;
                if (!want.empty()) {
                    if (info.path == want) best = k;
                    continue;
                }
                if (best == asset::kInvalidAssetKey || info.path < assets_.info(best).path) best = k;
            }
        }
        if (best == asset::kInvalidAssetKey) {
            if (!want.empty()) return Error(Errc::NotFound, "mce_player: no such input map in the pack", want);
            Array<byte> map;
            input::default_map_chunk(map);
            return input_.load_map(map.span());
        }
        MCE_TRY_ASSIGN(const asset::AssetHandle<asset::InputMap> h, assets_.load_sync<asset::InputMap>(best));
        const Result<void> r = input_.load_map(assets_.resolve(h));
        assets_.release(asset::AnyHandle(h));
        MCE_TRY(r);
        MCE_LOG_INFO("{}: input map {}", desc_.app_name, assets_.info(best).path);
        return {};
    }

    // ---- languages (M-MOBILE step 11a) ----------------------------------------------------------

    // Every string table in the pack (assets/loc/<locale>.csv) goes into one Localization resource:
    // the saved choice first, then -language for this run, then the device languages, then
    // -default_language. A choice the game makes later (Localization::set_locale) is saved at once.
    Result<void> init_localization() {
        loc::Localization l10n;
        for (asset::AssetKey k = 0; k < assets_.asset_count(); ++k) {
            if (assets_.info(k).type != asset::ChunkType::StringTable) continue;
            MCE_TRY_ASSIGN(const asset::AssetHandle<asset::StringTable> h, assets_.load_sync<asset::StringTable>(k));
            loc::StringTable table;
            const Result<void> r = loc::decode_table(assets_.resolve(h), table);
            assets_.release(asset::AnyHandle(h));
            MCE_TRY(r);
            l10n.add_table(std::move(table));
        }
        if (l10n.tables().empty()) return ok();
        l10n.set_default_locale(cv_default_language.get_string());
        Array<FixedString<16>> device;
        platform::preferred_locales(device);
        l10n.set_preferred(device.span());
        if (Result<void> r = l10n.load_choice(); !r) MCE_LOG_WARN("{}: the saved language choice is unreadable", desc_.app_name);
        if (const StringView lang = cv_language.get_string(); !lang.empty() && !l10n.set_locale(lang))
            MCE_LOG_WARN("{}: no string table for -language {}", desc_.app_name, lang);
        l10n.persist_choice();
        MCE_LOG_INFO("{}: language {} ({} string tables)", desc_.app_name, l10n.locale(), l10n.tables().size());
        world_.set_resource(std::move(l10n));
        return ok();
    }

    // ---- live services (M-LIVEOPS) ----------------------------------------------------------------

    Result<void> init_liveops() {
        const StringView mode = cv_liveops_consent.get_string();
        if (mode == "granted") liveops::set_consent(liveops::Consent::Granted);
        else if (mode == "denied") liveops::set_consent(liveops::Consent::Denied);
        else if (mode != "auto")
            return Error(Errc::InvalidArgument, "player: -liveops.consent wants auto, granted or denied", mode);
        liveops_consent_auto_ = mode == "auto";

        liveops::ServiceDesc d;
        d.analytics = desc_.analytics_backend;
        d.crash_uploader = desc_.crash_uploader;
        const StringView provider = cv_liveops_provider.get_string();
        // Firebase: asked for, or the default on a phone build that has it and a host that brought
        // no backend of its own. Crashlytics catches native crashes itself (the engine's handler
        // hands them on), so the engine's own reports stay on the device.
        const bool firebase = provider == "firebase" || (provider.empty() && !d.analytics && !d.crash_uploader &&
                                                         platform::firebase::backend() ==
                                                             platform::firebase::Backend::Firebase);
        if (firebase) {
            if (platform::firebase::start()) {
                d.analytics = &firebase_analytics_;
                d.crash_uploader = nullptr;
                firebase_on_ = true;
                firebase_collection_ = ~0u;
                MCE_LOG_INFO("{}: live services vendor firebase (Crashlytics, Analytics)", desc_.app_name);
            } else {
                MCE_LOG_WARN("{}: -liveops.provider firebase, but this build has no Firebase; events are dropped",
                             desc_.app_name);
                d.analytics = nullptr;
            }
        } else if (provider == "local") {
            FixedString<512> events(save::root());
            events.append("/analytics.jsonl");
            FixedString<512> outbox(save::root());
            outbox.append("/crash_outbox");
            local_analytics_.emplace(events.view());
            local_outbox_.emplace(outbox.view());
            d.analytics = &*local_analytics_;
            d.crash_uploader = &*local_outbox_;
        } else if (provider == "none") {
            d.analytics = nullptr;
            d.crash_uploader = nullptr;
        } else if (!provider.empty()) {
            return Error(Errc::InvalidArgument, "player: -liveops.provider wants firebase, local or none", provider);
        }
        // Where the crash handler writes (-crash_dir, or the per-user folder), also when it is off this
        // run (-crash_reports 0, or a sanitizer build): reports from earlier runs still wait there.
        if (crash::installed()) d.crash_dir.assign(crash::dump_dir());
        else if (!samples::g_cv_crash_dir.get_string().empty()) d.crash_dir.assign(samples::g_cv_crash_dir.get_string());
        else d.crash_dir.assign(crash::default_dump_dir(desc_.app_name).view());
        liveops_.init(d);
        MCE_TRY(init_remote_config());
        return init_game_services();
    }

    Result<void> init_game_services() {
        liveops::ServicesCatalog catalog;
        bool found = false;
        for (asset::AssetKey k = 0; k < assets_.asset_count() && !found; ++k) {
            const asset::AssetInfo info = assets_.info(k);
            if (info.type != asset::ChunkType::GameServicesCatalog) continue;
            MCE_TRY_ASSIGN(const asset::AssetHandle<asset::GameServicesCatalogChunk> h,
                           assets_.load_sync<asset::GameServicesCatalogChunk>(k));
            const Result<void> r = liveops::decode_services_catalog(assets_.resolve(h), catalog);
            assets_.release(asset::AnyHandle(h));
            MCE_TRY(r);
            found = true;
        }
        if (!parse_game_services_test(cv_game_services_test.get_string()))
            return Error(Errc::InvalidArgument, "player: -game_services_test wants \"tick:action[:id[:value]] ...\"",
                         cv_game_services_test.get_string());
        if (!found) {
            if (!game_services_test_.empty())
                return Error(Errc::InvalidArgument, "player: -game_services_test needs a .mcservices in the pack");
            return ok();
        }
        liveops::IGameServicesProvider* provider = desc_.game_services_provider;
        const StringView mode = cv_liveops_provider.get_string();
        if (mode == "local") {
            FixedString<512> dir(cv_game_services_dir.get_string());
            if (dir.empty()) {
                dir.assign(save::root().view());
                dir.append("/game_services_platform");
            }
            local_services_.emplace(dir.view());
            local_services_->set_offline(cv_game_services_offline.get_bool());
            provider = &*local_services_;
        } else if (mode == "none") {
            provider = nullptr;
        }
        game_services_.init(catalog, provider);
        game_services_.install(world_);
        game_services_on_ = true;
        game_services_print_ = !game_services_test_.empty() || mode == "local";
        return ok();
    }

    // -game_services_test: "tick:action[:id[:value]]" steps.
    struct ServicesStep {
        u64 tick = 0;
        FixedString<16> action;
        FixedString<64> id;
        FixedString<64> value;
    };

    bool parse_game_services_test(StringView v) {
        game_services_test_.clear();
        usize i = 0;
        while (i < v.size()) {
            while (i < v.size() && v[i] == ' ') ++i;
            usize end = i;
            while (end < v.size() && v[end] != ' ') ++end;
            if (end == i) break;
            StringView tok = v.substr(i, end - i);
            i = end;
            StringView parts[4];
            u32 n = 0;
            while (n < 4) {
                usize c = 0;
                while (c < tok.size() && tok[c] != ':') ++c;
                parts[n++] = tok.substr(0, c);
                if (c == tok.size()) break;
                tok = tok.substr(c + 1);
            }
            ServicesStep step;
            if (n < 2 || parts[0].empty()) return false;
            for (const char c : parts[0]) {
                if (c < '0' || c > '9') return false;
                step.tick = step.tick * 10 + static_cast<u64>(c - '0');
            }
            step.action = parts[1];
            if (n > 2) step.id = parts[2];
            if (n > 3) step.value = parts[3];
            const StringView a = step.action.view();
            const bool needs_value = a == "progress" || a == "score" || a == "write";
            const bool needs_id = needs_value || a == "unlock" || a == "cloud_save" || a == "cloud_load" ||
                                  a == "keep_local" || a == "keep_cloud";
            if (!needs_id && a != "sign_in") return false;
            if (needs_id == step.id.empty() || needs_value == step.value.empty()) return false;
            game_services_test_.push_back(step);
        }
        return true;
    }

    static i64 parse_i64(StringView s) {
        i64 v = 0;
        const bool neg = !s.empty() && s[0] == '-';
        for (usize k = neg ? 1 : 0; k < s.size(); ++k) v = v * 10 + (s[k] - '0');
        return neg ? -v : v;
    }

    void game_services_test_requests() {
        liveops::GameServices& gs = world_.resource<liveops::GameServices>();
        for (const ServicesStep& s : game_services_test_) {
            if (s.tick != sim_ticks_) continue;
            const StringView a = s.action.view();
            i64 r = 0;
            if (a == "sign_in") r = gs.sign_in();
            else if (a == "unlock") r = gs.unlock(s.id.view());
            else if (a == "progress") r = gs.add_progress(s.id.view(), static_cast<u32>(parse_i64(s.value.view())));
            else if (a == "score") r = gs.submit_score(s.id.view(), parse_i64(s.value.view()));
            else if (a == "cloud_save") r = gs.cloud_save(s.id.view());
            else if (a == "cloud_load") r = gs.cloud_load(s.id.view());
            else if (a == "keep_local") r = gs.resolve_conflict(s.id.view(), true);
            else if (a == "keep_cloud") r = gs.resolve_conflict(s.id.view(), false);
            else if (a == "write")
                r = save::write(s.id.view(), Span<const byte>(reinterpret_cast<const byte*>(s.value.c_str()), s.value.size()))
                        ? 1
                        : 0;
            std::printf("mce_player: game_services tick %llu: %s %s %s -> %lld\n", static_cast<unsigned long long>(sim_ticks_),
                        s.action.c_str(), s.id.c_str(), s.value.c_str(), static_cast<long long>(r));
        }
    }

    // What entered this tick, as a game would see it (-game_services_test, or the local provider); a CloudLoaded
    // save is printed as text.
    void game_services_test_events() {
        const liveops::GameServices& gs = world_.resource<liveops::GameServices>();
        const liveops::ServicesCatalog& c = gs.catalog();
        auto* q = world_.event_queue<liveops::GameServicesEvent>();
        if (!q) return;
        for (const liveops::GameServicesEvent& e : q->read()) {
            const char* id = "-";
            switch (e.kind) {
                case liveops::GameServicesEventKind::AchievementSent:
                    if (e.index < c.achievements().size()) id = c.achievements()[e.index].id.c_str();
                    break;
                case liveops::GameServicesEventKind::ScoreSent:
                    if (e.index < c.leaderboards().size()) id = c.leaderboards()[e.index].id.c_str();
                    break;
                case liveops::GameServicesEventKind::SignedIn:
                case liveops::GameServicesEventKind::SignedOut: break;
                default:
                    if (e.index < c.cloud_saves().size()) id = c.cloud_saves()[e.index].c_str();
                    break;
            }
            std::printf("mce_player: game_services tick %llu: %s %s value %lld\n", static_cast<unsigned long long>(sim_ticks_),
                        liveops::game_services_event_name(e.kind), id, static_cast<long long>(e.value));
            if (e.kind == liveops::GameServicesEventKind::CloudLoaded) {
                Array<byte> b;
                if (save::read(id, b))
                    std::printf("mce_player: game_services: %s is now \"%.*s\"\n", id, static_cast<int>(b.size()),
                                reinterpret_cast<const char*>(b.data()));
            }
        }
    }

    Result<void> init_remote_config() {
        liveops::ConfigValues defaults;
        for (asset::AssetKey k = 0; k < assets_.asset_count(); ++k) {
            const asset::AssetInfo info = assets_.info(k);
            if (info.type != asset::ChunkType::RemoteConfig) continue;
            MCE_TRY_ASSIGN(const asset::AssetHandle<asset::RemoteConfigChunk> h,
                           assets_.load_sync<asset::RemoteConfigChunk>(k));
            const Result<void> r = liveops::decode_config(assets_.resolve(h), defaults);
            assets_.release(asset::AnyHandle(h));
            MCE_TRY(r);
            MCE_LOG_INFO("{}: remote config defaults {}", desc_.app_name, info.path);
            break;
        }
        liveops::IConfigFetcher* fetcher = desc_.config_fetcher;
        if (!cv_remote_config_file.get_string().empty()) {
            file_fetcher_.emplace(cv_remote_config_file.get_string());
            fetcher = &*file_fetcher_;
        }
        if (!fetcher && firebase_on_) {
            firebase_config_.emplace(defaults);
            fetcher = &*firebase_config_;
            MCE_LOG_INFO("{}: remote config fetched from Firebase Remote Config", desc_.app_name);
        }
        if (cv_liveops_provider.get_string() == "none") fetcher = nullptr;
        remote_config_.init(defaults, fetcher);
        remote_config_.install(world_);
        dumped_config_version_ = ~0u;
        return ok();
    }

    void dump_remote_config() {
        if (!cv_remote_config_dump.get_bool() || remote_config_.version() == dumped_config_version_) return;
        dumped_config_version_ = remote_config_.version();
        for (const liveops::ConfigValue& v : remote_config_.active().values()) {
            switch (v.type) {
                case liveops::ConfigType::Int:
                    std::printf("remote_config v%u: %s = %lld\n", dumped_config_version_, v.key.c_str(),
                                static_cast<long long>(v.i));
                    break;
                case liveops::ConfigType::Float:
                    std::printf("remote_config v%u: %s = %g\n", dumped_config_version_, v.key.c_str(), v.f);
                    break;
                case liveops::ConfigType::Bool:
                    std::printf("remote_config v%u: %s = %s\n", dumped_config_version_, v.key.c_str(),
                                v.i ? "true" : "false");
                    break;
                case liveops::ConfigType::String:
                    std::printf("remote_config v%u: %s = \"%s\"\n", dumped_config_version_, v.key.c_str(), v.s.c_str());
                    break;
            }
        }
    }

    void update_liveops() {
        // The ad network's consent form decides for every service unless the game or -liveops.consent did.
        if (liveops_consent_auto_ && liveops::consent() == liveops::Consent::Unknown) {
            const platform::ads::Consent ads = platform::ads::consent();
            if (ads == platform::ads::Consent::Personalized) liveops::set_consent(liveops::Consent::Granted);
            else if (ads == platform::ads::Consent::NonPersonalized) liveops::set_consent(liveops::Consent::Denied);
        }
        if (firebase_on_) {
            const liveops::Consent c = liveops::consent();
            const platform::firebase::Collection col = c == liveops::Consent::Granted ? platform::firebase::Collection::Granted
                                                       : c == liveops::Consent::Denied
                                                           ? platform::firebase::Collection::Denied
                                                           : platform::firebase::Collection::Undecided;
            if (static_cast<u32>(col) != firebase_collection_) {
                firebase_collection_ = static_cast<u32>(col);
                platform::firebase::set_collection(col);
            }
        }
        liveops_.update();
        remote_config_.update();
        if (game_services_on_) game_services_.update();
        dump_remote_config();
    }

    // ---- purchases and ads (M-MONETIZE) ---------------------------------------------------------

    Result<void> init_monetize(StringView want) {
        monetize::Catalog catalog;
        asset::AssetKey best = asset::kInvalidAssetKey;
        if (Result<asset::Guid> g = asset::parse_guid(want)) {
            best = assets_.find(g.value());
        } else {
            for (asset::AssetKey k = 0; k < assets_.asset_count(); ++k) {
                const asset::AssetInfo info = assets_.info(k);
                if (info.type != asset::ChunkType::StoreCatalog) continue;
                if (!want.empty()) {
                    if (info.path == want) best = k;
                    continue;
                }
                if (best == asset::kInvalidAssetKey || info.path < assets_.info(best).path) best = k;
            }
        }
        if (best != asset::kInvalidAssetKey) {
            MCE_TRY_ASSIGN(const asset::AssetHandle<asset::StoreCatalog> h, assets_.load_sync<asset::StoreCatalog>(best));
            const Result<void> r = monetize::decode_catalog(assets_.resolve(h), catalog);
            assets_.release(asset::AnyHandle(h));
            MCE_TRY(r);
            MCE_LOG_INFO("{}: store catalog {}", desc_.app_name, assets_.info(best).path);
        } else if (!want.empty()) {
            return Error(Errc::NotFound, "mce_player: no such store catalog in the pack", want);
        } else {
            MCE_LOG_WARN("{}: purchases and ads are on but the pack has no store catalog (.mcstore)", desc_.app_name);
        }
        monetize::IStoreProvider* store = desc_.store_provider;
        monetize::IAdProvider* ads = desc_.ad_provider;
        const StringView provider = cv_monetize_provider.get_string();
        if (provider == "fake") {
            monetize::FakeConfig cfg;
            if (!monetize::fake_config_from_convars(cfg))
                return Error(Errc::InvalidArgument, "player: a -monetize.fake.* value is not one it knows (see -help)");
            fake_store_.set_config(cfg);
            fake_ads_.set_config(cfg);
            store = &fake_store_;
            ads = &fake_ads_;
        } else if (provider == "none") {
            store = nullptr;
            ads = nullptr;
        } else if (!provider.empty()) {
            return Error(Errc::InvalidArgument, "player: -monetize.provider wants fake or none", provider);
        } else {
            if (!store && platform_store_.available()) store = &platform_store_;  // Play Billing / StoreKit 2
            if (!ads && platform_ads_.available()) ads = &platform_ads_;          // Google Mobile Ads
        }
        MCE_TRY(monetize_.init(catalog, store, ads));
        monetize_.install(world_);
        if (!parse_monetize_test(cv_monetize_test.get_string()))
            return Error(Errc::InvalidArgument, "player: -monetize_test wants \"tick:action[:id] ...\"", cv_monetize_test.get_string());
        if (!monetize_test_.empty()) {
            const monetize::Store& st = world_.resource<monetize::Store>();
            for (const monetize::ProductInfo& p : st.products())
                if (st.owns(p.id.view())) std::printf("mce_player: monetize: owns %s\n", p.id.c_str());
        }
        return ok();
    }

    // -monetize_test: "tick:action[:id]" steps, sorted by tick as given.
    struct MonetizeStep {
        u64 tick = 0;
        FixedString<16> action;
        FixedString<64> id;
    };

    bool parse_monetize_test(StringView v) {
        monetize_test_.clear();
        usize i = 0;
        while (i < v.size()) {
            while (i < v.size() && v[i] == ' ') ++i;
            usize end = i;
            while (end < v.size() && v[end] != ' ') ++end;
            if (end == i) break;
            const StringView tok = v.substr(i, end - i);
            i = end;
            usize c1 = 0;
            while (c1 < tok.size() && tok[c1] != ':') ++c1;
            if (c1 == 0 || c1 == tok.size()) return false;
            MonetizeStep step;
            for (usize k = 0; k < c1; ++k) {
                if (tok[k] < '0' || tok[k] > '9') return false;
                step.tick = step.tick * 10 + static_cast<u64>(tok[k] - '0');
            }
            StringView rest = tok.substr(c1 + 1);
            usize c2 = 0;
            while (c2 < rest.size() && rest[c2] != ':') ++c2;
            step.action = rest.substr(0, c2);
            if (c2 < rest.size()) step.id = rest.substr(c2 + 1);
            const StringView a = step.action.view();
            const bool needs_id = a == "buy" || a == "load" || a == "show" || a == "banner";
            const bool known = needs_id || a == "query" || a == "restore" || a == "hide";
            if (!known || needs_id == step.id.empty()) return false;
            monetize_test_.push_back(step);
        }
        return true;
    }

    void monetize_test_requests() {
        monetize::Store& store = world_.resource<monetize::Store>();
        monetize::Ads& ads = world_.resource<monetize::Ads>();
        for (const MonetizeStep& s : monetize_test_) {
            if (s.tick != sim_ticks_) continue;
            const StringView a = s.action.view();
            u32 req = 0;
            if (a == "query") req = store.query_products();
            else if (a == "buy") req = store.purchase(s.id.view());
            else if (a == "restore") req = store.restore_purchases();
            else if (a == "load") req = ads.load(s.id.view());
            else if (a == "show") req = ads.show(s.id.view());
            else if (a == "banner") req = ads.show_banner(s.id.view());
            else if (a == "hide") ads.hide_banner();
            std::printf("mce_player: monetize tick %llu: %s %s -> request %u\n", static_cast<unsigned long long>(sim_ticks_),
                        s.action.c_str(), s.id.c_str(), req);
        }
    }

    // What entered this tick, as a game would see it; consumables are granted (finished) at once.
    void monetize_test_events() {
        monetize::Store& store = world_.resource<monetize::Store>();
        const monetize::Ads& ads = world_.resource<monetize::Ads>();
        const unsigned long long tick = static_cast<unsigned long long>(sim_ticks_);
        if (auto* q = world_.event_queue<monetize::StoreEvent>()) {
            for (const monetize::StoreEvent& e : q->read()) {
                const char* id = e.product < store.products().size() ? store.products()[e.product].id.c_str() : "-";
                std::printf("mce_player: monetize tick %llu: store %s %s request %u", tick, monetize::store_event_name(e.kind), id,
                            e.request);
                if (e.kind == monetize::StoreEventKind::ProductInfo) {
                    const StringView price = store.price(e.product);
                    std::printf(" price %.*s", static_cast<int>(price.size()), price.data());
                }
                std::printf("\n");
                if (e.kind == monetize::StoreEventKind::Purchased && e.product < store.products().size() &&
                    store.products()[e.product].type == monetize::ProductType::Consumable)
                    store.finish(e.transaction);
            }
        }
        if (auto* q = world_.event_queue<monetize::AdEvent>()) {
            for (const monetize::AdEvent& e : q->read()) {
                const char* id = e.placement < ads.placements().size() ? ads.placements()[e.placement].id.c_str() : "-";
                std::printf("mce_player: monetize tick %llu: ad %s %s request %u", tick, monetize::ad_event_name(e.kind), id,
                            e.request);
                if (e.kind == monetize::AdEventKind::Rewarded) std::printf(" reward %u", e.reward_amount);
                std::printf("\n");
            }
        }
    }

    // A fullscreen ad pauses the sim and the audio like the phone background (FR-RUN-100) and
    // resumes them when it closes; the pause overlay is not involved.
    void update_ad_pause() {
        const bool showing = monetize_.fullscreen_ad_showing();
        if (showing == ad_paused_) return;
        ad_paused_ = showing;
        if (showing) release_input();
        audio_.set_paused(showing || os_background_);
        MCE_LOG_INFO("{}: {}", desc_.app_name, showing ? "a fullscreen ad is up (sim and audio paused)" : "the ad closed (resumed)");
    }

    void set_safe_insets(const platform::SafeInsets& s) {
        safe_insets_ = s;
        if (monetize_on_) monetize_.set_safe_insets({s.left, s.top, s.right, s.bottom});
    }

    // ---- the developer console (FR-UI-045) ----------------------------------------------------

    // The UI scale for this framebuffer: the display's scale times -ui_scale, fitted (M-MOBILE 11b).
    f32 ui_scale(rhi::Extent2D bb) const { return ui::compute_ui_scale(bb, window_->display_scale(), cv_ui_scale.get_float()); }

    void set_console(bool open) {
        console_.set_open(open);
        window_->set_text_input(open);
    }

    void update_console(rhi::Extent2D bb) {
        ui::UpdateContext ctx;
        ctx.viewport = bb;
        ctx.safe_insets = ui::SafeInsets{static_cast<f32>(safe_insets_.left), static_cast<f32>(safe_insets_.top),
                                         static_cast<f32>(safe_insets_.right), static_cast<f32>(safe_insets_.bottom)};
        ctx.ui_scale = ui_scale(bb);
        input::InputState in;
        in.cancel_pressed = con_.cancel;
        ctx.actions = &in;
        ctx.text_input = con_.typed.view();
        ctx.key_backspace = con_.backspace;
        ctx.key_enter = con_.enter;
        ctx.key_up = con_.up;
        ctx.key_down = con_.down;
        ctx.dt = 1.0 / 60.0;
        const bool was_open = console_.is_open();
        console_.update(ctx);  // pulls the log ring even while closed
        if (was_open && !console_.is_open()) window_->set_text_input(false);  // closed with Esc
        con_ = ConsoleKeys{};
    }

    // Game UI layers (PlayerDesc::ui_layers): views of the world, updated every rendered frame.
    void update_ui_layers(rhi::Extent2D bb) {
        if (desc_.ui_layer_count == 0) return;
        ui::UpdateContext ctx;
        ctx.viewport = bb;
        ctx.safe_insets = ui::SafeInsets{static_cast<f32>(safe_insets_.left), static_cast<f32>(safe_insets_.top),
                                         static_cast<f32>(safe_insets_.right), static_cast<f32>(safe_insets_.bottom)};
        ctx.ui_scale = ui_scale(bb);
        ctx.dt = 1.0 / 60.0;
        for (u32 i = 0; i < desc_.ui_layer_count; ++i) {
            const ui::Layer* l = desc_.ui_layers[i];
            if (l && l->update) l->update(l->user, world_, ctx);
        }
    }

    // The scene's UI screens (kit::UiScreen, M-UI-DATA step 2).
    Result<void> load_ui_screens() {
        const ui::Skin* skin = device_ ? &skin_.skin : nullptr;
        kit::SpriteExtract* extract = device_ ? &extract_ : nullptr;
        if (const StringView preview = cv_ui_preview.get_string(); !preview.empty()) {
            const Result<asset::Guid> g = asset::parse_guid(preview);
            if (!g) return Error(Errc::InvalidArgument, "player: -ui_preview wants a UI document GUID", preview);
            ui_preview_ = true;
            return screens_.load_preview(world_, assets_, entities_.span(), g.value(), skin, extract);
        }
        MCE_TRY(screens_.load(world_, assets_, entities_.span(), skin, extract));
        return {};
    }

    void update_ui_screens(rhi::Extent2D bb) {
        if (!screens_.active()) return;
        // A visible modal screen owns the controls: the "UI" action map (Navigate / Confirm / Cancel)
        // blocks the game's map below it until the screen hides.
        const bool modal = screens_.modal_visible() && !console_.is_open();
        if (modal && !ui_map_pushed_ && !ui_map_missing_) {
            ui_map_pushed_ = input_.push_map(MCE_SID("UI"));
            ui_map_missing_ = !ui_map_pushed_;
            if (ui_map_missing_) MCE_LOG_WARN("{}: the input map has no \"UI\" map for the UI screens", desc_.app_name);
        } else if (!modal && ui_map_pushed_) {
            input_.pop_map();
            ui_map_pushed_ = false;
        }
        ui::UpdateContext ctx;
        ctx.viewport = bb;
        ctx.safe_insets = ui::SafeInsets{static_cast<f32>(safe_insets_.left), static_cast<f32>(safe_insets_.top),
                                         static_cast<f32>(safe_insets_.right), static_cast<f32>(safe_insets_.bottom)};
        ctx.ui_scale = ui_scale(bb);
        ctx.dt = 1.0 / 60.0;
        // Edges (Confirm, Cancel, a navigation press) once per tick, not once per rendered frame.
        input::InputState in = ui_actions_;
        if (in.has_action(MCE_SID("Navigate"))) {
            const math::vec2 nav = in.axis2(MCE_SID("Navigate"));
            in.move = nav;
        }
        if (!ui_fresh_) {
            in.confirm_pressed = in.cancel_pressed = in.pause_pressed = in.fire_pressed = in.jump_pressed = false;
            for (u32 i = 0; i < in.action_count; ++i) in.actions[i].pressed = in.actions[i].released = false;
        }
        ui_fresh_ = false;
        ctx.actions = console_.is_open() ? nullptr : &in;
        if (!console_.is_open()) {
            const platform::PixelSize pt = window_->size();
            const f32 sx = pt.width > 0 ? static_cast<f32>(bb.width) / static_cast<f32>(pt.width) : 1.0f;
            const f32 sy = pt.height > 0 ? static_cast<f32>(bb.height) / static_cast<f32>(pt.height) : 1.0f;
            if (ui_pointer_.touch) ctx.pointer = {ui_pointer_.pos.x * static_cast<f32>(bb.width), ui_pointer_.pos.y * static_cast<f32>(bb.height)};
            else if (ui_pointer_.pos.x >= 0.0f) ctx.pointer = {ui_pointer_.pos.x * sx, ui_pointer_.pos.y * sy};
            ctx.pointer_down = ui_pointer_.down;
            ctx.pointer_pressed = ui_pointer_.pressed;
            ctx.pointer_released = ui_pointer_.released;
        }
        ui_pointer_.pressed = ui_pointer_.released = false;
        screens_.update(world_, ctx);
    }

    void record_overlay(rhi::CommandBuffer& cmd, rhi::Extent2D bb) {
        render::SpriteBatch hud(renderer_.sprite_sets(), mem::frame());
        hud.begin(render::compute_screen_view(bb), render::PassId::Screen2D);
        screens_.submit(hud, -120);  // the scene's UI screens, under the game's UI layers
        // Game UI under the pause veil (sort layer 0) and the console (200).
        for (u32 i = 0; i < desc_.ui_layer_count; ++i) {
            const ui::Layer* l = desc_.ui_layers[i];
            if (l && l->submit) l->submit(l->user, hud, static_cast<i16>(-40 + 4 * static_cast<i32>(i)));
        }
        if (paused_) add_pause_overlay(hud, bb);
        if (cv_stats.get_bool()) add_stats_line(hud);
        console_.submit(hud, 200);
        hud.build();
        renderer_.record_2d(cmd, hud);
    }

    void add_pause_overlay(render::SpriteBatch& hud, rhi::Extent2D bb) {
        render::SpriteInstance dim;  // a translucent veil over the frozen frame
        dim.set = extract_.sprite_set(asset::Guid{});
        dim.position = {0.5f * static_cast<f32>(bb.width), 0.5f * static_cast<f32>(bb.height)};
        dim.scale = {static_cast<f32>(bb.width), static_cast<f32>(bb.height)};
        dim.tint = Color{0.0f, 0.0f, 0.0f, 0.55f};
        hud.add(dim);
        render::TextLayout layout(mem::frame());
        render::TextLayoutDesc desc;
        desc.size_px = 32;
        desc.align = render::TextAlign::Center;
        render::layout_text(font_.face, "PAUSED\nESC OR TAP RESUME   Q QUIT", desc, layout);
        render::TextPlacement p;
        p.origin = {0.5f * (static_cast<f32>(bb.width) - layout.width), 0.5f * (static_cast<f32>(bb.height) - layout.height)};
        p.sort_order = 1;
        hud.add_text(font_, layout, p);
    }

    // F3 / -r.stats 1: what the frame drew (3D counts on the 3D path).
    void add_stats_line(render::SpriteBatch& hud) {
        char line[256];
        if (three_d_) {
            const kit::Kit3DStats& k = extract3d_.stats();
            const u32* lt = stats3d_.lod_triangles;
            std::snprintf(line, sizeof line,
                          "3D  TRIS %u (LOD %u/%u/%u/%u)  DRAWS %u/%u  MESHES %u  INST %u  LIGHTS %u  ANIM %u  MISSING %u",
                          stats3d_.triangles, lt[0], lt[1], lt[2], lt[3], stats3d_.visible, stats3d_.submeshes, k.meshes,
                          k.instances, stats3d_.point_lights, k.animated, k.missing);
        } else {
            std::snprintf(line, sizeof line, "2D");
        }
        render::TextLayout layout(mem::frame());
        render::TextLayoutDesc desc;
        desc.size_px = 16;
        render::layout_text(font_.face, line, desc, layout);
        render::TextPlacement p;
        p.origin = {8.0f + static_cast<f32>(safe_insets_.left), 8.0f + static_cast<f32>(safe_insets_.top)};
        p.sort_order = 2;
        hud.add_text(font_, layout, p);
    }

    // The 3D path (M-KIT3D): the forward renderer and the kit3d extract, started once a scene has a
    // Camera3DComponent.
    Result<void> init_3d() {
        if (three_d_ || three_d_failed_) return {};
        // r.quality's tier sets the cost knobs; r.msaa caps its MSAA.
        const render::QualityTier tier = render::quality_tier();
        render::QualitySettings quality = render::quality_settings(tier);
        render::Forward3DDesc fd;
        render::apply_quality(quality, fd);
        fd.msaa = math::min(fd.msaa, static_cast<u32>(math::clamp<i64>(cv_msaa.get_int(), 1, 8)));
        MCE_TRY(renderer_.init_3d(fd));
        extract3d_.init(renderer_.meshes(), &assets_);
        // High caps no Environment3D cascade count (its 3 is only the scene default).
        if (tier == render::QualityTier::High) quality.shadow_cascades = render::kMaxCascades;
        extract3d_.set_quality(quality);
        if (cv_preload3d.get_bool()) MCE_TRY(extract3d_.preload());
        three_d_ = true;
        MCE_LOG_INFO("{}: 3D path on ({} quality, {}x MSAA)", desc_.app_name, render::quality_tier_name(tier), fd.msaa);
        return {};
    }

    // Relative mouse while a mouse-look camera plays; the cursor comes back for the pause overlay,
    // the console and an unfocused window.
    void update_mouse_look(bool camera_wants) {
        if (!window_) return;
        const bool want = camera_wants && cv_mouse_look.get_bool() && !paused_ && !console_.is_open() && !background_ &&
                          !os_background_;
        if (want == relative_mouse_) return;
        relative_mouse_ = want;
        if (!window_->set_relative_mouse(want) && want) MCE_LOG_WARN("{}: relative mouse mode refused", desc_.app_name);
    }

    void ensure_post_target(rhi::Extent2D size) {
        if (!post_target_.is_null() && post_size_.width == size.width && post_size_.height == size.height) return;
        if (!post_target_.is_null()) device_->destroy_texture(post_target_);
        rhi::TextureDesc td;
        td.size = size;
        td.usage = rhi::TextureUsage::SampledColorTarget;
        td.debug_name = "player.post2d_src";
        post_target_ = device_->create_texture(td);
        post_size_ = size;
    }

    // -capture_tick / -capture_ticks and -view.* / -safe_area, read once at start.
    ResultV parse_capture_and_view() {
        capture_ticks_.clear();
        if (const StringView list = cv_capture_ticks.get_string(); !list.empty()) {
            if (!parse_numbers(list, ticks_tmp_, 1, 64))
                return Error(Errc::InvalidArgument, "player: -capture_ticks wants ascending ticks \"60,120,300\" (1-64 of them)");
            for (usize i = 0; i < ticks_tmp_.size(); ++i) {
                const f64 t = ticks_tmp_[i];
                if (t < 1 || t != static_cast<f64>(static_cast<u64>(t)) || (i > 0 && t <= ticks_tmp_[i - 1]))
                    return Error(Errc::InvalidArgument, "player: -capture_ticks wants ascending whole ticks >= 1");
                capture_ticks_.push_back(static_cast<u64>(t));
            }
        } else if (cv_capture_tick.get_int() > 0) {
            capture_ticks_.push_back(static_cast<u64>(cv_capture_tick.get_int()));
        }
        view_free_.clear();
        if (const StringView v = cv_view_free.get_string(); !v.empty()) {
            if (!parse_numbers(v, view_free_, 2, 7) || view_free_.size() == 4 || view_free_.size() == 5)
                return Error(Errc::InvalidArgument,
                             "player: -view.free wants \"x,y[,zoom]\" (2D) or \"x,y,z,tx,ty,tz[,fov]\" (3D)");
        }
        safe_override_.clear();
        if (const StringView v = cv_safe_area.get_string(); !v.empty()) {
            if (!parse_numbers(v, safe_override_, 4, 4) || safe_override_[0] < 0 || safe_override_[1] < 0 ||
                safe_override_[2] < 0 || safe_override_[3] < 0)
                return Error(Errc::InvalidArgument, "player: -safe_area wants \"left,top,right,bottom\" in pixels (>= 0)");
        }
        return ok();
    }

    // Comma-separated numbers, between `min` and `max` of them.
    static bool parse_numbers(StringView text, Array<f64>& out, usize min, usize max) {
        out.clear();
        usize start = 0;
        while (start <= text.size()) {
            usize end = text.find(',', start);
            if (end == StringView::npos) end = text.size();
            char buf[48] = {};
            const StringView item = text.substr(start, end - start);
            if (item.empty() || item.size() >= sizeof(buf)) return false;
            std::memcpy(buf, item.data(), item.size());
            char* stop = nullptr;
            const f64 v = std::strtod(buf, &stop);
            if (stop == buf || *stop != 0) return false;
            out.push_back(v);
            start = end + 1;
        }
        return out.size() >= min && out.size() <= max;
    }

    bool capture_due() const {
        return capture_next_ < capture_ticks_.size() && sim_ticks_ - run_start_tick_ >= capture_ticks_[capture_next_];
    }

    // -capture's path for `tick`: {tick} replaced, else _<tick> before the extension when there are several ticks.
    FixedString<1024> capture_path(u64 tick) const {
        const StringView p = cv_capture.get_string();
        char num[24];
        std::snprintf(num, sizeof(num), "%llu", static_cast<unsigned long long>(tick));
        FixedString<1024> out;
        const usize at = p.find("{tick}");
        if (at != StringView::npos) {
            out.append(p.substr(0, at));
            out.append(num);
            out.append(p.substr(at + 6));
        } else if (capture_ticks_.size() > 1) {
            usize dot = p.rfind('.');
            const usize slash = p.find_last_of("/\\");
            if (dot == StringView::npos || (slash != StringView::npos && dot < slash)) dot = p.size();
            out.append(p.substr(0, dot));
            out.append("_");
            out.append(num);
            out.append(p.substr(dot));
        } else {
            out.append(p);
        }
        return out;
    }

    platform::SafeInsets window_insets() const {
        if (safe_override_.size() != 4) return window_->safe_insets();
        return {static_cast<u32>(safe_override_[0]), static_cast<u32>(safe_override_[1]), static_cast<u32>(safe_override_[2]),
                static_cast<u32>(safe_override_[3])};
    }

    // The scene entity -view.entity names, or null.
    Entity view_entity() const {
        const i64 i = cv_view_entity.get_int();
        return i >= 0 && static_cast<usize>(i) < entities_.size() ? entities_[static_cast<usize>(i)] : Entity{};
    }

    // -view.entity / -view.free on the 2D camera (render only).
    void view_override_2d(render::Camera2D& cam, f32& ppm, Color& clear) {
        if (const Entity e = view_entity(); !e.is_null()) kit::camera_2d_of(world_, e, cam, ppm, &clear);
        if (view_free_.size() == 2 || view_free_.size() == 3) {
            cam.position = {static_cast<f32>(view_free_[0]), static_cast<f32>(view_free_[1])};
            if (view_free_.size() == 3) {
                cam.zoom = math::max(static_cast<f32>(view_free_[2]), 0.01f);
                cam.fit_width = cam.fit_height = 0.0f;
            }
        }
    }

    // The same on the 3D camera; true when an override set `cam`.
    bool view_override_3d(f32 alpha, render::Camera3D& cam) {
        bool set = false;
        if (const Entity e = view_entity(); !e.is_null()) set = extract3d_.camera_of(world_, e, alpha, cam);
        if (view_free_.size() >= 6) {
            const vec3 eye{static_cast<f32>(view_free_[0]), static_cast<f32>(view_free_[1]), static_cast<f32>(view_free_[2])};
            const vec3 at{static_cast<f32>(view_free_[3]), static_cast<f32>(view_free_[4]), static_cast<f32>(view_free_[5])};
            const vec3 d = at - eye;
            const f32 dist = math::max(length(d), 1e-3f);
            const vec3 n = d * (1.0f / dist);
            const f32 fov = view_free_.size() == 7 ? static_cast<f32>(view_free_[6]) : 60.0f;
            cam = render::orbit_camera(at, std::atan2(-n.x, -n.z), std::asin(math::clamp(-n.y, -1.0f, 1.0f)), dist,
                                       math::radians(math::clamp(fov, 5.0f, 170.0f)));
            set = true;
        }
        return set;
    }

    // `path` is taken as given (relative: against the working directory); a missing directory
    // in it is created, so "-capture shots/title.ppm" works from a fresh project folder.
    void capture(const char* path, rhi::Extent2D size) {
        Array<u8> px;
        px.resize(usize(size.width) * size.height * 4);
        if (!device_->read_pixels({}, rhi::Rect2D{0, 0, size.width, size.height}, px.span())) return;
        const StringView p(path);
        usize sep = 0;
        for (usize i = 0; i < p.size(); ++i)
            if (p[i] == '/' || p[i] == '\\') sep = i;
        if (sep > 0) (void)platform::create_directories(p.substr(0, sep));  // fopen below reports the failure
        std::FILE* f = std::fopen(path, "wb");
        if (!f) {
            MCE_LOG_ERROR("mce_player: cannot write '{}' ({}; relative paths are taken from the working directory)", path,
                          std::strerror(errno));
            return;
        }
        std::fprintf(f, "P6\n%u %u\n255\n", size.width, size.height);
        for (usize i = 0; i < px.size(); i += 4) std::fwrite(&px[i], 1, 3, f);
        std::fclose(f);
        MCE_LOG_INFO("mce_player: wrote {}", path);
    }

    void write_save() {
        const StringView slot = cv_save.get_string();
        if (slot.empty()) return;
        save::SaveInfo info;
        info.scene = scene_guid_;
        info.tick = sim_ticks_;
        info.playtime = static_cast<f64>(sim_ticks_) * tick_dt_;
        if (Result<void> r = save::save_game(world_, slot, info); !r) {
            FixedString<512> why;
            format_error(r.error(), why);
            MCE_LOG_ERROR("mce_player: save '{}' failed: {}", slot, why.view());
        } else {
            std::printf("mce_player: save '%.*s' written (tick %llu)\n", static_cast<int>(slot.size()), slot.data(),
                        static_cast<unsigned long long>(sim_ticks_));
        }
    }

    // FR-RUN-100: the phone lifecycle. Background and terminating arrive inside the OS callback, so
    // the save is written before the app is frozen or killed.
    void on_lifecycle(platform::EventType type) {
        switch (type) {
            case platform::EventType::AppBackground: {
                if (os_background_) return;
                os_background_ = true;
                paused_ = true;
                audio_.set_paused(true);
                release_input();
                write_save();
                MCE_LOG_INFO("{}: in the background (sim and audio paused)", desc_.app_name);
                break;
            }
            case platform::EventType::AppForeground:
                if (!os_background_) return;
                os_background_ = false;
                audio_.set_paused(ad_paused_);
                MCE_LOG_INFO("{}: back in the foreground (paused until resumed)", desc_.app_name);
                break;
            case platform::EventType::AppTerminating: write_save(); break;
            case platform::EventType::AppLowMemory: MCE_LOG_WARN("{}: the OS reports low memory", desc_.app_name); break;
            default: break;
        }
    }

    // Everything held reads released from the next tick (the input system's focus-loss rule).
    void release_input() {
        platform::Event e;
        e.type = platform::EventType::FocusOut;
        e.timestamp_us = platform::now_us();
        input_.push_event(e);
    }

    // The pause overlay's resume. The resuming tap is swallowed: the game never sees it as a tap.
    void resume() {
        if (!paused_ || os_background_) return;
        paused_ = false;
        release_input();
    }

    bool parse_lifecycle_test(StringView v) {
        lifecycle_from_ = lifecycle_ticks_ = 0;
        if (v.empty()) return true;
        u64* out = &lifecycle_from_;
        bool digit = false;
        for (const char c : v) {
            if (c >= '0' && c <= '9') {
                *out = *out * 10 + static_cast<u64>(c - '0');
                digit = true;
            } else if (c == ',' && out == &lifecycle_from_ && digit) {
                out = &lifecycle_ticks_;
                digit = false;
            } else {
                return false;
            }
        }
        return digit && out == &lifecycle_ticks_ && lifecycle_from_ >= 1;
    }

    // -lifecycle_test: the round trip through the same path the OS takes (windowed: the platform's
    // delivery to the window; headless: straight in), then the tap that resumes.
    void rehearse_lifecycle(u64 app_tick) {
        auto send = [&](platform::EventType type) {
            if (window_) platform::simulate_app_event(type);
            else on_lifecycle(type);
        };
        if (app_tick == lifecycle_from_) send(platform::EventType::AppBackground);
        if (app_tick == lifecycle_from_ + lifecycle_ticks_) {
            send(platform::EventType::AppForeground);
            resume();
        }
    }

    // Unfocused / minimized: pause (the overlay waits for the player) and mute the master bus.
    void set_background(bool background) {
        // A desktop has no background event before a kill or a sleep: the focus loss saves (#177).
        if (background && !unfocused_ && !os_background_ && cv_save_on_focus_loss.get_bool()) write_save();
        unfocused_ = background;
        if (!cv_pause_on_focus_loss.get_bool() || background == background_) return;
        background_ = background;
        if (background) paused_ = true;
        set_muted(background);
    }

    // -r.frame_report N (#214): wall time between rendered frames, logged as average / p95 / worst
    // every N frames and for the whole run at exit. Windowed only (headless frames are not paced).
    void meter_frame() {
        const i64 every = cv_frame_report.get_int();
        if (!device_ || every <= 0) return;
        const u64 now = platform::now_us();
        if (last_frame_us_ != 0) {
            const f32 ms = static_cast<f32>(now - last_frame_us_) * 0.001f;
            frame_ms_.push_back(ms);
            run_sum_ms_ += ms;
            run_worst_ms_ = std::max(run_worst_ms_, ms);
            ++run_frames_;
            if (frame_ms_.size() >= static_cast<usize>(every)) {
                report_frames("frame times", frame_ms_);
                frame_ms_.clear();
            }
        }
        last_frame_us_ = now;
    }

    void report_frames(const char* what, Array<f32>& ms) const {
        if (ms.empty()) return;
        f64 sum = 0;
        for (f32 v : ms) sum += v;
        std::sort(ms.begin(), ms.end());
        const usize p95 = std::min(ms.size() - 1, (ms.size() * 95) / 100);
        std::printf("%s: %s over %zu frames: avg %.2f ms, p95 %.2f ms, worst %.2f ms\n", desc_.app_name, what, ms.size(),
                    sum / static_cast<f64>(ms.size()), static_cast<f64>(ms[p95]), static_cast<f64>(ms.back()));
    }

    void report_run_frames() const {
        if (run_frames_ == 0) return;
        std::printf("%s: frame times over the run (%llu frames): avg %.2f ms, worst %.2f ms\n", desc_.app_name,
                    static_cast<unsigned long long>(run_frames_), run_sum_ms_ / static_cast<f64>(run_frames_),
                    static_cast<f64>(run_worst_ms_));
    }

    // The fullscreen convar (command line, console, the toggle keys) applied to the window (#178).
    void sync_fullscreen() {
        if (!window_ || cv_fullscreen.get_bool() == fullscreen_) return;
        fullscreen_ = cv_fullscreen.get_bool();
        if (!window_->set_fullscreen(fullscreen_)) MCE_LOG_WARN("mce_player: the window cannot go full screen here");
    }

    void set_muted(bool muted) {
        if (muted == muted_) return;
        muted_ = muted;
        if (muted) {
            master_db_ = audio_.bus_gain_db(audio::Bus_Master);
            audio_.set_bus_gain(audio::Bus_Master, -80.0f);
        } else {
            audio_.set_bus_gain(audio::Bus_Master, master_db_);
        }
    }

    static void on_event(const platform::Event& e, void* user) {
        auto* self = static_cast<PlayerApp*>(user);
        switch (e.type) {
            case platform::EventType::AppBackground:
            case platform::EventType::AppForeground:
            case platform::EventType::AppTerminating:
            case platform::EventType::AppLowMemory: self->on_lifecycle(e.type); return;
            case platform::EventType::SafeAreaChanged:
            case platform::EventType::OrientationChanged:
            case platform::EventType::Resized: self->set_safe_insets(self->window_insets()); break;
            case platform::EventType::TouchUp:
                // A tap on the pause overlay (a finger that went down while paused) resumes; the game gets a
                // focus-loss release instead of the tap.
                if (e.code < 16 && (self->paused_touches_ & (1u << e.code))) {
                    self->paused_touches_ &= ~(1u << e.code);
                    if (!(e.flags & platform::EventFlag::TouchCanceled)) {
                        self->resume();
                        return;
                    }
                }
                break;
            case platform::EventType::TouchDown:
                if (self->paused_ && !self->os_background_ && !self->console_.is_open() && e.code < 16) {
                    self->paused_touches_ |= 1u << e.code;
                } else if (!self->paused_ && !self->console_.is_open() && !self->screens_.active() && e.x >= 0.85f &&
                           e.y <= 0.15f) {
                    self->paused_ = true;  // the top-right corner pauses (the default map's Pause tap zone)
                }
                break;
            default: break;
        }
        self->track_ui_pointer(e);
        // The game's input: every event while the console is closed; releases and focus changes
        // always, so no key sticks behind the console or the pause overlay.
        if (!self->console_.is_open() || e.type == platform::EventType::KeyUp || e.type == platform::EventType::TouchUp ||
            e.type == platform::EventType::FocusOut)
            self->input_.push_event(e);
        switch (e.type) {
            case platform::EventType::FocusOut:
            case platform::EventType::Minimized: self->set_background(true); return;
            case platform::EventType::FocusIn:
            case platform::EventType::Restored: self->set_background(false); return;
            default: break;
        }
        if (self->console_.is_open()) {  // the console owns the keyboard while open
            if (e.type == platform::EventType::TextInput && e.code >= 32 && e.code < 127 && e.code != '`') {
                const char c = static_cast<char>(e.code);
                self->con_.typed.append(StringView(&c, 1));
            }
            if (e.type != platform::EventType::KeyDown) return;
            switch (static_cast<platform::Key>(e.code)) {
                case platform::Key::Escape: self->con_.cancel = true; break;
                case platform::Key::Return: self->con_.enter = true; break;
                case platform::Key::Backspace: self->con_.backspace = true; break;
                case platform::Key::Up: self->con_.up = true; break;
                case platform::Key::Down: self->con_.down = true; break;
                case platform::Key::Grave:
                case platform::Key::F1:
                    if (!(e.flags & platform::EventFlag::Repeat)) self->set_console(false);
                    break;
                default: break;
            }
            return;
        }
        if (e.type != platform::EventType::KeyDown) return;
        switch (static_cast<platform::Key>(e.code)) {
            case platform::Key::Grave:
            case platform::Key::F1:
                if (cv_console.get_bool() && !(e.flags & platform::EventFlag::Repeat)) self->set_console(true);
                break;
            case platform::Key::Escape:
                if (!self->screens_.active()) self->paused_ = !self->paused_;  // UI screens: the game's own Pause
                break;
            case platform::Key::Q:
                if (self->paused_) self->quit(0);
                break;
            case platform::Key::F7: cv_post.set_bool(!cv_post.get_bool()); break;
            case platform::Key::F11: cv_fullscreen.set_bool(!cv_fullscreen.get_bool()); break;
            case platform::Key::Return:
                if ((e.flags & platform::EventFlag::Alt) && !(e.flags & platform::EventFlag::Repeat))
                    cv_fullscreen.set_bool(!cv_fullscreen.get_bool());
                break;
            case platform::Key::F:
                if ((e.flags & platform::EventFlag::Ctrl) && (e.flags & platform::EventFlag::Gui) &&
                    !(e.flags & platform::EventFlag::Repeat))
                    cv_fullscreen.set_bool(!cv_fullscreen.get_bool());
                break;
            case platform::Key::F3: cv_stats.set_bool(!cv_stats.get_bool()); break;
            default: break;
        }
    }

    // The pointer the UI screens hit-test: the mouse (window points) or the first finger (0..1).
    void track_ui_pointer(const platform::Event& e) {
        switch (e.type) {
            case platform::EventType::MouseMove:
                if (!ui_pointer_.touch || !ui_pointer_.down) {
                    ui_pointer_.touch = false;
                    ui_pointer_.pos = {e.x, e.y};
                }
                break;
            case platform::EventType::MouseButtonDown:
            case platform::EventType::MouseButtonUp:
                if (e.code != static_cast<u32>(platform::MouseButton::Left)) break;
                ui_pointer_.touch = false;
                ui_pointer_.pos = {e.x, e.y};
                ui_pointer_.down = e.type == platform::EventType::MouseButtonDown;
                (ui_pointer_.down ? ui_pointer_.pressed : ui_pointer_.released) = true;
                break;
            case platform::EventType::TouchDown:
                if (ui_pointer_.down) break;  // one finger points
                ui_pointer_ = UiPointer{{e.x, e.y}, e.code, true, true, true, false};
                break;
            case platform::EventType::TouchMove:
                if (ui_pointer_.touch && ui_pointer_.down && ui_pointer_.slot == e.code) ui_pointer_.pos = {e.x, e.y};
                break;
            case platform::EventType::TouchUp:
                if (!ui_pointer_.touch || !ui_pointer_.down || ui_pointer_.slot != e.code) break;
                ui_pointer_.pos = {e.x, e.y};
                ui_pointer_.down = false;
                if (!(e.flags & platform::EventFlag::TouchCanceled)) ui_pointer_.released = true;
                break;
            default: break;
        }
    }

    const player::PlayerDesc& desc_;
    platform::Window* window_;
    rhi::RenderDevice* device_;
    audio::AudioDevice& audio_;
    asset::AssetSystem assets_;
    render::Renderer renderer_;
    render::AssetUploadSink sink_;
    audio::ClipBank clips_;
    kit::SpriteExtract extract_;
    kit::Kit3DExtract extract3d_;
    render::Scene3D::Stats stats3d_;
    bool three_d_ = false;         // the 3D path is on (a scene with a Camera3DComponent)
    bool three_d_failed_ = false;  // init_3d failed once: the game runs 2D-only
    bool relative_mouse_ = false;
    render::Font font_;
    ui::BuiltinSkin skin_;
    ui::DevConsole console_;
    input::InputSystem input_;
    struct ConsoleKeys {  // this frame's console input, gathered by on_event
        FixedString<64> typed;
        bool cancel = false, enter = false, backspace = false, up = false, down = false;
    };
    ConsoleKeys con_;
    // Declared before the World so it is destroyed after it: the World holds the module's code.
    struct ModuleGuard {
        LoadedModule module;
        ~ModuleGuard() { unload_game_module(module); }
    } module_;
    monetize::FakeStore fake_store_;  // -monetize.provider fake
    monetize::PlatformStore platform_store_;  // the phone's store, the default when the host gives none
    monetize::PlatformAds platform_ads_;      // the phone's ad network, likewise
    monetize::FakeAds fake_ads_;
    monetize::Service monetize_;  // before the World: its resources point here (PlayerDesc::monetize)
    liveops::Service liveops_;  // PlayerDesc::liveops
    std::optional<liveops::analytics::FileBackend> local_analytics_;  // -liveops.provider local
    FirebaseAnalytics firebase_analytics_;                             // -liveops.provider firebase
    bool firebase_on_ = false;
    u32 firebase_collection_ = ~0u;  // the Collection last handed to the SDK
    std::optional<liveops::OutboxUploader> local_outbox_;
    std::optional<liveops::FileConfigFetcher> file_fetcher_;  // -remote_config.file
    std::optional<FirebaseConfigFetcher> firebase_config_;      // with firebase_on_ and no other fetcher
    liveops::RemoteConfigService remote_config_;  // before the World: its resource points here
    u32 dumped_config_version_ = ~0u;
    std::optional<liveops::LocalGameServices> local_services_;  // -liveops.provider local
    liveops::GameServicesService game_services_;  // before the World: its resource points here
    Array<ServicesStep> game_services_test_;
    bool game_services_on_ = false;
    bool game_services_print_ = false;  // -game_services_test, or the local provider
    bool liveops_on_ = false;
    bool liveops_consent_auto_ = true;
    Array<MonetizeStep> monetize_test_;
    bool monetize_on_ = false;
    World world_;
    Array<Entity> entities_;
    FixedString<256> scene_path_;
    asset::ModList mods_;  // mounted mods in load order (published as a resource when the game takes mods)
    u64 sim_ticks_ = 0;
    u64 run_start_tick_ = 0;  // sim_ticks_ when this run began (a -save slot's tick), for -capture_tick
    Array<u64> capture_ticks_;  // -capture_tick / -capture_ticks, ascending
    usize capture_next_ = 0;    // the next of them to capture
    Array<f64> ticks_tmp_;
    Array<f64> view_free_;      // -view.free
    Array<f64> safe_override_;  // -safe_area
    bool paused_ = false;
    player::UiScreens screens_;
    input::InputState ui_actions_;  // the newest tick snapshot, for the UI screens' navigation
    bool ui_fresh_ = false;         // ui_actions_ arrived since the last rendered frame
    bool ui_map_pushed_ = false;
    bool ui_map_missing_ = false;
    bool ui_preview_ = false;  // -ui_preview: one document over the clear color
    struct UiPointer {
        math::vec2 pos{-1, -1};
        u32 slot = 0;
        bool touch = false, down = false, pressed = false, released = false;
    } ui_pointer_;
    bool background_ = false;  // unfocused or minimized
    bool unfocused_ = false;   // the window lost focus (or minimized); the focus-loss save ran
    bool fullscreen_ = false;  // what cv_fullscreen last applied
    u64 last_frame_us_ = 0;    // -r.frame_report: the previous rendered frame
    Array<f32> frame_ms_;      // this report window's frame times
    f64 run_sum_ms_ = 0;
    f32 run_worst_ms_ = 0;
    u64 run_frames_ = 0;
    bool os_background_ = false;  // the app itself is in the background (phones): no drawing at all
    bool ad_paused_ = false;      // a fullscreen ad covers the game (PlayerDesc::monetize)
    platform::SafeInsets safe_insets_;
    u64 lifecycle_from_ = 0, lifecycle_ticks_ = 0;  // -lifecycle_test
    u32 paused_touches_ = 0;  // finger slots that went down on the pause overlay
    bool muted_ = false;
    f32 master_db_ = 0.0f;     // the master gain to restore when focus returns
    f64 tick_dt_ = 1.0 / 60.0;
    asset::Guid scene_guid_;
    rhi::TextureHandle post_target_;
    rhi::Extent2D post_size_;
};

// -input <file>: the script that stands in for the keyboard (both modes). Null without -input;
// a bad file is an error the caller reports (exit 1), not a silent keyboard-less run.
Result<input::InputScript*> load_input_script(const player::PlayerDesc& pd) {
    static input::InputScript script;
    const StringView path = cv_input.get_string();
    if (path.empty()) return static_cast<input::InputScript*>(nullptr);
    const FixedString<1024> p(path);
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return Error(Errc::FileNotFound, "player.input", path);
    Array<char> text;
    char buf[4096];
    for (usize n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
        for (usize i = 0; i < n; ++i) text.push_back(buf[i]);
    std::fclose(f);
    MCE_TRY(script.parse(StringView(text.data(), text.size())));
    MCE_LOG_INFO("{}: input script {} ({} segments, last tick {})", pd.app_name, path, script.segments().size(),
                 script.last_tick());
    return &script;
}

int run_headless(const player::PlayerDesc& pd) {
    // The windowed-only flags: say so instead of silently running the default tick count.
    if (cv_frames.get_int() > 0)
        MCE_LOG_WARN("{}: -frames applies to windowed runs; headless runs -ticks N (running {} ticks)", pd.app_name,
                     cv_ticks.get_int() > 0 ? cv_ticks.get_int() : 600);
    if (!cv_capture.get_string().empty() || cv_capture_tick.get_int() > 0 || !cv_capture_ticks.get_string().empty())
        MCE_LOG_WARN("{}: -capture / -capture_tick(s) need a window; nothing is drawn headless", pd.app_name);
    audio::AudioDesc ad;
    ad.backend = audio::Backend::Silent;
    auto audio = audio::AudioDevice::create(ad);
    if (!audio) return 1;
    Result<input::InputScript*> script = load_input_script(pd);
    if (!script) return fail("input script", script.error());
    PlayerApp app(pd, nullptr, nullptr, *audio.value());
    app::AppDesc desc;
    desc.name = pd.app_name;
    desc.headless = true;
    desc.max_ticks = static_cast<u64>(cv_ticks.get_int() > 0 ? cv_ticks.get_int() : 600);
    PlayerApp::ScriptBySimTick by_sim(app, script.value());
    desc.input_source = script.value() ? static_cast<app::IInputSource*>(&by_sim) : nullptr;
    const int rc = app::run(app, desc);
    if (rc == 0)
        std::printf("%s: %llu ticks, %zu entities, state_hash %016llx\n", pd.app_name, static_cast<unsigned long long>(app.ticks()),
                    app.entity_count(), static_cast<unsigned long long>(app.state_hash()));
    if (const scene::StreamingStats* st = app.streaming_stats(); rc == 0 && st && st->chunks > 0)
        std::printf("%s: streaming %u/%u chunks loaded, %u streamed entities, %llu loads, %llu unloads\n", pd.app_name,
                    st->loaded, st->chunks, st->streamed_entities, static_cast<unsigned long long>(st->total_loads),
                    static_cast<unsigned long long>(st->total_unloads));
    return rc;
}

int run_windowed(const player::PlayerDesc& pd) {
    platform::WindowDesc wd;
    wd.title = pd.title;
    wd.width = 1280;
    wd.height = 720;
    if (const StringView ws = cv_window.get_string(); !ws.empty()) {
        unsigned w = 0, h = 0;
        char extra = 0;
        char spec[64] = {};
        std::memcpy(spec, ws.data(), std::min<usize>(ws.size(), sizeof(spec) - 1));
        if (std::sscanf(spec, "%ux%u%c", &w, &h, &extra) != 2 || w < 64 || h < 64 || w > 16384 || h > 16384) {
            std::fprintf(stderr, "mce_player: -window wants WxH (64..16384 each), e.g. 1080x2400\n");  // usage, no dialog
            return 2;
        }
        wd.width = w;
        wd.height = h;
    }
    auto window = platform::Window::create(wd);
    if (!window) return fail("window creation failed", window.error());
    rhi::DeviceDesc dd;
    dd.debug_layer = cv_gl_debug.get_bool();
    dd.uniform_ring_bytes = static_cast<usize>(math::clamp<i64>(cv_uniform_ring.get_int(), 1, 64)) * MiB;
    auto device = rhi::create_default_device(*window.value(), dd);
    if (!device) return fail("render device creation failed", device.error());
    auto audio = audio::AudioDevice::create({});  // falls back to Silent when no output device opens
    if (!audio) return fail("audio init failed", audio.error());

    Result<input::InputScript*> script = load_input_script(pd);
    if (!script) return fail("input script", script.error());
    PlayerApp app(pd, window.value().get(), device.value().get(), *audio.value());
    app::AppDesc desc;
    desc.name = pd.app_name;
    desc.platform = &app;
    desc.fences = device.value().get();
    PlayerApp::ScriptBySimTick by_sim(app, script.value());
    desc.input_source = script.value() ? static_cast<app::IInputSource*>(&by_sim) : &app.input();
    app::RunStats stats;
    int rc = app::run(app, desc, &stats);
    const u32 gl_errors = device.value()->validation_error_count();
    std::printf("%s: %llu frames, %llu ticks, %u GL errors\n", pd.app_name, static_cast<unsigned long long>(stats.frames),
                static_cast<unsigned long long>(app.ticks()), gl_errors);
    if (rc == 0 && gl_errors != 0) rc = 2;
    return rc;
}

}  // namespace

int mce::player::run(int argc, char** argv, const PlayerDesc& pd) {
    log::LogDesc log_desc;
    log_desc.app_name = pd.app_name;
    log::init(log_desc);
    const samples::BuildInfo build = samples::begin_build();
    // A game module's own MCE_CVARs register only when it loads: their switches wait for it (#213).
    console::parse_command_line(argc, argv, /*defer_unknown=*/true);
    FixedString<1024> usage;
    usage.append("usage: ");
    usage.append(pd.app_name);
    usage.append(pd.pack ? " [-pack <assets.mce | cooked dir>]" : " -pack <assets.mce | cooked dir>");
    usage.append(" [-scene <guid | path under assets/>] [-module <game module>] [-headless 1 -ticks N] [-input <script>]\n"
                 "       [-frames N | -capture_tick N] [-capture <file.ppm>] [-save <slot>] [-version] [-help]\n"
                 "  Windowed by default (Esc pauses, Q quits while paused, ` or F1 opens the console).\n"
                 "  -headless 1 runs -ticks fixed steps without a window and prints the World state hash.");
    if (!samples::start_build(pd.app_name, build, usage.c_str())) {
        log::shutdown();
        return 0;
    }

    const ConVar* headless = ConVar::find(StringId("headless"));
    const bool is_headless = headless && headless->get_bool();
    platform::haptics::set_enabled(!is_headless && cv_haptics.get_bool());  // output only: a headless run plays none
    int rc = 1;
    if (is_headless) {
        rc = run_headless(pd);
    } else if (ResultV r = platform::init(); !r) {
        rc = fail("platform init failed", r.error());
    } else {
        rc = run_windowed(pd);
        platform::shutdown();
    }
    log::shutdown();
    return rc;
}
