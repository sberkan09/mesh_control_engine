#pragma once
// Factory of the Autodesk FBX importer (M-IMPORT3D step 3b, ADR-0047). Cook-private on purpose: importer.h is included by
// ~170 translation units, so the factory is declared here and only importer_registry.cpp and the cook's tests see it. The
// importer exists only in builds that have ufbx (host builds: MCE_HAS_FBX), so importer_registry.cpp guards the call.

#include "mce/cook/importer.h"

namespace mce::cook {

mem::UniquePtr<IImporter> make_fbx_importer();  // .fbx (binary or ASCII, version 3000 and up) -> CHNK_MESH

// `mce_cook --dump-fbx-scene` (M-IMPORT3D step 4a): the lights and cameras of an FBX as JSON for `mce import fbx`: per node the
// world translation / rotation / scale in the engine's frame (+Y up, right-handed, metres), plus the light (type, decay,
// colour, intensity, local aim direction, cone angles as the file's full angles, cast_light as "enabled") or camera
// (projection, vertical field of view, planes). Same load limits as the importer; false with a one-line `error` when the
// file cannot be read. Without ufbx (a cross build) it returns false saying so.
bool dump_fbx_scene_json(StringView display_name, Span<const byte> src, String& json, String& error);

}  // namespace mce::cook
