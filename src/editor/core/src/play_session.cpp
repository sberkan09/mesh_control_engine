#include "mce/editor/play_session.h"

#include "mce/audio/clip_bank.h"
#include "mce/audio/components.h"
#include "mce/input/default_map.h"
#include "mce/input/ecs.h"
#include "mce/input/input_system.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/particles.h"
#include "mce/kit/sequence.h"
#include "mce/log.h"
#include "mce/memory/frame_slots.h"
#include "mce/physics/module.h"
#include "mce/platform/platform.h"
#include "mce/scene/scene.h"
#include "mce/scene/streaming.h"

namespace mce::editor {

namespace {
GameInstallFn g_game_install = nullptr;  // set once at startup by a project editor's main

constexpr const char* kStage = "editor.play";

// The project's action map, picked the way mce_player picks it with no -input_map: the first
// cooked .mcinput by path, else the built-in default map.
Result<void> load_project_input_map(asset::AssetSystem& assets, input::InputSystem& in) {
    asset::AssetKey best = asset::kInvalidAssetKey;
    for (asset::AssetKey k = 0; k < assets.asset_count(); ++k) {
        const asset::AssetInfo info = assets.info(k);
        if (info.type != asset::ChunkType::InputMap) continue;
        if (best == asset::kInvalidAssetKey || info.path < assets.info(best).path) best = k;
    }
    if (best == asset::kInvalidAssetKey) {
        Array<byte> map;
        input::default_map_chunk(map);
        return in.load_map(map.span());
    }
    MCE_TRY_ASSIGN(const asset::AssetHandle<asset::InputMap> h, assets.load_sync<asset::InputMap>(best));
    const Result<void> r = in.load_map(assets.resolve(h));
    assets.release(asset::AnyHandle(h));
    return r;
}

}  // namespace

PlaySession::PlaySession() = default;

PlaySession::~PlaySession() { stop(); }

Result<void> PlaySession::enter(EditorProject& project, const SceneDocument& doc, StringView scene_path,
                                cook::DiagList& diags) {
    stop();
    const f64 t0 = platform::now_seconds();
    asset::AssetSystem* assets = project.mounted();
    if (!project.is_open() || !assets) {
        diags.error(Errc::InvalidArgument, scene_path, 0, 0, kStage, "play needs an open, cooked project");
        return Error(Errc::InvalidArgument, kStage, "no cooked project");
    }

    // 1. The document, cooked in memory: what the cook would write for the saved file.
    cook::String text;
    doc.serialize(text);
    cook::AssetRegistry registry;
    registry.scan(project.root(), project.importers(), false, diags);
    const cook::IImporter* importer = project.importers().for_extension(".mcscene");
    MCE_ASSERT(importer);
    cook::String rel;
    if (scene_path.empty() || !project.relative_asset_path(scene_path, rel)) rel = "scenes/untitled.mcscene";
    Array<byte> container;
    if (!cook::cook_document(registry, *importer, doc.guid(), rel.view(), text.view(), container, diags))
        return Error(Errc::ImportFailed, kStage, rel.view());
    MCE_TRY_ASSIGN(asset::ContainerView view, asset::ContainerView::open(container.span(), rel.view()));
    const u32 chunk = view.find(asset::ChunkType::Scene);
    if (chunk >= view.chunk_count()) return Error(Errc::ImportFailed, kStage, "no scene chunk");
    MCE_TRY_ASSIGN(asset::SceneView scene, asset::SceneView::read(view.payload(chunk)));

    // 2. mce_player's world: input, physics2d, the audio bridge on a Silent device, the scene.
    input_ = mem::make_unique<input::InputSystem>(mem::heap());
    if (Result<void> r = load_project_input_map(*assets, *input_); !r) {
        diags.error(r.error().code, rel.view(), 0, 0, kStage, "the project's input map does not load");
        stop();
        return r;
    }
    audio::AudioDesc ad;
    ad.backend = audio::Backend::Silent;
    MCE_TRY_ASSIGN(audio_, audio::AudioDevice::create(ad));
    clips_ = mem::make_unique<audio::ClipBank>(mem::heap(), *assets, *audio_);
    world_ = mem::make_unique<World>(mem::heap());
    world_->set_profiling(true);  // the Profiler panel ranks the play world's systems (never hashed)
    input::install(*world_);
    physics::install_physics2d(*world_);
    audio::install(*world_, *audio_, clips_.get());
    kit::install_particles(*world_, assets);
    kit::install_sequences(*world_, assets);
    kit::install_kit3d(*world_, assets);
    if (g_game_install) {
        if (Result<void> r = g_game_install(*world_); !r) {
            diags.error(r.error().code, rel.view(), 0, 0, kStage, "the game's install() failed");
            stop();
            return r;
        }
    }
    // Scene streaming (FR-ENT-120), inert without SceneChunks. Synchronous here: play ticks are
    // driven step by step by tools and tests, which expect a chunk to be in once it is wanted.
    scene::StreamingConfig streaming;
    streaming.async = false;
    scene::install_streaming(*world_, *assets, streaming);
    if (Result<void> r = scene::instantiate(*world_, scene, &entities_); !r) {
        diags.error(r.error().code, rel.view(), 0, 0, kStage, "the scene does not instantiate");
        stop();
        return r;
    }
    scene::pump_streaming(*world_, scene::PumpMode::Settle);
    if (!scratch_) scratch_ = mem::make_unique<mem::LinearArena>(mem::heap(), mem::heap(), 16 * MiB, "editor.play");
    state_ = State::Playing;
    accumulator_ = 0.0;
    ticks_ = 0;
    last_close_us_ = platform::now_us();
    input_->set_tick_origin_us(last_close_us_);
    enter_hash_ = world_->state_hash();
    enter_seconds_ = platform::now_seconds() - t0;
    MCE_LOG_INFO("editor: play {} ({} entities, entered in {:.0f} ms)", rel.view(), entities_.size(),
                 enter_seconds_ * 1000.0);
    return ok();
}

void set_play_game_install(GameInstallFn install) { g_game_install = install; }
GameInstallFn play_game_install() { return g_game_install; }

void PlaySession::stop() {
    if (clips_) clips_->release_all();
    world_.reset();  // before the bank and the device: audio components hold clip handles
    clips_.reset();
    audio_.reset();
    input_.reset();
    entities_.clear();
    if (state_ != State::Stopped) MCE_LOG_INFO("editor: play stopped after {} ticks", ticks_);
    state_ = State::Stopped;
    accumulator_ = 0.0;
}

u64 PlaySession::state_hash() const { return world_ ? world_->state_hash() : 0; }

void PlaySession::push_event(const platform::Event& e) {
    if (input_ && state_ != State::Stopped) input_->push_event(e);
}

void PlaySession::tick_once(u64 close_us) {
    // The editor ticks inside its own app frame; tests and tools call this outside one.
    mem::LinearArena* prev = nullptr;
    const bool own_arena = !mem::has_frame_arena();
    if (own_arena) {
        scratch_->reset();
        prev = mem::set_frame_arena(scratch_.get());
    }
    // The tick's input: events stamped before its close (wall clock: pause, catch-up clamps and
    // slow motion move the tick grid, so the grid is not origin + N*T here). Closes only advance.
    if (close_us <= last_close_us_) close_us = last_close_us_ + 1;
    last_close_us_ = close_us;
    input::publish(*world_, *input_->sample_for_tick(ticks_ + 1, close_us));
    scene::pump_streaming(*world_);
    world_->tick(kTickDt, ticks_);
    ++ticks_;
    audio_->update();  // the Silent backend mixes one block per call (mce_player -headless)
    if (own_arena) mem::set_frame_arena(prev);
}

u32 PlaySession::advance(f64 real_dt) {
    if (state_ != State::Playing) {
        if (clips_) clips_->sync();
        last_advance_ms_ = 0.0;
        last_advance_ticks_ = 0;
        return 0;
    }
    const f64 t0 = platform::now_seconds();
    accumulator_ += (real_dt > 0.0 ? real_dt : 0.0) * static_cast<f64>(time_scale_);
    const f64 cap = kTickDt * kMaxCatchup;
    if (accumulator_ > cap) accumulator_ = cap;
    u32 n = 0;
    for (f64 left = accumulator_; left + 1e-9 >= kTickDt; left -= kTickDt) ++n;
    // This frame's ticks share out the wall time up to now: the last one closes at now, each
    // earlier one a tick's worth of time before it (+1: the close is exclusive, and an event
    // stamped this microsecond is already in).
    const u64 now = platform::now_us() + 1;
    const u64 tick_us = static_cast<u64>(kTickDt * 1e6);
    for (u32 i = 0; i < n; ++i) {
        const u64 back = static_cast<u64>(n - 1 - i) * tick_us;
        tick_once(now > back ? now - back : 0);
        accumulator_ -= kTickDt;
    }
    if (accumulator_ < 0.0) accumulator_ = 0.0;
    audio_->update();
    clips_->sync();
    last_advance_ms_ = (platform::now_seconds() - t0) * 1000.0;
    last_advance_ticks_ = n;
    return n;
}

void PlaySession::pause() {
    if (state_ == State::Playing) state_ = State::Paused;
}

void PlaySession::resume() {
    if (state_ == State::Paused) {
        state_ = State::Playing;
        accumulator_ = 0.0;
    }
}

void PlaySession::step() {
    if (state_ == State::Stopped) return;
    if (state_ == State::Playing) {
        pause();
        return;
    }
    tick_once(platform::now_us() + 1);
    clips_->sync();
}

void PlaySession::set_time_scale(f32 scale) { time_scale_ = scale < 0.0f ? 0.0f : (scale > 8.0f ? 8.0f : scale); }

}  // namespace mce::editor
