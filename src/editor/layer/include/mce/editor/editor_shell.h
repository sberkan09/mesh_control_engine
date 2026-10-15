#pragma once
// EditorShell — the editor frame (editor-tools.md §2.2, §2.7, §2.10): ImGui docking shell with a
// panel registry, dock-layout persistence, the file menu, and the editing session (project +
// scene + preview renderer + editor camera). Without a project the viewport shows the M0 stub
// world; with one it shows the open scene. This header is ImGui-free: mce_editor.exe sees only
// this.
//
// Per frame (main thread, inside device.begin_frame/end_frame):
//   shell.build_ui(dt);              // ImGui frame: menu, dockspace, panels
//   shell.record(cmd, alpha);        // viewport world -> its render target, then ImGui -> backbuffer

#include "mce/editor/console_model.h"
#include "mce/editor/editor_session.h"
#include "mce/editor/gizmo.h"
#include "mce/editor/tilemap_editor.h"
#include "mce/editor/ui_document_editor.h"
#include "mce/editor/orbit_camera.h"
#include "mce/editor/perf_monitor.h"
#include "mce/editor/preview_renderer.h"
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
    bool focused = false;  // the panel has keyboard focus and no text field owns the keyboard
};

// Editor-side view settings shared by the viewport and the menus.
struct ViewSettings {
    bool show_grid = true;
    f32 grid_step = 1.0f;  // metres
    u32 grid_major = 8;
    bool snap = false;      // gizmo drags land on multiples of snap_step (Ctrl inverts)
    f32 snap_step = 0.5f;   // metres
    bool play_camera = true;  // while playing, the viewport looks through the scene's primary camera
    bool orbit_view = false;  // 3D orbit inspection view (wireframe, camera collision against colliders)
    bool orbit_collision = true;
    f32 orbit_depth = OrbitColliders::kDefaultDepth;  // extrusion of 2D colliders along Z (metres)
    GizmoMode gizmo = GizmoMode::Move;  // W / E / R
    bool tile_paint = false;  // the viewport paints the selected TilemapLayer (Tilemap panel, T)
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
    // The host calls this after RenderDevice::submit with the submit's wall time: the frame's
    // sample (sim / UI / record / submit / GPU) goes to the Profiler's history.
    void end_frame_perf(f64 submit_ms);
    PerfMonitor& perf() { return perf_; }

    void add_panel(mem::UniquePtr<IEditorPanel> panel);
    IEditorPanel* find_panel(StringView name);
    usize panel_count() const { return panels_.size(); }

    // Asks before discarding unsaved scene changes (Save / Discard / Cancel).
    void request_quit();
    void force_quit() { quit_requested_ = true; }
    bool quit_requested() const { return quit_requested_; }

    // ---- editing session ----
    EditorSession& session() { return session_; }
    EditorScene& scene() { return session_.scene(); }
    PreviewRenderer& preview() { return preview_; }
    EditorCamera2D& camera() { return camera_; }
    OrbitCamera& orbit() { return orbit_; }
    OrbitColliders& orbit_colliders() { return orbit_colliders_; }
    ViewSettings& view() { return view_; }
    TilemapEditor& tilemap() { return tilemap_; }
    // The UI document the UI Document panel edits (open_ui_document: a double click in Assets).
    UiDocumentEditor& ui_document() { return ui_document_; }
    void open_ui_document(asset::Guid guid);
    // True once a project is open: the viewport shows the scene instead of the stub world.
    bool scene_mode() const { return session_.project().is_open(); }
    bool open_project(StringView root);
    bool open_scene(StringView path);  // absolute, or relative to the project's assets/ (offers autosave recovery)
    void request_open_scene(StringView path);  // asks first when the open scene is dirty
    bool save_scene();                 // Save As prompt when the scene has no file yet
    void new_scene();                  // asks first when the open scene is dirty
    void cook(bool pack = false);
    // Play-in-editor (FR-EDT-060, F5): the open document runs in the viewport; editing is locked
    // until Stop, and the document comes back exactly as it was.
    bool play_in_editor();
    void stop_play();
    bool playing() const { return session_.play().active(); }
    // Run-in-player (FR-EDT-090, Ctrl+F5): save, cook with --pack, start mce_player on the open scene as a
    // child process. False (and the reason logged / shown in the Cook panel) when it cannot.
    bool play();
    void stop_player();
    bool player_running() const;
    // Clipboard (system clipboard, .mcscene text).
    void copy_selection();
    void cut_selection();
    void paste();
    void duplicate_selection();
    // The viewport frames the whole scene (or the selection) on its next draw.
    void frame_scene() { frame_request_ = true; }
    bool frame_requested() const { return frame_request_; }
    bool take_frame_request() {
        const bool r = frame_request_;
        frame_request_ = false;
        return r;
    }
    // Focus a panel by name on the next frame (e.g. the cook panel after a failed cook).
    void focus_panel(const char* name) { focus_panel_ = name; }
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
    void draw_popups();
    void handle_shortcuts();
    void update_title();
    // `e` as the play world's input sees it (on_event, while playing).
    platform::Event game_event(const platform::Event& e) const;

    Impl* impl_ = nullptr;
    platform::Window* window_ = nullptr;
    rhi::RenderDevice* device_ = nullptr;
    StubWorld world_;
    ConsoleModel console_;
    EditorSession session_;
    PreviewRenderer preview_;
    EditorCamera2D camera_;
    OrbitCamera orbit_;
    OrbitColliders orbit_colliders_;
    ViewSettings view_;
    TilemapEditor tilemap_;
    UiDocumentEditor ui_document_;
    PerfMonitor perf_;
    FrameSample perf_pending_;
    Array<mem::UniquePtr<IEditorPanel>> panels_;
    ViewportInfo viewport_;
    FixedString<256> ini_path_;
    u64 frames_ = 0;
    bool quit_requested_ = false;
    bool reset_layout_ = false;
    bool show_demo_ = false;
    bool frame_request_ = false;
    const char* focus_panel_ = nullptr;
    // Pending modal popups (opened on the next frame, inside the ImGui frame).
    enum class Popup : u8 { None, OpenScene, SaveAs, ConfirmQuit, ConfirmNew, ConfirmOpen, Recover };
    Popup popup_ = Popup::None;
    bool popup_opened_ = false;
    char save_as_buf_[256] = {};
    FixedString<512> pending_open_;
    FixedString<512> title_;
};

// Registers the panel set: Viewport, Hierarchy, Inspector, Cook, Console, Stats, Profiler, ...
void add_default_panels(EditorShell& shell);

}  // namespace mce::editor
