#include "mce/editor/editor_shell.h"

#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder (default layout)

#include <cstdio>

#include "imgui_platform.h"
#include "imgui_renderer.h"
#include "mce/convar.h"
#include "mce/log.h"
#include "mce/platform/library.h"
#include "mce/platform/platform.h"
#include "mce/platform/process.h"

namespace mce::editor {

struct EditorShell::Impl {
    ImGuiContext* context = nullptr;
    ImGuiPlatform platform;
    ImGuiRhiRenderer renderer;
    platform::Process player;
    bool layout_checked = false;
};

namespace {

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
    if (scene_mode() && scene().dirty() && !quit_requested_) {
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
    if (!session_.open_scene(path)) {
        focus_panel("Cook");
        return false;
    }
    if (session_.has_recovery()) popup_ = Popup::Recover;
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
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) copy_selection();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_X)) cut_selection();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V)) paste();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) duplicate_selection();
    if (ImGui::IsKeyChordPressed(ImGuiKey_W)) view_.gizmo = GizmoMode::Move;
    if (ImGui::IsKeyChordPressed(ImGuiKey_E)) view_.gizmo = GizmoMode::Rotate;
    if (ImGui::IsKeyChordPressed(ImGuiKey_R)) view_.gizmo = GizmoMode::Scale;
    if (ImGui::IsKeyChordPressed(ImGuiKey_T)) view_.tile_paint = !view_.tile_paint;
    if (ImGui::IsKeyChordPressed(ImGuiKey_O)) {
        view_.orbit_view = !view_.orbit_view;
        if (view_.orbit_view) frame_scene();
    }
}

void EditorShell::request_open_scene(StringView path) {
    if (scene().dirty()) {
        pending_open_.assign(path);
        popup_ = Popup::ConfirmOpen;
        return;
    }
    open_scene(path);
}

void EditorShell::draw_popups() {
    static constexpr const char* kIds[] = {"", "Open Scene", "Save Scene As", "Unsaved changes##quit",
                                           "Unsaved changes##new", "Unsaved changes##open", "Recover unsaved changes"};
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
                if (scene().dirty()) {
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
        ImGui::Text("'%s' has unsaved changes.", scene().doc().name().data());
        const auto proceed = [&] {
            const Popup p = popup_;
            close();
            if (p == Popup::ConfirmQuit) quit_requested_ = true;
            if (p == Popup::ConfirmNew) {
                session_.new_scene("untitled");
                frame_scene();
            }
            if (p == Popup::ConfirmOpen) open_scene(pending_open_.view());
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
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, sel)) cut_selection();
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, sel)) copy_selection();
        if (ImGui::MenuItem("Paste", "Ctrl+V")) paste();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, sel)) duplicate_selection();
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
        ImGui::MenuItem("Game camera while playing", nullptr, &view_.play_camera, scene_mode());
        if (ImGui::MenuItem("3D orbit view", "O", &view_.orbit_view, scene_mode()) && view_.orbit_view) frame_scene();
        ImGui::MenuItem("Orbit camera collision", nullptr, &view_.orbit_collision, scene_mode() && view_.orbit_view);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        ImGui::DragFloat("Collider depth", &view_.orbit_depth, 0.05f, 0.05f, 50.0f, "%.2f m");
        if (ImGui::MenuItem("Frame scene", "F", false, scene_mode())) frame_scene();
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
