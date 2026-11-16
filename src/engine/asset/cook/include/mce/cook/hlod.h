#pragma once
// The .mchlod file (M-OPENWORLD A3): an HLOD proxy mesh as the hlod_bake tool writes it and the "hlod" importer
// cooks it (src/hlod_importer.cpp): "MCHL", a u32 version, a CHNK_MESH payload (render::build_merged_mesh's
// output) and a trailing FNV-1a 64 of every byte before it.

#include "mce/containers/array.h"

namespace mce::cook {

// Wraps a CHNK_MESH payload into .mchlod bytes (replacing `out`). The cook validates the payload again.
void write_hlod_file(Span<const byte> mesh_payload, Array<byte>& out);

}  // namespace mce::cook
