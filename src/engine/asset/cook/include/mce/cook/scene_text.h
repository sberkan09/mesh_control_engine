#pragma once
// The .mcscene component codec (assets.md §6), shared by the scene cook and the editor's
// SceneDocument: the cook owns the format, the editor only drives it (editor-tools.md §1.2).
//
// Reading follows the scene importer exactly (it is the same walker): omitted fields keep the C++
// default member values, Transient fields are rejected, EntityRef fields receive the scene-local
// entity index in their first u32 (kNoSceneEntity for null) with the rest zeroed — the CHNK_SCENE
// row layout. Writing emits only the fields that differ from the type's default (so files stay
// small, readable and robust to new fields), as one line: { "field": value, ... }.

#include "mce/cook/diagnostics.h"
#include "mce/cook/json.h"
#include "mce/refl/refl.h"

namespace mce::cook {

inline constexpr u32 kSceneFormatVersion = 1;
inline constexpr u32 kNoSceneEntity = 0xFFFFFFFFu;

// Fills `dst` (one value of `type`, already default-constructed by the caller) from `obj`.
// `entity_count` bounds "$<index>" references. `path` prefixes diagnostics ("entities[2].Sprite").
// Returns false when any diagnostic was added.
bool read_scene_component(const refl::TypeRecord& type, JsonValue obj, void* dst, u32 entity_count, StringView file,
                          StringView path, DiagList& diags);

// Maps the bytes of an EntityRef field to a scene-local index; kNoSceneEntity writes nothing
// (the field reads back as null).
using SceneEntityIndexFn = u32 (*)(const void* field, void* user);

// Appends `value` as a JSON object holding its non-default, non-Transient fields.
void write_scene_component(const refl::TypeRecord& type, const void* value, String& out, SceneEntityIndexFn entity_index,
                           void* user);

// Appends every field of `value`, Transient and default ones included: a live component as tools
// inspect it (MCP run_headless). Never cooked; the scene importer ignores Transient fields.
void write_scene_component_state(const refl::TypeRecord& type, const void* value, String& out, SceneEntityIndexFn entity_index,
                                 void* user);

// Appends exactly the fields of `value` whose bit is set in `field_mask` (bit i = type.fields[i];
// Transient fields never), whether or not they hold the default. A prefab instance's overrides:
// the cook writes them over the prefab root's values, so an override back to the C++ default
// must still be written.
void write_scene_component_fields(const refl::TypeRecord& type, const void* value, u64 field_mask, String& out,
                                  SceneEntityIndexFn entity_index, void* user);

// Shortest decimal that reads back as exactly `v` (as a float), JSON-safe (NaN/Inf write 0).
void append_json_f32(String& out, f32 v);

}  // namespace mce::cook
