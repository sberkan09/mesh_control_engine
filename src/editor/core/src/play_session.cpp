#include "mce/editor/play_session.h"

#include "mce/audio/clip_bank.h"
#include "mce/audio/components.h"
#include "mce/kit/particles.h"
#include "mce/kit/sequence.h"
#include "mce/log.h"
#include "mce/memory/frame_slots.h"
#include "mce/physics/module.h"
#include "mce/platform/platform.h"
#include "mce/scene/scene.h"

namespace mce::editor {

namespace {

constexpr const char* kStage = "editor.play";

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

    // 2. mce_player's headless world: physics2d, the audio bridge on a Silent device, the scene.
    audio::AudioDesc ad;
    ad.backend = audio::Backend::Silent;
    MCE_TRY_ASSIGN(audio_, audio::AudioDevice::create(ad));
    clips_ = mem::make_unique<audio::ClipBank>(mem::heap(), *assets, *audio_);
    world_ = mem::make_unique<World>(mem::heap());
    physics::install_physics2d(*world_);
    audio::install(*world_, *audio_, clips_.get());
    kit::install_particles(*world_, assets);
    kit::install_sequences(*world_, assets);
    if (Result<void> r = scene::instantiate(*world_, scene, &entities_); !r) {
        diags.error(r.error().code, rel.view(), 0, 0, kStage, "the scene does not instantiate");
        stop();
        return r;
    }
    if (!scratch_) scratch_ = mem::make_unique<mem::LinearArena>(mem::heap(), mem::heap(), 16 * MiB, "editor.play");
    state_ = State::Playing;
    accumulator_ = 0.0;
    ticks_ = 0;
    enter_hash_ = world_->state_hash();
    enter_seconds_ = platform::now_seconds() - t0;
    MCE_LOG_INFO("editor: play {} ({} entities, entered in {:.0f} ms)", rel.view(), entities_.size(),
                 enter_seconds_ * 1000.0);
    return ok();
}

void PlaySession::stop() {
    if (clips_) clips_->release_all();
    world_.reset();  // before the bank and the device: audio components hold clip handles
    clips_.reset();
    audio_.reset();
    entities_.clear();
    if (state_ != State::Stopped) MCE_LOG_INFO("editor: play stopped after {} ticks", ticks_);
    state_ = State::Stopped;
    accumulator_ = 0.0;
}

u64 PlaySession::state_hash() const { return world_ ? world_->state_hash() : 0; }

void PlaySession::tick_once() {
    // The editor ticks inside its own app frame; tests and tools call this outside one.
    mem::LinearArena* prev = nullptr;
    const bool own_arena = !mem::has_frame_arena();
    if (own_arena) {
        scratch_->reset();
        prev = mem::set_frame_arena(scratch_.get());
    }
    world_->tick(kTickDt, ticks_);
    ++ticks_;
    audio_->update();  // the Silent backend mixes one block per call (mce_player -headless)
    if (own_arena) mem::set_frame_arena(prev);
}

u32 PlaySession::advance(f64 real_dt) {
    if (state_ != State::Playing) {
        if (clips_) clips_->sync();
        return 0;
    }
    accumulator_ += (real_dt > 0.0 ? real_dt : 0.0) * static_cast<f64>(time_scale_);
    const f64 cap = kTickDt * kMaxCatchup;
    if (accumulator_ > cap) accumulator_ = cap;
    u32 n = 0;
    while (accumulator_ + 1e-9 >= kTickDt) {
        tick_once();
        accumulator_ -= kTickDt;
        ++n;
    }
    if (accumulator_ < 0.0) accumulator_ = 0.0;
    audio_->update();
    clips_->sync();
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
    tick_once();
    clips_->sync();
}

void PlaySession::set_time_scale(f32 scale) { time_scale_ = scale < 0.0f ? 0.0f : (scale > 8.0f ? 8.0f : scale); }

}  // namespace mce::editor
