#include "mce/editor/editor_shell.h"

#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder (default layout)

#include <cstdio>

#include "imgui_platform.h"
#include "imgui_renderer.h"
#include "mce/log.h"

namespace mce::editor {

struct EditorShell::Impl {
    ImGuiContext* context = nullptr;
    ImGuiPlatform platform;
    ImGuiRhiRenderer renderer;
    bool layout_checked = false;
};

namespace {

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
    MCE_LOG_INFO("editor: ImGui {} docking shell up ({} panels)", IMGUI_VERSION, panels_.size());
    return ok();
}

void EditorShell::shutdown() {
    if (!impl_) return;
    for (auto& p : panels_) p->shutdown(*this);
    panels_.clear();
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

bool EditorShell::on_event(const platform::Event& e) { return impl_ && impl_->platform.on_event(e); }

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
    console_.poll();
    impl_->platform.new_frame(frame_dt);
    ImGui::NewFrame();

    handle_shortcuts();
    draw_menu_bar();
    draw_dockspace();
    viewport_.target = {};  // the viewport panel republishes it when visible
    for (auto& p : panels_) {
        if (!p->open) continue;
        if (focus_panel_ && StringView(focus_panel_) == p->name()) ImGui::SetNextWindowFocus();
        if (ImGui::Begin(p->name(), &p->open)) p->draw(*this);
        ImGui::End();
    }
    focus_panel_ = nullptr;
    draw_popups();
    if (show_demo_) ImGui::ShowDemoWindow(&show_demo_);
    ImGui::Render();
    if (asset::AssetSystem* a = session_.project().mounted()) a->end_frame();
    update_title();
    ++frames_;
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

bool EditorShell::open_scene(StringView path) {
    if (!session_.open_scene(path)) {
        focus_panel("Cook");
        return false;
    }
    frame_scene();
    return true;
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
}

void EditorShell::draw_popups() {
    static constexpr const char* kIds[] = {"", "Open Scene", "Save Scene As", "Unsaved changes##quit",
                                           "Unsaved changes##new", "Unsaved changes##open"};
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
            if (save_as_buf_[0] == 0) std::snprintf(save_as_buf_, sizeof(save_as_buf_), "scenes/untitled.mcscene");
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24.0f);
        const bool enter = ImGui::InputText("##path", save_as_buf_, sizeof(save_as_buf_), ImGuiInputTextFlags_EnterReturnsTrue);
        const StringView path(save_as_buf_);
        const bool valid = path.size() > 8 && path.substr(path.size() - 8) == ".mcscene";
        if (!valid) ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "The file must end in .mcscene");
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
        if (ImGui::Button("Discard")) proceed();
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) close();
        ImGui::EndPopup();
    }
}

void EditorShell::record(rhi::CommandBuffer& cmd, f32 alpha) {
    for (auto& p : panels_) p->record(*this, cmd, alpha);
    ImDrawData* dd = ImGui::GetDrawData();
    impl_->renderer.update_textures(dd);
    impl_->renderer.record(dd, cmd, kClearColor);
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
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) request_quit();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        for (auto& p : panels_) ImGui::MenuItem(p->name(), nullptr, &p->open);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset layout")) reset_layout();
        ImGui::MenuItem("Grid", nullptr, &view_.show_grid, scene_mode());
        if (ImGui::MenuItem("Frame scene", "F", false, scene_mode())) frame_scene();
        ImGui::MenuItem("ImGui demo", nullptr, &show_demo_);
        ImGui::EndMenu();
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
        ImGui::DockBuilderDockWindow("Hierarchy", left);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Console", bottom);
        ImGui::DockBuilderDockWindow("Cook", bottom);
        ImGui::DockBuilderDockWindow("Stats", bottom);
        ImGui::DockBuilderFinish(id);
        for (auto& p : panels_) p->open = true;
    }
    ImGui::DockSpaceOverViewport(id, vp, ImGuiDockNodeFlags_None);
}

}  // namespace mce::editor
