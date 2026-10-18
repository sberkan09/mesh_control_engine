#pragma once
// EditorSession — the open project + the open scene (editor-tools.md §2.2): the file-level
// commands the menu bar, the cook panel and the selftests share (new/open/save/save-as/cook).
// ImGui-free, so the whole open -> edit -> save -> cook loop runs headless in CI.
//
// Paths: scene paths are absolute (or relative to the working directory); `scene_rel` helpers
// take paths relative to the project's assets/ folder, which is how the catalog lists them.

#include "mce/cook/impostor.h"
#include "mce/kit/chunk_lod3d.h"
#include "mce/physics3d/navmesh3d.h"
#include "mce/editor/editor_scene.h"
#include "mce/editor/play_session.h"
#include "mce/editor/project.h"

namespace mce::editor {

class PreviewRenderer;

class EditorSession {
public:
    EditorProject& project() { return project_; }
    const EditorProject& project() const { return project_; }
    EditorScene& scene() { return scene_; }
    const EditorScene& scene() const { return scene_; }

    // The renderer that draws the mounted content; detached and re-attached around every cook
    // (the old asset system dies inside the cook). Null: no preview.
    void set_preview(PreviewRenderer* preview);

    // Opens `root` (must hold assets/), cooks it when nothing is cooked yet, starts an empty scene.
    Result<void> open_project(StringView root);

    void new_scene(StringView name);
    // `path` absolute, or relative to assets/ when a project is open and the file exists there.
    // On failure the current scene stays open and scene_diags() says why.
    bool open_scene(StringView path);
    Result<void> save_scene();                       // to path(); fails when the scene was never saved
    // Same path resolution as open_scene. A new file gets a fresh GUID; `keep` (non-null) is the
    // GUID the file keeps instead, i.e. the existing file's when overwriting it on purpose.
    Result<void> save_scene_as(StringView path, asset::Guid keep = {});
    const cook::DiagList& scene_diags() const { return scene_diags_; }

    // Cooks the project (pack: also writes <root>/assets.mce) and remounts the preview.
    const cook::CookReport& cook(bool pack = false, bool adopt = false, Span<const cook::String> paths = {},
                                 bool skip_errors = false, u32 max_texture_size = 0,
                                 cook::TextureCompression textures = cook::TextureCompression::None);

    // ---- autosave + recovery (editor-tools.md §2.11) ----
    // Where the open scene autosaves: <root>/.mce/autosave/<path under assets/>.autosave (an
    // untitled scene uses "untitled.mcscene"), or "<scene file>.autosave" outside a project.
    // Empty when there is nowhere to put it (no project and never saved).
    cook::String autosave_path() const;
    // Writes the autosave (atomic) when the scene changed since the last one. True if written.
    bool autosave();
    // Autosaves once `interval` seconds have passed since the last autosave (or since the scene
    // was opened) while there are unsaved changes. `now` is any monotonic clock in seconds.
    void tick_autosave(f64 now, f64 interval);
    // True when an autosave exists for the open scene and differs from what is on disk: the
    // editor ended without saving (crash or kill). The editor offers recover() / discard.
    bool has_recovery() const;
    bool recover();
    void discard_autosave();

    // ---- play-in-editor (FR-EDT-060) ----
    // Plays the open document as it is now (saved or not); cook, open and new stop it first.
    // On failure play_diags() says why.
    bool enter_play();
    void stop_play() { play_.stop(); }
    PlaySession& play() { return play_; }
    const PlaySession& play() const { return play_; }
    const cook::DiagList& play_diags() const { return play_diags_; }

    // ---- hot reload (FR-AST-090) ----
    // Watches the project's assets/ folder; tick_hot_reload() polls it every `interval` seconds
    // and calls hot_reload() when something changed. True when a reload ran.
    bool tick_hot_reload(f64 now, f64 interval);
    // Re-cooks what changed and swaps it into the live mount (play keeps running unless a remount
    // is needed). When the open scene's file is among `changed` and differs from the document, an
    // unmodified document is re-read from disk; a modified one is kept (and a warning logged).
    HotReloadResult hot_reload(Span<const cook::String> changed = {});
    const HotReloadResult& last_hot_reload() const { return last_reload_; }
    // How many hot reloads have run: what was built from the mounted content is stale when it moves.
    u64 hot_reload_count() const { return reload_count_; }
    // Asset browser rename/move (GUID-stable, FR-EDT-110): moves the file, follows the open scene
    // when it is the one moved, and hot-reloads so the mount sees the new path.
    Result<void> rename_asset(asset::Guid guid, StringView new_rel_path);
    // Writes a text asset the editor authors (a tilemap) under assets/ and hot-reloads it at once:
    // the file is cooked and swapped (or mounted, when new) before this returns.
    Result<HotReloadResult> write_asset(StringView rel_path, StringView text);
    // EditorScene::apply_override's hook: writes `fields` of `value` into the root of the prefab
    // asset `prefab` (its .mcprefab under assets/) and hot-reloads.
    Result<void> apply_to_prefab(asset::Guid prefab, const DocComponent& value, u64 fields);
    // Save as prefab (M-DATA-REACH 2): writes `root` and its ChildOf subtree as a new .mcprefab
    // at `rel_path` (under assets/; the root first and without its parent link, references that
    // leave the subtree nulled) with a fresh GUID, and hot-reloads it. With `replace`, the subtree
    // becomes an instance of the new prefab at the same index, parent and transform (one undo
    // step). Refuses an existing file and a prefab instance's subtree that the prefab could not
    // hold. Returns the prefab's GUID.
    Result<asset::Guid> create_prefab_from(EntityId root, StringView rel_path, bool replace = true);

    // Reflection probe bake (M-VISUAL 2): captures every ReflectionProbe3D of the open scene through
    // the preview with all of the scene's meshes, writes assets/probes/<scene>/probe_<entity id>.mcenv
    // (+ sidecar; a re-bake keeps the file's GUID), hot-reloads them and points each component's
    // `baked` at its file (one undo step). Returns how many probes were baked.
    Result<u32> bake_reflection_probes();
    // GI volume bake (M-VISUAL 6): fills the open scene's first enabled GiVolume3D through the preview
    // with all of the scene's meshes (three bounces), writes assets/gi/<scene>/gi_<entity id>.mcenv
    // (+ sidecar; a re-bake keeps the file's GUID), hot-reloads it and points the component's `baked`
    // at it (one undo step). The Low quality tier draws the volume from this bake only.
    Result<void> bake_gi_volume();
    // Navmesh bake (M-GAMEPLAY3D 1): bakes the open scene's first NavMeshSurface3D from its colliders,
    // areas and links (kit::bake_scene_navmesh), writes assets/navmesh/<scene>/nav_<entity id>.mcnav
    // (+ sidecar; a re-bake keeps the file's GUID), hot-reloads it and points the component's `baked`
    // at it (one undo step). Returns the stats of the bake.
    Result<physics::NavBakeStats3D> bake_navmesh();

    // HLOD bake (M-OPENWORLD A3): for every SceneChunk of the open scene (or only `only`, when not kNoEntity), merges
    // the area scene's static meshes into one proxy (kit::bake_chunk_hlod, positions relative to the chunk entity),
    // writes assets/hlod/<scene>/chunk_<entity id>.mchlod (+ sidecar; a re-bake keeps the GUID), hot-reloads it and
    // points the chunk's ChunkLod3D `proxy` at it, adding the component when missing (one undo step). A chunk whose
    // area has nothing to merge is left as it is. Returns one row per chunk tried.
    // With `impostor` set, a chunk that got a proxy also gets an impostor (cook::bake_impostor over the same sources):
    // chunk_<entity id>_far.png (the atlas, a "texture" sidecar) and chunk_<entity id>_far.mchlod, and ChunkLod3D
    // `impostor` points at the mesh.
    struct HlodBakeRow {
        EntityId chunk = 0;
        asset::Guid proxy;  // null: nothing merged
        u32 bytes = 0;
        kit::HlodBakeStats3D stats;
        asset::Guid impostor;  // null: none baked
        u32 impostor_bytes = 0;  // the .mchlod's
        cook::ImpostorStats impostor_stats;
    };
    Result<Array<HlodBakeRow>> bake_hlod(const kit::HlodBakeSettings3D& settings, EntityId only = kNoEntity,
                                         const cook::ImpostorSettings* impostor = nullptr);

    // Every scene in the catalog, by path.
    void list_scenes(Array<const ProjectAsset*>& out) const;
    // Absolute path of a scene path given relative to assets/ (or `path` unchanged).
    cook::String resolve(StringView path) const;

private:
    void attach_preview();

    EditorProject project_;
    EditorScene scene_;
    PreviewRenderer* preview_ = nullptr;
    cook::DiagList scene_diags_;
    AssetWatcher watcher_;
    HotReloadResult last_reload_;
    u64 reload_count_ = 0;
    PlaySession play_;  // after project_: it reads the project's mounted tree until destroyed
    cook::DiagList play_diags_;
    u64 autosaved_revision_ = ~u64(0);
    f64 autosave_clock_ = -1.0;  // time of the last autosave (or scene open); < 0: not started
};

}  // namespace mce::editor
