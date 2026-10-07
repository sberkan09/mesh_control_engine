#pragma once
// EditorShell — the M0 editor frame (editor-tools.md §2.2, §2.7, §2.10): ImGui docking shell with
// a panel registry, the viewport and console panels over a stub world, and dock-layout
// persistence. This header is ImGui-free: mce_editor.exe sees only this.
//
// Per frame (main thread, inside device.begin_frame/end_frame):
//   shell.build_ui(dt);              // ImGui frame: menu, dockspace, panels
//   shell.record(cmd, alpha);        // viewport world -> its render target, then ImGui -> backbuffer

#include "mce/editor/console_model.h"
#include "mce/editor/stub_world.h"
#include "mce/memory/allocator.h"
#include "mce/platform/window.h"
#include "mce/rhi/rhi.h"

namespace mce::editor {

class EditorShell;

class IEditorPanel {
public:
    virtual ~IEditorPanel() = default;
    // Window title; also the dock-layout file key, so keep it stable.
    virtual const char* name() const = 0;
    // Called inside ImGui::Begin(name()) while the panel is open and visible.
    virtual void draw(EditorShell& shell) = 0;
    // Called every frame after the ImGui frame is built, before the ImGui pass is recorded.
    virtual void record(EditorShell&, rhi::CommandBuffer&, f32 /*alpha*/) {}
    virtual void shutdown(EditorShell&) {}

    bool open = true;
};

struct EditorShellDesc {
    platform::Window* window = nullptr;
    rhi::RenderDevice* device = nullptr;
    // Dock layout + window state file; null or "" disables persistence (tests).
    const char* ini_path = "mce_editor.ini";
};

struct ViewportInfo {
    rhi::TextureHandle target;  // null while the panel is hidden
    rhi::Extent2D size;         // pixels
    bool hovered = false;
};

class EditorShell {
public:
    EditorShell();
    ~EditorShell();
    EditorShell(const EditorShell&) = delete;
    EditorShell& operator=(const EditorShell&) = delete;

    ResultV init(const EditorShellDesc& desc);
    void shutdown();

    // ImGui sees every event first; returns true when it consumed the event (editor-tools §2.10).
    bool on_event(const platform::Event& e);
    void tick(f64 dt) { world_.tick(dt); }
    void build_ui(f64 frame_dt);
    void record(rhi::CommandBuffer& cmd, f32 alpha);

    void add_panel(mem::UniquePtr<IEditorPanel> panel);
    IEditorPanel* find_panel(StringView name);
    usize panel_count() const { return panels_.size(); }

    void request_quit() { quit_requested_ = true; }
    bool quit_requested() const { return quit_requested_; }
    void reset_layout() { reset_layout_ = true; }

    rhi::RenderDevice& device() { return *device_; }
    platform::Window& window() { return *window_; }
    StubWorld& world() { return world_; }
    ConsoleModel& console() { return console_; }
    const ViewportInfo& viewport() const { return viewport_; }
    void set_viewport(const ViewportInfo& v) { viewport_ = v; }
    u64 frame_count() const { return frames_; }
    f32 framerate() const;
    u32 imgui_vertex_count() const;
    u32 imgui_clipped_vertex_count() const;

private:
    struct Impl;
    void draw_menu_bar();
    void draw_dockspace();

    Impl* impl_ = nullptr;
    platform::Window* window_ = nullptr;
    rhi::RenderDevice* device_ = nullptr;
    StubWorld world_;
    ConsoleModel console_;
    Array<mem::UniquePtr<IEditorPanel>> panels_;
    ViewportInfo viewport_;
    FixedString<256> ini_path_;
    u64 frames_ = 0;
    bool quit_requested_ = false;
    bool reset_layout_ = false;
    bool show_demo_ = false;
};

// Registers the M0 panel set: Viewport, Console, Stats.
void add_default_panels(EditorShell& shell);

}  // namespace mce::editor
