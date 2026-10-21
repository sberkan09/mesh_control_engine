#pragma once
// Forms over JSON source assets (M-ASSETLOOK 3): the editor's Material panel edits a .mcmat as a struct of
// fields, and this file turns the struct back into the file. Headless (tested in CI); the panel draws it.
//
//   MaterialForm   the scalar parameters, the alpha mode and the texture slots of a .mcmat
//   A save keeps every member of the file the form does not own (sheen, anisotropy, parallax steps, "name", a
//   comment-like extra key) and their order; the members it owns are rewritten in the file's own layout (two
//   spaces, one member per line, arrays inline), a member the file did not have is only added when it is not
//   the default. The text is cooked in memory with the project's importer before anything is written, so a
//   save that the cook refuses writes nothing and says why.

#include "mce/cook/string.h"
#include "mce/editor/asset_thumb.h"
#include "mce/editor/project.h"

namespace mce::editor {

struct MaterialForm {
    f32 base_color[4] = {1, 1, 1, 1};
    f32 metallic = 0;
    f32 roughness = 1;
    f32 emissive[3] = {0, 0, 0};
    f32 uv_scale[2] = {1, 1};
    int alpha_mode = 0;  // 0 opaque, 1 mask, 2 blend
    f32 alpha_cutoff = 0.5f;
    bool double_sided = false;
    bool unlit = false;
    f32 clearcoat = 0;
    f32 clearcoat_roughness = 0;
    f32 transmission = 0;
    f32 ior = 1.5f;
    f32 height_scale = 0;
    // "guid:<uuid>" per slot, empty = none.
    static constexpr u32 kSlots = 5;
    cook::String texture[kSlots];  // base_color, normal, metallic_roughness, emissive, height

    static const char* slot_name(u32 slot);
    // The swatch the form draws (the scalar look; textures are not read here).
    MaterialSwatch swatch() const;
};

// Reads the form from the .mcmat source text. False with `err` set when it is not a material document.
bool material_form_from_text(StringView text, MaterialForm& out, cook::String& err);
// The file text after applying `form` to `old_text` (see above). False with `err` set when `old_text` is not a JSON object.
bool material_form_to_text(StringView old_text, const MaterialForm& form, cook::String& out, cook::String& err);

// Reads, or cooks-then-writes, the source file of a Material asset of the project; `err` says why not.
bool load_material_form(const EditorProject& project, const ProjectAsset& asset, MaterialForm& out, cook::String& err);
bool save_material_form(EditorProject& project, const ProjectAsset& asset, const MaterialForm& form, cook::String& err);

// The .mcpart twin (M-ASSETLOOK 4): the same rules, the preset's own members. Angles are degrees as in the file.
struct ParticleForm {
    f32 rate = 0;
    u32 burst = 0;
    u32 max_particles = 256;
    f32 duration = 0;
    f32 lifetime[2] = {1, 1};
    f32 speed[2] = {1, 1};
    f32 direction = 90;  // degrees
    f32 spread = 0;      // degrees
    f32 gravity[2] = {0, 0};
    f32 drag = 0;
    f32 size[2] = {0.25f, 0.25f};
    f32 spin[2] = {0, 0};  // degrees per second
    f32 area[2] = {0, 0};
    f32 color_start[4] = {1, 1, 1, 1};
    f32 color_end[4] = {1, 1, 1, 1};
    bool additive = false;
    cook::String texture;  // "guid:<uuid>", empty = none

    ParticleSwatch swatch() const;
};
bool particle_form_from_text(StringView text, ParticleForm& out, cook::String& err);
bool particle_form_to_text(StringView old_text, const ParticleForm& form, cook::String& out, cook::String& err);
bool load_particle_form(const EditorProject& project, const ProjectAsset& asset, ParticleForm& out, cook::String& err);
bool save_particle_form(EditorProject& project, const ProjectAsset& asset, const ParticleForm& form, cook::String& err);

// The .mcseq twin (M-SEQEDIT): an action list. A save rewrites the "steps" array from the form (every step is one
// action key plus its extras, which is all the cook accepts) and "name" / "loop"; the other members stay in place.
struct SequenceStepForm {
    enum Kind { Wait, WaitEvent, MoveTo, Anim, Sound, StopSound, Emit, Call, Spawn, Destroy, kKinds };
    static const char* kind_name(int kind);  // the action key: "wait", "move_to", ...
    int kind = Wait;
    f32 seconds = 0;           // Wait
    cook::String text;         // WaitEvent / Anim / Emit / Call: the name; Sound / Spawn: "guid:<uuid>"
    f32 pos[3] = {0, 0, 0};    // MoveTo target, Spawn "at"
    bool has_z = false;        // the position had a third number
    bool relative = false;     // MoveTo / Spawn
    f32 speed = 1;             // MoveTo
    f32 tolerance = 0.01f;     // MoveTo
    f32 gain_db = 0;           // Sound
};
struct SequenceForm {
    cook::String name;
    bool loop = false;
    Array<SequenceStepForm> steps;
    // A one-line description of a step for lists ("wait 0.5 s", "move_to [4, 2] at 3 m/s").
    static cook::String describe(const SequenceStepForm& step);
};
bool sequence_form_from_text(StringView text, SequenceForm& out, cook::String& err);
bool sequence_form_to_text(StringView old_text, const SequenceForm& form, cook::String& out, cook::String& err);
bool load_sequence_form(const EditorProject& project, const ProjectAsset& asset, SequenceForm& out, cook::String& err);
bool save_sequence_form(EditorProject& project, const ProjectAsset& asset, const SequenceForm& form, cook::String& err);

// Cooks `text` in memory as the asset at `asset.path` would cook and writes nothing. False when the cook refuses it
// (the errors are in `diags`) or no importer reads the file type (`diags` empty, `err` says so).
bool check_source(const EditorProject& project, const ProjectAsset& asset, StringView text, cook::DiagList& diags, cook::String& err);

// Cooks `text` in memory as the asset at `asset.path` would cook, then writes it atomically and rescans the
// catalog. Nothing is written when the cook refuses it. False with `err` set.
bool validate_and_save_source(EditorProject& project, const ProjectAsset& asset, StringView text, cook::String& err);

}  // namespace mce::editor
