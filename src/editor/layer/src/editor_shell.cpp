#include "mce/editor/editor_shell.h"

#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder (default layout)

#include <cstdio>

#include "imgui_platform.h"
#include "imgui_renderer.h"
#include "mce/convar.h"
#include "mce/cook/fs.h"
#include "mce/editor/align_entities.h"
#include "mce/editor/batch_rename.h"
#include "mce/editor/camera_bookmarks.h"
#include "mce/editor/entity_path.h"
#include "mce/editor/group_entities.h"
#include "mce/editor/quick_open.h"
#include "mce/editor/recent_list.h"
#include "mce/editor/reset_transform.h"
#include "mce/editor/select_all.h"
#include "mce/editor/select_related.h"
#include "mce/editor/selection_history.h"
#include "mce/editor/selection_sets.h"
#include "mce/editor/unique_names.h"
#include "mce/log.h"
#include "mce/platform/library.h"
#include "mce/platform/platform.h"
#include "mce/platform/process.h"
#include "scene_panels.h"

namespace mce::editor {

struct EditorShell::Impl {
    ImGuiContext* context = nullptr;
    ImGuiPlatform platform;
    ImGuiRhiRenderer renderer;
    platform::Process player;
    bool layout_checked = false;
    // Ctrl+P (M-QUICKOPEN): the searchable items, filled when the popup opens, and the query box state.
    QuickOpen quick;
    char quick_query[128] = {};
    i32 quick_sel = 0;
    // File > Open Recent (M-RECENT): the open project's list, <root>/.mce/recent_scenes.txt.
    RecentList recent;
    cook::String recent_root;  // the project `recent` was loaded for
    cook::String recent_last;  // the scene path last added to it
    // Camera bookmarks (M-CAMBOOKMARK): the open project's nine slots, <root>/.mce/camera_bookmarks.txt.
    CameraBookmarks bookmarks;
    // Alt+Left / Alt+Right (M-SELHISTORY): the entities selected in the open scene.
    SelectionHistory sel_history;
    const EditorScene* sel_scene = nullptr;  // the scene `sel_history` and `sel_sets` belong to
    // Ctrl+Shift+1..9 / Alt+1..9 (M-SELSETS): numbered groups of selected entities.
    SelectionSets sel_sets;
    // Edit > Rename Selection (M-BATCHRENAME): the pattern box and the first number.
    char rename_pattern[64] = {};
    i32 rename_start = 1;
};

namespace {

// Edit > Align: lines the selection up on `axis` (`distribute`: spaces it evenly instead).
void align_selection(EditorScene& scene, AlignAxis axis, AlignMode mode, bool distribute = false) {
    Array<EntityId> ids;  // a copy: the edits below touch the scene
    for (EntityId id : scene.selection().items()) ids.push_back(id);
    if (distribute)
        distribute_entities(scene, ids.span(), axis);
    else
        align_entities(scene, ids.span(), axis, mode);
}

// Edit > Make names unique: numbers the duplicates among the selection, or among every entity when nothing is selected.
void unique_names_in(EditorScene& scene) {
    Array<EntityId> ids;
    if (scene.selection().empty())
        ids = select_all(scene);
    else
        for (EntityId id : scene.selection().items()) ids.push_back(id);
    make_names_unique(scene, ids.span());
}

// Edit > Reset transform: the selection's local transforms go back to the identity.
void reset_selection_transforms(EditorScene& scene) {
    Array<EntityId> ids;  // a copy: the edit below touches the scene
    for (EntityId id : scene.selection().items()) ids.push_back(id);
    reset_transforms(scene, ids.span());
}

// Edit > Copy path: the hierarchy paths of the selection (Room/Wall/Torch), one per line, on the clipboard.
void copy_selection_paths(const EditorScene& scene) {
    const cook::String text = entity_paths(scene, scene.selection().items());
    if (!text.empty()) ImGui::SetClipboardText(text.c_str());
}

// Ctrl+G: the selection moves under a new Group, which becomes the selection. Ctrl+Shift+G: the selected parents
// dissolve and their children are selected.
void group_selection(EditorScene& scene) {
    if (scene.selection().empty()) return;
    Array<EntityId> ids;  // a copy: the edits below change the selection
    for (EntityId id : scene.selection().items()) ids.push_back(id);
    const EntityId g = group_entities(scene, ids.span());
    if (g != kNoEntity) scene.selection().set(g);
}

void ungroup_selection(EditorScene& scene) {
    Array<EntityId> ids;
    for (EntityId id : scene.selection().items()) ids.push_back(id);
    const Array<EntityId> freed = ungroup_entities(scene, ids.span());
    if (freed.empty()) return;
    scene.selection().set(freed[0]);
    for (usize i = 1; i < freed.size(); ++i) scene.selection().toggle(freed[i]);
}

// Replaces the selection with the entities `relation` names around it; nothing related leaves it alone.
void select_around(EditorScene& scene, Relation relation) {
    const Array<EntityId> ids = select_related(scene, scene.selection().items(), relation);
    if (ids.empty()) return;
    scene.selection().set(ids[0]);
    for (usize i = 1; i < ids.size(); ++i) scene.selection().toggle(ids[i]);
}

// Replaces the selection with `ids` (the first becomes the anchor); an empty list clears it.
void select_ids(EditorScene& scene, const Array<EntityId>& ids) {
    scene.selection().clear();
    if (ids.empty()) return;
    scene.selection().set(ids[0]);
    for (usize i = 1; i < ids.size(); ++i) scene.selection().toggle(ids[i]);
}

MCE_CVAR(cv_autosave_interval, "editor.autosave_interval", 300.0f,
         "Seconds of unsaved changes before the editor autosaves (0 = only on focus loss)", CVarFlags::None);
MCE_CVAR(cv_hot_reload, "editor.hot_reload_interval", 0.5f,
         "Seconds between polls of assets/ for hot reload (0 = off)", CVarFlags::None);
MCE_CVAR(cv_player, "editor.player", "", "mce_player executable for Play (empty: next to the editor / build tree)",
         CVarFlags::None);

constexpr const char* kDockspace = "mce.dockspace";
constexpr f32 kClearColor[4] = {0.06f, 0.06f, 0.07f, 1.0f};

}  // namespace

EditorShell::EditorShell() = default;

EditorShell::~EditorShell() { shutdown(); }

ResultV EditorShell::init(const EditorShellDesc& desc) {
    MCE_ASSERT_MSG(desc.window && desc.device, "EditorShell needs a window and a device");
    window_ = desc.window;
    device_ = desc.device;
    impl_ = mem::make<Impl>(mem::heap());

    IMGUI_CHECKVERSION();
    impl_->context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    // Multi-viewport stays off (editor-tools.md binding context).
    ini_path_.clear();
    if (desc.ini_path) ini_path_.append(desc.ini_path);
    io.IniFilename = ini_path_.view().empty() ? nullptr : ini_path_.c_str();

    ImGui::StyleColorsDark();
    const f32 scale = window_->display_scale();
    if (scale > 0.0f && scale != 1.0f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale);
        style.FontScaleDpi = scale;
    }

    impl_->platform.init(*window_);
    MCE_TRY(impl_->renderer.init(*device_));
    MCE_TRY(world_.init(*device_));
    MCE_TRY(preview_.init(*device_));
    session_.set_preview(&preview_);
    // Record mode (Timeline panel): inspector and gizmo edits are keyed into the open timeline.
    session_.scene().set_edit_observer(
        [](void* user, EditorScene& scene, EntityId id, const refl::TypeRecord& type, const void* before, const void* after) {
            static_cast<TimelineEditor*>(user)->record(scene, id, type, before, after);
        },
        &timeline_);
    MCE_LOG_INFO("editor: ImGui {} docking shell up ({} panels)", IMGUI_VERSION, panels_.size());
    return ok();
}

void EditorShell::shutdown() {
    if (!impl_) return;
    for (auto& p : panels_) p->shutdown(*this);
    panels_.clear();
    session_.scene().set_edit_observer(nullptr, nullptr);
    impl_->player.kill();
    impl_->player.wait();
    session_.set_preview(nullptr);
    preview_.shutdown();
    world_.shutdown();
    if (impl_->context) {
        impl_->renderer.shutdown();
        impl_->platform.shutdown();
        ImGui::DestroyContext(impl_->context);  // writes the layout file
    }
    mem::destroy(mem::heap(), impl_);
    impl_ = nullptr;
}

bool EditorShell::on_event(const platform::Event& e) {
    // Leaving the editor window is a natural checkpoint: autosave unsaved work (M4).
    if (e.type == platform::EventType::FocusOut && scene_mode()) session_.autosave();
    const bool consumed = impl_ && impl_->platform.on_event(e);
    if (playing()) session_.play().push_event(game_event(e));
    return consumed;
}

platform::Event EditorShell::game_event(const platform::Event& e) const {
    // Play-in-editor input (FR-EDT-060): the game gets the keyboard while the viewport has focus,
    // the mouse while it is hovered, and gamepads always. Presses the editor
    // keeps are flagged ConsumedByEditor: input drops them but still honors their releases, so
    // nothing sticks when focus moves to a panel mid-press (audio-input-ui §4.6).
    using platform::EventType;
    bool to_game = true;
    switch (e.type) {
        case EventType::KeyDown:
        case EventType::TextInput: to_game = viewport_.focused; break;
        case EventType::MouseMove:
        case EventType::MouseButtonDown:
        case EventType::MouseWheel: to_game = viewport_.hovered; break;
        default: break;
    }
    platform::Event out = e;
    if (!to_game) out.flags |= platform::EventFlag::ConsumedByEditor;
    return out;
}

void EditorShell::add_panel(mem::UniquePtr<IEditorPanel> panel) { panels_.push_back(std::move(panel)); }

IEditorPanel* EditorShell::find_panel(StringView name) {
    for (auto& p : panels_)
        if (name == p->name()) return p.get();
    return nullptr;
}

f32 EditorShell::framerate() const { return impl_ ? ImGui::GetIO().Framerate : 0.0f; }
u32 EditorShell::imgui_vertex_count() const { return impl_ ? impl_->renderer.last_vertex_count() : 0; }
u32 EditorShell::imgui_clipped_vertex_count() const { return impl_ ? impl_->renderer.last_clipped_triangles() : 0; }

void EditorShell::build_ui(f64 frame_dt) {
    const f64 t0 = platform::now_seconds();
    perf_pending_ = FrameSample{};
    perf_pending_.frame_ms = static_cast<f32>(frame_dt * 1000.0);
    console_.poll();
    impl_->platform.new_frame(frame_dt);
    ImGui::NewFrame();

    // Hot reload first (a re-cooked texture swaps in this frame), then play-in-editor on wall time,
    // before the panels draw this frame's state.
    if (scene_mode() && cv_hot_reload.get_float() > 0.0f)
        session_.tick_hot_reload(platform::now_seconds(), static_cast<f64>(cv_hot_reload.get_float()));
    if (scene_mode()) {
        session_.play().advance(frame_dt);
        perf_pending_.sim_ms = static_cast<f32>(session_.play().last_advance_ms());
        perf_pending_.sim_ticks = session_.play().last_advance_ticks();
    }
    handle_shortcuts();
    track_recent_scenes();
    track_selection_history();
    draw_menu_bar();
    draw_dockspace();
    viewport_ = ViewportInfo{};  // the viewport panel republishes it when visible
    for (auto& p : panels_) {
        if (!p->open) continue;
        if (focus_panel_ && StringView(focus_panel_) == p->name()) ImGui::SetNextWindowFocus();
        if (ImGui::Begin(p->name(), &p->open)) p->draw(*this);
        ImGui::End();
    }
    if (frames_ >= 2) focus_panel_ = nullptr;  // held over the first frames: the default dock layout lands on frame 1
    // A drag in a widget is one undo step: coalescing ends when no widget is held any more.
    if (scene_mode() && !ImGui::IsAnyItemActive()) scene().seal_undo();
    draw_popups();
    if (show_demo_) ImGui::ShowDemoWindow(&show_demo_);
    ImGui::Render();
    if (asset::AssetSystem* a = session_.project().mounted()) a->end_frame();
    if (scene_mode()) {
        const f32 interval = cv_autosave_interval.get_float();
        if (interval > 0.0f) session_.tick_autosave(platform::now_seconds(), static_cast<f64>(interval));
    }
    if (impl_->player.started() && !impl_->player.running())
        MCE_LOG_INFO("editor: player exited ({})", impl_->player.exit_code());
    update_title();
    ++frames_;
    const f32 build_ms = static_cast<f32>((platform::now_seconds() - t0) * 1000.0);
    perf_pending_.ui_ms = build_ms > perf_pending_.sim_ms ? build_ms - perf_pending_.sim_ms : 0.0f;
}

// ---- file commands ------------------------------------------------------------------------------

void EditorShell::request_quit() {
    if (scene_mode() && (scene().dirty() || tabs_.any_dirty(session_)) && !quit_requested_) {
        popup_ = Popup::ConfirmQuit;
        return;
    }
    quit_requested_ = true;
}

bool EditorShell::open_project(StringView root) {
    if (ui_document_.dirty()) (void)ui_document_.save(session_);
    ui_document_.close();
    timeline_preview_.stop(session_);
    if (timeline_.dirty()) (void)timeline_.save(session_);
    timeline_.close();
    if (anim_graph_.dirty()) (void)anim_graph_.save(session_);
    anim_graph_.close();
    if (bt_.dirty()) (void)bt_.save(session_);
    bt_.close();
    if (skeleton2d_.dirty()) (void)skeleton2d_.save(session_);
    skeleton2d_.close();
    material_request_ = asset::Guid{};
    particle_request_ = asset::Guid{};
    sequence_request_ = asset::Guid{};
    tabs_.clear();
    if (Result<void> r = session_.open_project(root); !r) {
        FixedString<512> msg;
        format_error(r.error(), msg);
        MCE_LOG_ERROR("editor: cannot open project {}: {}", root, msg.view());
        return false;
    }
    if (!session_.project().last_report().ok()) focus_panel("Cook");
    frame_scene();
    return true;
}

void EditorShell::open_ui_document(asset::Guid guid) {
    if (ui_document_.dirty() && guid != ui_document_.guid()) (void)ui_document_.save(session_);
    if (!ui_document_.open(session_, guid)) {
        MCE_LOG_ERROR("editor: cannot open the UI document: {}", ui_document_.last_error());
        return;
    }
    if (IEditorPanel* p = find_panel("UI Document")) p->open = true;
    focus_panel("UI Document");
}

void EditorShell::open_timeline(asset::Guid guid) {
    if (timeline_.dirty() && guid != timeline_.guid()) (void)timeline_.save(session_);
    if (guid != timeline_.guid()) timeline_preview_.stop(session_);
    if (!timeline_.open(session_, guid)) {
        MCE_LOG_ERROR("editor: cannot open the timeline: {}", timeline_.last_error());
        return;
    }
    if (IEditorPanel* p = find_panel("Timeline")) p->open = true;
    focus_panel("Timeline");
}

void EditorShell::open_anim_graph(asset::Guid guid) {
    if (anim_graph_.dirty() && guid != anim_graph_.guid()) (void)anim_graph_.save(session_);
    if (!anim_graph_.open(session_, guid)) {
        MCE_LOG_ERROR("editor: cannot open the animation graph: {}", anim_graph_.last_error());
        return;
    }
    if (IEditorPanel* p = find_panel("Animation")) p->open = true;
    focus_panel("Animation");
}

void EditorShell::open_bt(asset::Guid guid) {
    if (bt_.dirty() && guid != bt_.guid()) (void)bt_.save(session_);
    if (!bt_.open(session_, guid)) {
        MCE_LOG_ERROR("editor: cannot open the behavior tree: {}", bt_.last_error());
        return;
    }
    if (IEditorPanel* p = find_panel("Behavior Tree")) p->open = true;
    focus_panel("Behavior Tree");
}

void EditorShell::open_material(asset::Guid guid) {
    material_request_ = guid;
    if (IEditorPanel* p = find_panel("Material")) p->open = true;
    focus_panel("Material");
}

void EditorShell::open_particle(asset::Guid guid) {
    particle_request_ = guid;
    if (IEditorPanel* p = find_panel("Particle")) p->open = true;
    focus_panel("Particle");
}

void EditorShell::open_sequence(asset::Guid guid) {
    sequence_request_ = guid;
    if (IEditorPanel* p = find_panel("Sequence")) p->open = true;
    focus_panel("Sequence");
}

void EditorShell::open_skeleton2d(asset::Guid guid) {
    if (skeleton2d_.dirty() && guid != skeleton2d_.guid()) (void)skeleton2d_.save(session_);
    if (!skeleton2d_.open(session_, guid)) {
        MCE_LOG_ERROR("editor: cannot open the 2D skeleton: {}", skeleton2d_.last_error());
        return;
    }
    if (IEditorPanel* p = find_panel("Skeleton")) p->open = true;
    focus_panel("Skeleton");
}

bool EditorShell::open_scene(StringView path) {
    cook::String why;
    if (!tabs_.open(session_, path, why)) {
        MCE_LOG_ERROR("editor: cannot open {}: {}", path, why.view());
        focus_panel("Cook");
        return false;
    }
    // Parked changes come back dirty on their own; an autosave of an earlier session is still offered.
    if (!session_.scene().dirty() && session_.has_recovery()) popup_ = Popup::Recover;
    frame_scene();
    return true;
}

// ---- run-in-player ------------------------------------------------------------------------------

namespace {

#if defined(_WIN32)
constexpr const char* kPlayerExe = "mce_player.exe";
#else
constexpr const char* kPlayerExe = "mce_player";
#endif

// -editor.player, else next to the editor (shipped layout), else the build tree's src/player.
bool find_player(FixedString<512>& out) {
    out.clear();
    if (!cv_player.get_string().empty()) {
        out.append(cv_player.get_string());
        return platform::file_exists(out.view());
    }
    const char* candidates[] = {"", "../../player/", "../player/"};
    for (const char* rel : candidates) {
        out.clear();
        out.append(platform::base_path());
        out.append(rel);
        out.append(kPlayerExe);
        if (platform::file_exists(out.view())) return true;
    }
    return false;
}

}  // namespace

bool EditorShell::player_running() const { return impl_ && impl_->player.started(); }

void EditorShell::stop_player() {
    if (!player_running()) return;
    impl_->player.kill();
    impl_->player.wait();
    MCE_LOG_INFO("editor: player stopped");
}

bool EditorShell::play() {
    if (!scene_mode()) return false;
    if (scene().path().empty()) {
        MCE_LOG_WARN("editor: save the scene once before Play");
        popup_ = Popup::SaveAs;
        return false;
    }
    if (scene().dirty() && !save_scene()) return false;
    const cook::CookReport& r = session_.cook(true);
    if (!r.ok()) {
        MCE_LOG_ERROR("editor: Play needs a clean cook ({} error(s))", r.diags.error_count());
        focus_panel("Cook");
        return false;
    }
    FixedString<512> player;
    if (!find_player(player)) {
        MCE_LOG_ERROR("editor: mce_player not found (set -editor.player <path>)");
        return false;
    }
    stop_player();
    const cook::String pack = cook::path_join(session_.project().root(), "assets.mce");
    const asset::GuidString guid = asset::to_string(scene().doc().guid());
    const StringView args[] = {player.view(), "-pack", pack.view(), "-scene", guid.view()};
    if (Result<void> s = impl_->player.start(Span<const StringView>(args, 5)); !s) {
        MCE_LOG_ERROR("editor: cannot start {}", player.view());
        return false;
    }
    MCE_LOG_INFO("editor: playing {} in {}", scene().doc().name(), player.view());
    return true;
}

bool EditorShell::play_in_editor() {
    if (!scene_mode()) return false;
    if (!session_.enter_play()) {
        focus_panel("Cook");
        return false;
    }
    focus_panel("Viewport");
    return true;
}

void EditorShell::stop_play() { session_.stop_play(); }

// ---- clipboard ----------------------------------------------------------------------------------

void EditorShell::copy_selection() {
    if (!scene_mode() || scene().selection().empty()) return;
    cook::String text;
    scene().copy_entities(scene().selection().items(), text);
    ImGui::SetClipboardText(text.c_str());
}

void EditorShell::cut_selection() {
    copy_selection();
    Array<EntityId> ids;
    for (EntityId id : scene().selection().items()) ids.push_back(id);
    scene().begin_edit("Cut");
    for (EntityId id : ids) scene().destroy_entity(id);
    scene().end_edit();
}

void EditorShell::paste() {
    if (!scene_mode()) return;
    const char* text = ImGui::GetClipboardText();
    if (!text || !*text) return;
    // After the anchor, else at the end.
    const i32 anchor = scene().doc().index_of(scene().selection().anchor());
    cook::DiagList diags;
    if (!scene().paste_entities(StringView(text), anchor < 0 ? -1 : anchor + 1, diags))
        MCE_LOG_WARN("editor: the clipboard holds no entities");
}

void EditorShell::duplicate_selection() {
    if (!scene_mode() || scene().selection().empty()) return;
    Array<EntityId> ids;
    for (EntityId id : scene().selection().items()) ids.push_back(id);
    scene().duplicate_entities(ids.span());
}

bool EditorShell::save_scene() {
    if (!scene_mode()) return false;
    if (scene().path().empty()) {
        popup_ = Popup::SaveAs;
        return false;
    }
    if (Result<void> r = session_.save_scene(); !r) {
        FixedString<512> msg;
        format_error(r.error(), msg);
        MCE_LOG_ERROR("editor: save failed: {}", msg.view());
        return false;
    }
    return true;
}

void EditorShell::new_scene() {
    if (scene().dirty()) {
        popup_ = Popup::ConfirmNew;
        return;
    }
    session_.new_scene("untitled");
    frame_scene();
}

void EditorShell::cook(bool pack) {
    if (!scene_mode()) return;
    const cook::CookReport& r = session_.cook(pack);
    if (!r.ok()) focus_panel("Cook");
}

void EditorShell::update_title() {
    FixedString<512> t;
    t.append("Mesh Control Engine - Editor");
    if (scene_mode()) {
        t.append(" - ");
        StringView p = scene().path();
        const usize slash = p.find_last_of("/\\");
        t.append(p.empty() ? scene().doc().name() : (slash == StringView::npos ? p : p.substr(slash + 1)));
        if (scene().dirty()) t.append("*");
    }
    if (t.view() != title_.view()) {
        title_ = t;
        window_->set_title(title_.c_str());
    }
}

void EditorShell::handle_shortcuts() {
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Q)) request_quit();
    if (popup_ == Popup::None && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_P)) popup_ = Popup::QuickOpen;
    if (!scene_mode()) return;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) save_scene();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S)) popup_ = Popup::SaveAs;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) popup_ = Popup::OpenScene;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) new_scene();
    if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) cook();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F5)) {
        if (player_running()) stop_player();
        else play();
    } else if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        if (playing()) stop_play();
        else play_in_editor();
    }
    if (playing()) {
        if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) {
            PlaySession& p = session_.play();
            if (p.state() == PlaySession::State::Paused) p.resume();
            else p.pause();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F10, false)) session_.play().step();
        return;  // the document is locked while playing
    }
    // Entity clipboard: only while no text field owns the keyboard.
    if (ImGui::GetIO().WantTextInput) return;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) scene().undo();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z))
        scene().redo();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Alt | ImGuiKey_LeftArrow)) selection_back();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Alt | ImGuiKey_RightArrow)) selection_forward();
    // Ctrl+A selects every entity, Ctrl+I the ones that are not selected now.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) select_ids(scene(), select_all(scene()));
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_I)) select_ids(scene(), invert_selection(scene()));
    // Alt+Up selects the parents, Alt+Down the children, Alt+Shift+Down everything below.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Alt | ImGuiKey_UpArrow)) select_around(scene(), Relation::Parent);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Alt | ImGuiKey_DownArrow)) select_around(scene(), Relation::Children);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Alt | ImGuiMod_Shift | ImGuiKey_DownArrow))
        select_around(scene(), Relation::Descendants);
    // Selection sets: Ctrl+Shift+number stores the selection, Alt+number selects it again.
    for (u32 i = 0; i < SelectionSets::kSlots; ++i) {
        const ImGuiKey key = static_cast<ImGuiKey>(ImGuiKey_1 + static_cast<int>(i));
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | key))
            save_selection_set(i);
        else if (ImGui::IsKeyChordPressed(ImGuiMod_Alt | key))
            recall_selection_set(i);
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) copy_selection();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_X)) cut_selection();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V)) paste();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) duplicate_selection();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_G)) group_selection(scene());
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_G)) ungroup_selection(scene());
    // Camera bookmarks: Ctrl+1..9 stores the viewport's view, 1..9 flies back to it.
    if (viewport_.focused) {
        for (u32 i = 0; i < CameraBookmarks::kSlots; ++i) {
            const ImGuiKey key = static_cast<ImGuiKey>(ImGuiKey_1 + static_cast<int>(i));
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | key))
                save_bookmark(i);
            else if (ImGui::IsKeyChordPressed(key))
                recall_bookmark(i);
        }
    }
    if (ImGui::IsKeyChordPressed(ImGuiKey_W)) view_.gizmo = GizmoMode::Move;
    if (ImGui::IsKeyChordPressed(ImGuiKey_E)) view_.gizmo = GizmoMode::Rotate;
    if (ImGui::IsKeyChordPressed(ImGuiKey_R)) view_.gizmo = GizmoMode::Scale;
    if (ImGui::IsKeyChordPressed(ImGuiKey_T)) view_.tile_paint = !view_.tile_paint;
    if (ImGui::IsKeyChordPressed(ImGuiKey_O)) {
        view_.orbit_view = !view_.orbit_view;
        if (view_.orbit_view) frame_scene();
    }
}

// Keeps the Open Recent list of the open project: loaded when a project opens, the open scene's file moves to the
// front whenever it changes (opened, saved as), and the list is saved with it.
void EditorShell::track_recent_scenes() {
    if (!scene_mode()) {
        impl_->recent_root = cook::String();
        return;
    }
    cook::String file = session_.project().root();
    file += "/.mce/recent_scenes.txt";
    if (impl_->recent_root.view() != session_.project().root()) {
        impl_->recent_root = session_.project().root();
        impl_->recent_last = cook::String();
        impl_->recent.clear();
        cook::String text;
        if (cook::fs::read_text(file.view(), text)) impl_->recent.parse(text.view());
        impl_->bookmarks.clear_all();
        cook::String marks;
        marks = session_.project().root();
        marks += "/.mce/camera_bookmarks.txt";
        if (cook::fs::read_text(marks.view(), text)) impl_->bookmarks.parse(text.view());
    }
    const StringView cur = scene().path();
    if (cur.empty() || cur == impl_->recent_last.view()) return;
    impl_->recent_last = cur;
    impl_->recent.touch(cur);
    const cook::String text = impl_->recent.serialize();
    if (Result<void> r = cook::fs::write_file_atomic(
            file.view(), Span<const byte>(reinterpret_cast<const byte*>(text.view().data()), text.view().size()));
        !r)
        MCE_LOG_WARN("editor: cannot save the recent scenes list");
}

// Feeds the selection's anchor to the history; the history starts over when another scene becomes the open one.
void EditorShell::track_selection_history() {
    if (!scene_mode()) {
        impl_->sel_history.clear();
        impl_->sel_sets.clear_all();
        impl_->sel_scene = nullptr;
        return;
    }
    if (impl_->sel_scene != &scene()) {
        impl_->sel_history.clear();
        impl_->sel_sets.clear_all();
        impl_->sel_scene = &scene();
    }
    impl_->sel_history.visit(scene().selection().anchor());
}

void EditorShell::selection_back() {
    if (!scene_mode()) return;
    const SceneDocument& doc = scene().doc();
    const EntityId id = impl_->sel_history.back([&](EntityId e) { return doc.find(e) != nullptr; });
    if (id != kNoEntity) scene().selection().set(id);
}

void EditorShell::selection_forward() {
    if (!scene_mode()) return;
    const SceneDocument& doc = scene().doc();
    const EntityId id = impl_->sel_history.forward([&](EntityId e) { return doc.find(e) != nullptr; });
    if (id != kNoEntity) scene().selection().set(id);
}

// Gives the selected entities names from `pattern`, numbered from `first` in document order; one undo step.
void EditorShell::rename_selection(StringView pattern, u32 first) {
    if (!scene_mode()) return;
    EditorScene& s = scene();
    Array<EntityId> ids;
    for (const DocEntity& e : s.doc().entities())
        if (s.selection().contains(e.id)) ids.push_back(e.id);
    if (ids.empty()) return;
    s.begin_edit("Rename selection");
    u32 n = 0;
    for (const EntityId id : ids) {
        const DocEntity* e = s.doc().find(id);
        if (!e) continue;
        const FixedString<64> name = batch_rename_expand(pattern, e->name.view(), n++, first);
        s.rename_entity(id, name.view());
    }
    s.end_edit();
}

void EditorShell::save_selection_set(u32 slot) {
    if (scene_mode()) impl_->sel_sets.save(slot, scene().selection().items());
}

void EditorShell::recall_selection_set(u32 slot) {
    if (!scene_mode()) return;
    const SceneDocument& doc = scene().doc();
    const Array<EntityId> ids = impl_->sel_sets.recall(slot, [&](EntityId e) { return doc.find(e) != nullptr; });
    if (ids.empty()) return;
    scene().selection().set(ids[0]);
    for (usize i = 1; i < ids.size(); ++i) scene().selection().toggle(ids[i]);
}

void EditorShell::save_bookmark(u32 slot) {
    if (!scene_mode()) return;
    impl_->bookmarks.save(slot, camera_, orbit_);
    cook::String file = session_.project().root();
    file += "/.mce/camera_bookmarks.txt";
    const cook::String text = impl_->bookmarks.serialize();
    if (Result<void> r = cook::fs::write_file_atomic(
            file.view(), Span<const byte>(reinterpret_cast<const byte*>(text.view().data()), text.view().size()));
        !r)
        MCE_LOG_WARN("editor: cannot save the camera bookmarks");
}

void EditorShell::recall_bookmark(u32 slot) {
    if (scene_mode()) impl_->bookmarks.recall(slot, camera_, orbit_);
}

void EditorShell::request_open_scene(StringView path) {
    if (scene().dirty() && scene().path().empty()) {  // a scene with a file parks in its tab; this one has none
        pending_open_.assign(path);
        popup_ = Popup::ConfirmOpen;
        return;
    }
    open_scene(path);
}

namespace {

// The Quick Open commands (the ids of QuickOpenKind::Command items), in menu order.
enum QuickCommand : u64 {
    kCmdSave,
    kCmdNew,
    kCmdCook,
    kCmdPlay,
    kCmdStop,
    kCmdUndo,
    kCmdRedo,
    kCmdFrame,
    kCmdRescan,
    kCmdLayout
};

}  // namespace

void EditorShell::fill_quick_open() {
    QuickOpen& q = impl_->quick;
    q.clear();
    const bool proj = scene_mode();
    const bool edit = proj && !playing();
    if (edit) {
        for (usize i = 0; i < session_.project().assets().size(); ++i)
            q.add(QuickOpenKind::Asset, session_.project().assets()[i].path.view(), i);
        for (const DocEntity& e : scene().doc().entities()) q.add(QuickOpenKind::Entity, e.name.view(), e.id);
    }
    for (usize i = 0; i < panels_.size(); ++i) q.add(QuickOpenKind::Panel, panels_[i]->name(), i);
    if (edit) {
        q.add(QuickOpenKind::Command, "Save Scene", kCmdSave);
        q.add(QuickOpenKind::Command, "New Scene", kCmdNew);
        q.add(QuickOpenKind::Command, "Cook", kCmdCook);
        q.add(QuickOpenKind::Command, "Play in editor", kCmdPlay);
        if (scene().can_undo()) q.add(QuickOpenKind::Command, "Undo", kCmdUndo);
        if (scene().can_redo()) q.add(QuickOpenKind::Command, "Redo", kCmdRedo);
        q.add(QuickOpenKind::Command, "Frame scene", kCmdFrame);
        q.add(QuickOpenKind::Command, "Rescan assets", kCmdRescan);
    }
    if (proj && playing()) q.add(QuickOpenKind::Command, "Stop play", kCmdStop);
    q.add(QuickOpenKind::Command, "Reset layout", kCmdLayout);
}

void EditorShell::run_quick_open(const QuickOpenItem& item) {
    switch (item.kind) {
        case QuickOpenKind::Asset: {
            const auto assets = session_.project().assets();
            if (item.id >= assets.size()) return;
            const ProjectAsset& a = assets[static_cast<usize>(item.id)];
            asset::Guid open;
            open_asset_in_editor(*this, a, open);
            if (!open.is_null()) request_open_scene(a.path.view());
            break;
        }
        case QuickOpenKind::Entity:
            scene().selection().set(static_cast<EntityId>(item.id));
            break;
        case QuickOpenKind::Panel:
            if (IEditorPanel* p = panel_at(static_cast<usize>(item.id))) {
                p->open = true;
                focus_panel(p->name());
            }
            break;
        case QuickOpenKind::Command:
            switch (item.id) {
                case kCmdSave:
                    save_scene();
                    break;
                case kCmdNew:
                    new_scene();
                    break;
                case kCmdCook:
                    cook(false);
                    break;
                case kCmdPlay:
                    play_in_editor();
                    break;
                case kCmdStop:
                    stop_play();
                    break;
                case kCmdUndo:
                    scene().undo();
                    break;
                case kCmdRedo:
                    scene().redo();
                    break;
                case kCmdFrame:
                    frame_scene();
                    break;
                case kCmdRescan:
                    session_.project().rescan();
                    break;
                case kCmdLayout:
                    reset_layout();
                    break;
                default:
                    break;
            }
            break;
    }
}

void EditorShell::draw_popups() {
    static constexpr const char* kIds[] = {"",
                                           "Open Scene",
                                           "Save Scene As",
                                           "Unsaved changes##quit",
                                           "Unsaved changes##new",
                                           "Unsaved changes##open",
                                           "Recover unsaved changes",
                                           "Quick Open",
                                           "Rename Selection"};
    if (popup_ != Popup::None && !popup_opened_) {
        ImGui::OpenPopup(kIds[static_cast<int>(popup_)]);
        popup_opened_ = true;
    }
    const auto close = [this] {
        ImGui::CloseCurrentPopup();
        popup_ = Popup::None;
        popup_opened_ = false;
    };
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Open Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        Array<const ProjectAsset*> scenes;
        session_.list_scenes(scenes);
        if (scenes.empty()) ImGui::TextDisabled("No .mcscene files under assets/ yet.");
        // Prefab assets open the same way (FR-EDT-080): entity 0 is the root; saving re-cooks
        // every scene that uses it.
        for (const ProjectAsset& a : session_.project().assets())
            if (a.type == asset::ChunkType::Prefab) scenes.push_back(&a);
        for (const ProjectAsset* a : scenes) {
            if (ImGui::Selectable(a->path.c_str(), false, ImGuiSelectableFlags_NoAutoClosePopups)) {
                if (scene().dirty() && scene().path().empty()) {
                    pending_open_.assign(a->path.view());
                    close();
                    popup_ = Popup::ConfirmOpen;
                    break;
                }
                open_scene(a->path.view());
                close();
                break;
            }
        }
        ImGui::Separator();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) close();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Save Scene As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Path relative to assets/:");
        if (ImGui::IsWindowAppearing()) {
            if (save_as_buf_[0] == 0)
                std::snprintf(save_as_buf_, sizeof(save_as_buf_), "%s",
                              scene().doc().kind() == DocKind::Prefab ? "prefabs/untitled.mcprefab" : "scenes/untitled.mcscene");
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24.0f);
        const bool enter = ImGui::InputText("##path", save_as_buf_, sizeof(save_as_buf_), ImGuiInputTextFlags_EnterReturnsTrue);
        const StringView path(save_as_buf_);
        const StringView ext = scene().doc().kind() == DocKind::Prefab ? StringView(".mcprefab") : StringView(".mcscene");
        const bool valid = path.size() > ext.size() && path.substr(path.size() - ext.size()) == ext;
        if (!valid) ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "The file must end in %.*s", static_cast<int>(ext.size()), ext.data());
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button("Save") || (enter && valid)) {
            if (Result<void> r = session_.save_scene_as(path); !r) {
                FixedString<512> msg;
                format_error(r.error(), msg);
                MCE_LOG_ERROR("editor: save failed: {}", msg.view());
            }
            close();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) close();
        ImGui::EndPopup();
    }

    // Save / Discard / Cancel before an action that drops the open scene.
    for (int which = 3; which <= 5; ++which) {
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(kIds[which], nullptr, ImGuiWindowFlags_AlwaysAutoResize)) continue;
        if (scene().dirty()) ImGui::Text("'%s' has unsaved changes.", scene().doc().name().data());
        else ImGui::TextUnformatted("Other open scenes have unsaved changes (kept in their autosave files).");
        const auto proceed = [&] {
            const Popup p = popup_;
            close();
            if (p == Popup::ConfirmQuit) quit_requested_ = true;
            if (p == Popup::ConfirmNew) {
                session_.new_scene("untitled");
                frame_scene();
            }
            if (p == Popup::ConfirmOpen) {
                if (scene().path().empty()) session_.new_scene("untitled");  // the untitled draft was saved or discarded
                open_scene(pending_open_.view());
            }
        };
        if (ImGui::Button("Save")) {
            if (save_scene()) {
                proceed();
            } else {  // no file yet: Save As takes over; the pending action is dropped
                close();
                if (scene().path().empty()) popup_ = Popup::SaveAs;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            session_.discard_autosave();
            proceed();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) close();
        ImGui::EndPopup();
    }

    // Ctrl+P: type to search assets, entities, panels and commands; Up/Down pick, Enter runs, Esc closes.
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(center.x, vp->Pos.y + vp->Size.y * 0.15f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f));
        if (ImGui::BeginPopup("Quick Open", ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize)) {
            QuickOpen& q = impl_->quick;
            if (ImGui::IsWindowAppearing()) {
                fill_quick_open();
                impl_->quick_query[0] = '\0';
                impl_->quick_sel = 0;
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##query", impl_->quick_query, sizeof(impl_->quick_query))) impl_->quick_sel = 0;
            Array<u32> hits;
            q.search(StringView(impl_->quick_query), 12, hits);
            const i32 n = static_cast<i32>(hits.size());
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && n > 0) impl_->quick_sel = (impl_->quick_sel + 1) % n;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && n > 0) impl_->quick_sel = (impl_->quick_sel + n - 1) % n;
            if (impl_->quick_sel >= n) impl_->quick_sel = n > 0 ? n - 1 : 0;
            if (n == 0) ImGui::TextDisabled("(no match)");
            i32 run = -1;
            for (i32 i = 0; i < n; ++i) {
                const QuickOpenItem& it = q.items()[hits[static_cast<usize>(i)]];
                ImGui::PushID(i);
                if (ImGui::Selectable(it.label.c_str(), i == impl_->quick_sel, ImGuiSelectableFlags_NoAutoClosePopups))
                    run = i;
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
                ImGui::TextDisabled("%s", quick_open_kind_name(it.kind));
                ImGui::PopID();
            }
            if (n > 0 && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
                run = impl_->quick_sel;
            if (run >= 0) {
                const QuickOpenItem item = q.items()[hits[static_cast<usize>(run)]];
                close();
                run_quick_open(item);
            } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                close();
            }
            ImGui::EndPopup();
        } else if (popup_ == Popup::QuickOpen && popup_opened_) {
            popup_ = Popup::None;  // a click outside closed it
            popup_opened_ = false;
        }
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Rename Selection", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) {
            std::snprintf(impl_->rename_pattern, sizeof impl_->rename_pattern, "%s", "*_##");
            impl_->rename_start = 1;
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::TextUnformatted("Pattern: ## is the number, * the current name");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        const bool enter = ImGui::InputText("##pattern", impl_->rename_pattern, sizeof impl_->rename_pattern,
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        ImGui::InputInt("First number", &impl_->rename_start);
        if (impl_->rename_start < 0) impl_->rename_start = 0;
        const StringView pattern(impl_->rename_pattern);
        const u32 first = static_cast<u32>(impl_->rename_start);
        u32 n = 0;
        for (const DocEntity& e : scene().doc().entities()) {
            if (!scene().selection().contains(e.id)) continue;
            if (n < 4)
                ImGui::TextDisabled("%s -> %s", e.name.c_str(),
                                    batch_rename_expand(pattern, e.name.view(), n, first).c_str());
            ++n;
        }
        if (n > 4) ImGui::TextDisabled("... and %u more", n - 4);
        const bool valid = !pattern.empty() && n > 0;
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button("Rename") || (enter && valid)) {
            rename_selection(pattern, first);
            close();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) close();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Recover unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const cook::String path = session_.autosave_path();
        ImGui::TextUnformatted("The editor closed without saving this scene.");
        ImGui::TextDisabled("%s", path.c_str());
        if (ImGui::Button("Recover")) {
            session_.recover();
            close();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            session_.discard_autosave();
            close();
        }
        ImGui::SameLine();
        if (ImGui::Button("Later") || ImGui::IsKeyPressed(ImGuiKey_Escape)) close();
        ImGui::EndPopup();
    }
}

void EditorShell::record(rhi::CommandBuffer& cmd, f32 alpha) {
    const f64 t0 = platform::now_seconds();
    for (auto& p : panels_) p->record(*this, cmd, alpha);
    ImDrawData* dd = ImGui::GetDrawData();
    impl_->renderer.update_textures(dd);
    impl_->renderer.record(dd, cmd, kClearColor);
    perf_pending_.record_ms = static_cast<f32>((platform::now_seconds() - t0) * 1000.0);
}

void EditorShell::end_frame_perf(f64 submit_ms) {
    perf_pending_.submit_ms = static_cast<f32>(submit_ms);
    // GPU: the span between the first and last timestamp the frame wrote (none: not measured).
    const rhi::GpuTimestamps& ts = device_->last_gpu_timestamps();
    perf_pending_.gpu_ms = -1.0f;
    if (ts.written != 0) {
        u32 first = ~0u, last = 0;
        for (u32 i = 0; i < rhi::kMaxTimestamps; ++i)
            if (ts.has(i)) {
                if (first == ~0u) first = i;
                last = i;
            }
        if (first != ~0u && last > first) perf_pending_.gpu_ms = static_cast<f32>(ts.ms(first, last));
    }
    perf_.push(perf_pending_);
}

void EditorShell::draw_menu_bar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        const bool proj = scene_mode();
        if (ImGui::MenuItem("New Scene", "Ctrl+N", false, proj)) new_scene();
        if (ImGui::MenuItem("Open Scene...", "Ctrl+O", false, proj)) popup_ = Popup::OpenScene;
        if (ImGui::BeginMenu("Open Recent", proj && !impl_->recent.items().empty())) {
            cook::String chosen;
            for (const cook::String& path : impl_->recent.items()) {
                cook::String rel;
                const bool has_rel = session_.project().relative_asset_path(path.view(), rel);
                if (ImGui::MenuItem(has_rel ? rel.c_str() : path.c_str())) chosen = path;
            }
            if (!chosen.empty()) {
                if (cook::fs::exists(chosen.view())) {
                    request_open_scene(chosen.view());
                } else {  // deleted or moved since: forget it
                    MCE_LOG_WARN("editor: {} no longer exists", chosen.view());
                    impl_->recent.remove(chosen.view());
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, proj)) save_scene();
        if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S", false, proj)) popup_ = Popup::SaveAs;
        ImGui::Separator();
        if (ImGui::MenuItem("Cook", "F7", false, proj)) cook(false);
        if (ImGui::MenuItem("Cook + Pack", nullptr, false, proj)) cook(true);
        if (ImGui::MenuItem("Rescan assets", nullptr, false, proj)) session_.project().rescan();
        if (ImGui::MenuItem("Hot reload now", nullptr, false, proj)) session_.hot_reload();
        if (ImGui::MenuItem("Bake reflection probes", nullptr, false, proj && !playing())) {
            if (Result<u32> r = session_.bake_reflection_probes(); !r) {
                FixedString<512> why;
                format_error(r.error(), why);
                MCE_LOG_ERROR("editor: reflection probe bake failed: {}", why.view());
                focus_panel("Console");
            }
        }
        if (ImGui::MenuItem("Bake GI volume", nullptr, false, proj && !playing())) {
            if (Result<void> r = session_.bake_gi_volume(); !r) {
                FixedString<512> why;
                format_error(r.error(), why);
                MCE_LOG_ERROR("editor: GI volume bake failed: {}", why.view());
                focus_panel("Console");
            }
        }
        if (ImGui::MenuItem("Bake navmesh", nullptr, false, proj && !playing())) {
            if (Result<physics::NavBakeStats3D> r = session_.bake_navmesh(); !r) {
                FixedString<512> why;
                format_error(r.error(), why);
                MCE_LOG_ERROR("editor: navmesh bake failed: {}", why.view());
                focus_panel("Console");
            }
        }
        if (ImGui::MenuItem("Play in editor", "F5", false, proj && !playing())) play_in_editor();
        if (ImGui::MenuItem("Run in player", "Ctrl+F5", false, proj && !player_running())) play();
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) request_quit();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit", scene_mode() && !playing())) {
        const bool sel = !scene().selection().empty();
        FixedString<96> undo_text, redo_text;
        undo_text.append("Undo ");
        undo_text.append(scene().undo_label());
        redo_text.append("Redo ");
        redo_text.append(scene().redo_label());
        if (ImGui::MenuItem(undo_text.c_str(), "Ctrl+Z", false, scene().can_undo())) scene().undo();
        if (ImGui::MenuItem(redo_text.c_str(), "Ctrl+Y", false, scene().can_redo())) scene().redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Previous selection", "Alt+Left", false, impl_->sel_history.can_back())) selection_back();
        if (ImGui::MenuItem("Next selection", "Alt+Right", false, impl_->sel_history.can_forward()))
            selection_forward();
        if (ImGui::MenuItem("Rename selection...", nullptr, false, sel)) popup_ = Popup::BatchRename;
        if (ImGui::MenuItem("Make names unique")) unique_names_in(scene());
        if (ImGui::BeginMenu("Align", scene().selection().items().size() >= 2)) {
            if (ImGui::MenuItem("Left")) align_selection(scene(), AlignAxis::X, AlignMode::Min);
            if (ImGui::MenuItem("Center horizontally")) align_selection(scene(), AlignAxis::X, AlignMode::Center);
            if (ImGui::MenuItem("Right")) align_selection(scene(), AlignAxis::X, AlignMode::Max);
            if (ImGui::MenuItem("Bottom")) align_selection(scene(), AlignAxis::Y, AlignMode::Min);
            if (ImGui::MenuItem("Center vertically")) align_selection(scene(), AlignAxis::Y, AlignMode::Center);
            if (ImGui::MenuItem("Top")) align_selection(scene(), AlignAxis::Y, AlignMode::Max);
            ImGui::Separator();
            const bool three = scene().selection().items().size() >= 3;
            if (ImGui::MenuItem("Distribute horizontally", nullptr, false, three))
                align_selection(scene(), AlignAxis::X, AlignMode::Min, true);
            if (ImGui::MenuItem("Distribute vertically", nullptr, false, three))
                align_selection(scene(), AlignAxis::Y, AlignMode::Min, true);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Select")) {
            if (ImGui::MenuItem("All", "Ctrl+A")) select_ids(scene(), select_all(scene()));
            if (ImGui::MenuItem("Invert", "Ctrl+I")) select_ids(scene(), invert_selection(scene()));
            ImGui::Separator();
            ImGui::BeginDisabled(!sel);
            if (ImGui::MenuItem("Parent", "Alt+Up")) select_around(scene(), Relation::Parent);
            if (ImGui::MenuItem("Children", "Alt+Down")) select_around(scene(), Relation::Children);
            if (ImGui::MenuItem("All descendants", "Alt+Shift+Down")) select_around(scene(), Relation::Descendants);
            if (ImGui::MenuItem("Siblings")) select_around(scene(), Relation::Siblings);
            if (ImGui::MenuItem("Similar")) select_ids(scene(), select_similar(scene()));
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Selection sets")) {
            for (u32 i = 0; i < SelectionSets::kSlots; ++i) {
                FixedString<32> label;
                char text[32];
                std::snprintf(text, sizeof text, "Set %u (%zu)", i + 1, impl_->sel_sets.size(i));
                label.append(text);
                char keys[24];
                std::snprintf(keys, sizeof keys, "Alt+%u", i + 1);
                if (ImGui::MenuItem(label.c_str(), keys, false, impl_->sel_sets.has(i))) recall_selection_set(i);
            }
            ImGui::TextDisabled("Ctrl+Shift+1..9 stores the selection");
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, sel)) cut_selection();
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, sel)) copy_selection();
        if (ImGui::MenuItem("Copy path", nullptr, false, sel)) copy_selection_paths(scene());
        if (ImGui::MenuItem("Reset transform", nullptr, false, sel)) reset_selection_transforms(scene());
        if (ImGui::MenuItem("Paste", "Ctrl+V")) paste();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, sel)) duplicate_selection();
        if (ImGui::MenuItem("Group", "Ctrl+G", false, sel)) group_selection(scene());
        if (ImGui::MenuItem("Ungroup", "Ctrl+Shift+G", false, sel)) ungroup_selection(scene());
        ImGui::Separator();
        if (ImGui::MenuItem("Move tool", "W", view_.gizmo == GizmoMode::Move)) view_.gizmo = GizmoMode::Move;
        if (ImGui::MenuItem("Rotate tool", "E", view_.gizmo == GizmoMode::Rotate)) view_.gizmo = GizmoMode::Rotate;
        if (ImGui::MenuItem("Scale tool", "R", view_.gizmo == GizmoMode::Scale)) view_.gizmo = GizmoMode::Scale;
        ImGui::MenuItem("Paint tiles", "T", &view_.tile_paint);
        ImGui::Separator();
        ImGui::MenuItem("Snap to grid", nullptr, &view_.snap);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        ImGui::DragFloat("Snap step", &view_.snap_step, 0.05f, 0.05f, 16.0f, "%.2f m");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        ImGui::DragFloat("Grid step", &view_.grid_step, 0.05f, 0.05f, 16.0f, "%.2f m");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        for (auto& p : panels_) ImGui::MenuItem(p->name(), nullptr, &p->open);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset layout")) reset_layout();
        ImGui::MenuItem("Grid", nullptr, &view_.show_grid, scene_mode());
        ImGui::MenuItem("Perception (while playing)", nullptr, &view_.show_perception, scene_mode());
        ImGui::MenuItem("Roads (while playing)", nullptr, &view_.show_roads, scene_mode());
        ImGui::MenuItem("Gameplay debug (while playing)", nullptr, &view_.show_gameplay, scene_mode());
        ImGui::MenuItem("Game camera while playing", nullptr, &view_.play_camera, scene_mode());
        if (ImGui::MenuItem("3D orbit view", "O", &view_.orbit_view, scene_mode()) && view_.orbit_view) frame_scene();
        ImGui::MenuItem("Orbit camera collision", nullptr, &view_.orbit_collision, scene_mode() && view_.orbit_view);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        ImGui::DragFloat("Collider depth", &view_.orbit_depth, 0.05f, 0.05f, 50.0f, "%.2f m");
        if (ImGui::MenuItem("Frame scene", "F", false, scene_mode())) frame_scene();
        if (ImGui::BeginMenu("Camera bookmarks", scene_mode())) {
            for (u32 i = 0; i < CameraBookmarks::kSlots; ++i) {
                char label[32], key[8];
                std::snprintf(label, sizeof(label), "Go to %u", i + 1);
                std::snprintf(key, sizeof(key), "%u", i + 1);
                if (ImGui::MenuItem(label, key, false, impl_->bookmarks.has(i))) recall_bookmark(i);
            }
            ImGui::Separator();
            for (u32 i = 0; i < CameraBookmarks::kSlots; ++i) {
                char label[32], key[16];
                std::snprintf(label, sizeof(label), "Save view as %u", i + 1);
                std::snprintf(key, sizeof(key), "Ctrl+%u", i + 1);
                if (ImGui::MenuItem(label, key)) save_bookmark(i);
            }
            ImGui::EndMenu();
        }
        ImGui::MenuItem("ImGui demo", nullptr, &show_demo_);
        ImGui::EndMenu();
    }
    if (scene_mode()) {
        // Play / Pause / Step / Stop, centred in the menu bar; the player run stays in the File menu.
        PlaySession& p = session_.play();
        const bool paused = p.state() == PlaySession::State::Paused;
        const char* run = !playing() ? "Play (F5)" : (paused ? "Resume (F6)" : "Pause (F6)");
        const ImGuiStyle& st = ImGui::GetStyle();
        f32 w = ImGui::CalcTextSize(run).x + st.FramePadding.x * 2.0f;
        if (playing()) w += ImGui::CalcTextSize("Step (F10)Stop (F5)").x + st.FramePadding.x * 4.0f + st.ItemSpacing.x * 2.0f;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - w) * 0.5f);
        if (ImGui::MenuItem(run)) {
            if (!playing()) play_in_editor();
            else if (paused) p.resume();
            else p.pause();
        }
        if (playing()) {
            if (ImGui::MenuItem("Step (F10)")) p.step();
            if (ImGui::MenuItem("Stop (F5)")) stop_play();
            ImGui::TextDisabled("tick %llu", static_cast<unsigned long long>(p.ticks()));
        } else if (player_running()) {
            if (ImGui::MenuItem("Stop player")) stop_player();
        }
    }
    ImGui::EndMainMenuBar();
}

void EditorShell::draw_dockspace() {
    const ImGuiID id = ImHashStr(kDockspace);
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    // First frame: keep a layout restored from the layout file, otherwise build the default one.
    if (!impl_->layout_checked) {
        impl_->layout_checked = true;
        const ImGuiDockNode* node = ImGui::DockBuilderGetNode(id);
        if (!node || !node->IsSplitNode()) reset_layout_ = true;
    }
    if (reset_layout_) {
        reset_layout_ = false;
        ImGui::DockBuilderRemoveNode(id);
        ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(id, vp->WorkSize);
        ImGuiID top = 0, bottom = 0, center = 0, right = 0, left = 0, mid = 0;
        ImGui::DockBuilderSplitNode(id, ImGuiDir_Down, 0.28f, &bottom, &top);
        ImGui::DockBuilderSplitNode(top, ImGuiDir_Right, 0.24f, &right, &center);
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.22f, &left, &mid);
        ImGui::DockBuilderDockWindow("Viewport", mid);
        ImGui::DockBuilderDockWindow("UI Document", mid);
        ImGui::DockBuilderDockWindow("Hierarchy", left);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Console", bottom);
        ImGui::DockBuilderDockWindow("Cook", bottom);
        ImGui::DockBuilderDockWindow("Stats", bottom);
        ImGui::DockBuilderDockWindow("Profiler", bottom);
        ImGui::DockBuilderDockWindow("Play", bottom);
        ImGui::DockBuilderDockWindow("Assets", bottom);
        ImGui::DockBuilderDockWindow("Tilemap", right);
        ImGui::DockBuilderDockWindow("Terrain", right);
        ImGui::DockBuilderDockWindow("Timeline", bottom);
        ImGui::DockBuilderDockWindow("Animation", bottom);
        ImGui::DockBuilderDockWindow("Behavior Tree", bottom);
        ImGui::DockBuilderDockWindow("Skeleton", right);
        ImGui::DockBuilderFinish(id);
        for (auto& p : panels_) p->open = true;
    }
    ImGui::DockSpaceOverViewport(id, vp, ImGuiDockNodeFlags_None);
}

}  // namespace mce::editor
