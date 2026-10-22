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
#include "mce/editor/animgraph_editor.h"
#include "mce/editor/bt_editor.h"
#include "mce/editor/skeleton2d_editor.h"
#include "mce/editor/terrain_editor.h"
#include "mce/editor/tilemap_editor.h"
#include "mce/editor/timeline_editor.h"
#include "mce/editor/ui_document_editor.h"
#include "mce/editor/orbit_camera.h"
#include "mce/editor/perf_monitor.h"
#include "mce/editor/preview_renderer.h"
#include "mce/editor/scene_tabs.h"
#include "mce/editor/stub_world.h"
#include "mce/memory/allocator.h"
#include "mce/platform/window.h"
#include "mce/rhi/rhi.h"

namespace mce::editor {

class EditorShell;
struct QuickOpenItem;

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
    bool terrain_sculpt = false;  // the orbit view sculpts the selected Terrain3D (Terrain panel)
    bool show_perception = true;  // while playing, the viewport draws each Perceiver's cone and target (Behavior Tree panel)
    bool show_roads = true;       // while a 3D game plays, the viewport draws its road networks' lanes, connectors by light
    bool show_gameplay = false;   // while a 3D game plays, the viewport draws colliders, triggers, joints, vehicles and paths (M-DEBUGVIEW)
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
    IEditorPanel* panel_at(usize i) { return i < panels_.size() ? panels_[i].get() : nullptr; }

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
    NavMeshPreview& nav_preview() { return nav_preview_; }
    ViewSettings& view() { return view_; }
    TilemapEditor& tilemap() { return tilemap_; }
    TerrainEditor& terrain() { return terrain_; }
    // The UI document the UI Document panel edits (open_ui_document: a double click in Assets).
    UiDocumentEditor& ui_document() { return ui_document_; }
    void open_ui_document(asset::Guid guid);
    // The timeline the Timeline panel edits (open_timeline: a double click in Assets), and its
    // viewport preview. Record mode keys the scene's inspector and gizmo edits into it.
    TimelineEditor& timeline() { return timeline_; }
    TimelinePreview& timeline_preview() { return timeline_preview_; }
    void open_timeline(asset::Guid guid);
    // The animation graph the Animation panel edits (open_anim_graph: a double click in Assets).
    AnimGraphEditor& anim_graph() { return anim_graph_; }
    void open_anim_graph(asset::Guid guid);
    // The behavior tree the Behavior Tree panel edits (open_bt: a double click in Assets).
    BtEditor& bt() { return bt_; }
    void open_bt(asset::Guid guid);
    // The 2D skeleton the Skeleton panel edits (open_skeleton2d: a double click in Assets). The
    // viewport draws its bones, in the panel's previewed pose, on the selected entities that play it.
    Skeleton2DEditor& skeleton2d() { return skeleton2d_; }
    void open_skeleton2d(asset::Guid guid);
    // The material the Material panel edits (open_material: a double click in Assets). The panel loads the file
    // itself (a MaterialForm) and saves it with its own button; the shell only carries the request.
    asset::Guid material_request() const { return material_request_; }
    void open_material(asset::Guid guid);
    // The same for the Particle panel (a double click on a particle preset).
    asset::Guid particle_request() const { return particle_request_; }
    void open_particle(asset::Guid guid);
    // And for the Sequence panel (a double click on a sequence).
    asset::Guid sequence_request() const { return sequence_request_; }
    void open_sequence(asset::Guid guid);
    // True once a project is open: the viewport shows the scene instead of the stub world.
    bool scene_mode() const { return session_.project().is_open(); }
    bool open_project(StringView root);
    bool open_scene(StringView path);  // absolute, or relative to the project's assets/ (offers autosave recovery)
    void request_open_scene(StringView path);  // asks first only when the open scene is dirty and has no file yet
    // The open scene files (M-SCENETABS): opening one parks the dirty scene in its autosave file instead of asking.
    SceneTabs& scene_tabs() { return tabs_; }
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
    // The Profiler opens this .mcperf (project-relative or absolute) in its Capture tab on its next draw.
    void open_perf_capture(StringView path) {
        perf_capture_request_ = path;
        focus_panel("Profiler");
    }
    bool take_perf_capture(FixedString<256>& path) {
        if (perf_capture_request_.empty()) return false;
        path = perf_capture_request_;
        perf_capture_request_ = StringView();
        return true;
    }
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
    void track_recent_scenes();
    void save_bookmark(u32 slot);
    void recall_bookmark(u32 slot);
    void fill_quick_open();
    void run_quick_open(const QuickOpenItem& item);
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
    NavMeshPreview nav_preview_;
    ViewSettings view_;
    TilemapEditor tilemap_;
    TerrainEditor terrain_;
    UiDocumentEditor ui_document_;
    TimelineEditor timeline_;
    TimelinePreview timeline_preview_;
    AnimGraphEditor anim_graph_;
    BtEditor bt_;
    Skeleton2DEditor skeleton2d_;
    asset::Guid material_request_;
    SceneTabs tabs_;
    asset::Guid particle_request_;
    asset::Guid sequence_request_;
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
    FixedString<256> perf_capture_request_;
    // Pending modal popups (opened on the next frame, inside the ImGui frame).
    enum class Popup : u8 { None, OpenScene, SaveAs, ConfirmQuit, ConfirmNew, ConfirmOpen, Recover, QuickOpen };
    Popup popup_ = Popup::None;
    bool popup_opened_ = false;
    char save_as_buf_[256] = {};
    FixedString<512> pending_open_;
    FixedString<512> title_;
};

// Registers the panel set: Viewport, Hierarchy, Inspector, Cook, Console, Stats, Profiler, ...
void add_default_panels(EditorShell& shell);

}  // namespace mce::editor
