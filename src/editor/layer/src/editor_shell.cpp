#include "mce/editor/editor_shell.h"

#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder (default layout)

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
    MCE_LOG_INFO("editor: ImGui {} docking shell up ({} panels)", IMGUI_VERSION, panels_.size());
    return ok();
}

void EditorShell::shutdown() {
    if (!impl_) return;
    for (auto& p : panels_) p->shutdown(*this);
    panels_.clear();
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

    draw_menu_bar();
    draw_dockspace();
    viewport_.target = {};  // the viewport panel republishes it when visible
    for (auto& p : panels_) {
        if (!p->open) continue;
        if (ImGui::Begin(p->name(), &p->open)) p->draw(*this);
        ImGui::End();
    }
    if (show_demo_) ImGui::ShowDemoWindow(&show_demo_);
    ImGui::Render();
    ++frames_;
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
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) request_quit();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        for (auto& p : panels_) ImGui::MenuItem(p->name(), nullptr, &p->open);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset layout")) reset_layout();
        ImGui::MenuItem("ImGui demo", nullptr, &show_demo_);
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Q)) request_quit();
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
        ImGuiID top = 0, bottom = 0, center = 0, right = 0;
        ImGui::DockBuilderSplitNode(id, ImGuiDir_Down, 0.28f, &bottom, &top);
        ImGui::DockBuilderSplitNode(top, ImGuiDir_Right, 0.22f, &right, &center);
        ImGui::DockBuilderDockWindow("Viewport", center);
        ImGui::DockBuilderDockWindow("Console", bottom);
        ImGui::DockBuilderDockWindow("Stats", right);
        ImGui::DockBuilderFinish(id);
        for (auto& p : panels_) p->open = true;
    }
    ImGui::DockSpaceOverViewport(id, vp, ImGuiDockNodeFlags_None);
}

}  // namespace mce::editor
