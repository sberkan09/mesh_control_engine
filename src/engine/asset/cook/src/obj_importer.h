#pragma once
// Factories of the Wavefront OBJ importers (M-IMPORT3D step 2). Cook-private on purpose: importer.h is included by
// ~170 translation units, so these two are declared here and only importer_registry.cpp and the cook's tests see them.

#include "mce/cook/importer.h"

namespace mce::cook {

mem::UniquePtr<IImporter> make_obj_importer();  // .obj (+ the .mtl it names) -> CHNK_MESH
mem::UniquePtr<IImporter> make_mtl_importer();  // .mtl -> CHNK_MAT of its first material; the .obj reads the file's bytes

}  // namespace mce::cook
