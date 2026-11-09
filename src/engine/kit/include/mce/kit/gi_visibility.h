#pragma once
// Probe visibility, relocation and a following box for the irradiance volume as data (M-GI2 step 50h). One optional
// GiVisibility3D on the entity that has the GiVolume3D (kit3d.h); it is read there and nowhere else. It is a component of
// its own, not new GiVolume3D fields, because GiVolume3D is hashed and saved by its bytes (sizeof 88): a scene without a
// GiVisibility3D hashes, saves and draws exactly as before. install_kit3d registers it; there is no system and nothing is
// stored between ticks (Kit3DExtract copies the fields onto render::GiVolume, .cpp only).
//
//   GiVisibility3D  Authoring (all Save, clamped when read and never rewritten): `enabled` (1); `relocate` (0) a probe
//                   found inside geometry moves out of it; `max_offset` (0.4, 0..0.45) the fraction of the probe spacing
//                   it may move; `follow_camera` (0) the volume's box follows the primary camera in whole cells on x and
//                   z; `bake_format` (0) the record format the volume's bake was made with, written by File > Bake GI
//                   volume (see below), never by hand.
//
// Present and enabled, the volume stores the 10-texel record (visibility and classification come together, there is no
// separate switch): render::GiVolume::visibility is true. Without it, or with `enabled` 0, the carrier fields keep their
// defaults and the volume is today's.
//
// The bake carries no format version of its own (the .mcenv only says its width, 10 texels a probe): 50d's record held
// radial mean distances, 50e's holds axial ones. `bake_format` is the stamp: the editor's bake writes kGiRecordFormat into
// the volume's GiVisibility3D, and Kit3DExtract hands the bake to the renderer only when the component's stamp equals
// kGiRecordFormat (a 10-texel bake without the stamp, or with another one, is ignored: the volume fills at runtime and the
// Low tier, which has nothing else, takes the sky's ambient), as the width check refuses a bake of the wrong layout.
// Re-bake after an engine change of the record: bump kGiRecordFormat.
// Implemented in gi_visibility.cpp; nothing here is hashed.

#include "mce/types.h"
#include "mce/refl/macros.h"

namespace mce::kit {

// The record format a bake made by this build holds (1: 50e's axial distances in texels 4..7, texel 8 the relocation
// offset, texel 9 the classification).
inline constexpr u32 kGiRecordFormat = 1;

struct GiVisibility3D {
    u32 enabled = 1;
    u32 relocate = 0;
    f32 max_offset = 0.4f;
    u32 follow_camera = 0;
    u32 bake_format = 0;
};
MCE_COMPONENT(GiVisibility3D,
              (enabled, Save, Range{0, 1}, Help{"0: the volume is the plain one (no visibility, relocation or follow)"}),
              (relocate, Save, Range{0, 1}, Help{"1: a probe inside geometry moves out of it"}),
              (max_offset, Save, Range{0, 0.45f}, Help{"fraction of the probe spacing a probe may move out of a solid"}),
              (follow_camera, Save, Range{0, 1},
               Help{"1: the volume's box follows the primary camera in whole cells, on x and z"}),
              (bake_format, Save, Help{"record format of the volume's bake, set by the bake; a mismatch ignores the bake"}))

// max_offset as the renderer takes it: 0..0.45, NaN 0.4 (the default).
f32 gi_visibility_max_offset(const GiVisibility3D& v);

// Whether the volume's bake may be used: no component, a disabled one (the 4-texel layout) or a stamp equal to
// kGiRecordFormat.
bool gi_visibility_bake_ok(const GiVisibility3D* v);

}  // namespace mce::kit
