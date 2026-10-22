#pragma once
// Layer-internal: the scene-editing panels (scene_panels.cpp).

#include "mce/editor/editor_shell.h"

namespace mce::editor {

// Registers Hierarchy, Inspector and Cook.
void add_scene_panels(EditorShell& shell);
// Registers the Play panel (play_panel.cpp).
void add_play_panel(EditorShell& shell);
// Registers the Assets browser (asset_panel.cpp).
void add_asset_panel(EditorShell& shell);
void add_profiler_panel(EditorShell& shell);
void add_tilemap_panel(EditorShell& shell);
void add_terrain_panel(EditorShell& shell);
void add_project_settings_panel(EditorShell& shell);
// The UI Document panel (ui_panel.cpp): a .mcui's tree, inspector and preview.
void add_ui_document_panel(EditorShell& shell);
inline constexpr const char* kUiDocumentPanel = "UI Document";
// The Timeline panel (timeline_panel.cpp): tracks, keys, curves, record mode and the preview.
void add_timeline_panel(EditorShell& shell);
inline constexpr const char* kTimelinePanel = "Timeline";
// The Animation panel (animation_panel.cpp): an .mcanim graph, its inspector and a clip / graph preview.
void add_animation_panel(EditorShell& shell);
inline constexpr const char* kAnimationPanel = "Animation";
// The Behavior Tree panel (bt_panel.cpp): an .mcbt tree, its inspector and blackboard, and the play debugger.
void add_bt_panel(EditorShell& shell);
inline constexpr const char* kBtPanel = "Behavior Tree";
// The Skeleton panel (skeleton_panel.cpp): a .mcskel2d's bones, slots, rest poses, animation scrubber and IK targets.
void add_skeleton_panel(EditorShell& shell);
inline constexpr const char* kSkeletonPanel = "Skeleton";
// The Material panel (material_panel.cpp): a .mcmat's parameters and texture slots beside a live CPU swatch.
void add_material_panel(EditorShell& shell);
inline constexpr const char* kMaterialPanel = "Material";
// The Particle panel (particle_panel.cpp): a .mcpart's emitter parameters beside a live CPU swatch.
void add_particle_panel(EditorShell& shell);
inline constexpr const char* kParticlePanel = "Particle";
// The Sequence panel (sequence_panel.cpp): a .mcseq's steps as an editable list.
void add_sequence_panel(EditorShell& shell);
inline constexpr const char* kSequencePanel = "Sequence";
// The Mixer panel (mixer_panel.cpp): the scene's bus effects, reverb units, limiter and ducks (M-MIXEDIT).
void add_mixer_panel(EditorShell& shell);
inline constexpr const char* kMixerPanel = "Mixer";
// The Strings panel (strings_panel.cpp): every locale's string table as one key by locale grid.
void add_strings_panel(EditorShell& shell);
inline constexpr const char* kStringsPanel = "Strings";
// The Source panel (source_panel.cpp): any JSON source asset as text, checked by the cook as it is typed (M-SOURCEEDIT).
void add_source_panel(EditorShell& shell);
inline constexpr const char* kSourcePanel = "Source";
// Opens the Source panel on an asset (a no-op when the panel is not registered).
void open_source_panel(EditorShell& shell, asset::Guid guid);
// What a double click on an asset in the Assets panel does (asset_panel.cpp): the asset opens in its own panel, or
// `open_scene` is set to it when it is a scene or prefab (the caller then request_open_scene()s it).
void open_asset_in_editor(EditorShell& shell, const ProjectAsset& asset, asset::Guid& open_scene);
// The Scenes panel (scenes_panel.cpp): a tab per open scene file (M-SCENETABS).
void add_scenes_panel(EditorShell& shell);
inline constexpr const char* kScenesPanel = "Scenes";
// Drag-drop payload of the asset browser: the asset's Guid.
inline constexpr const char* kAssetPayload = "MCE_ASSET";
// Destroys every selected entity (Del in the Hierarchy and the Viewport).
void delete_selection(EditorScene& scene);

}  // namespace mce::editor
