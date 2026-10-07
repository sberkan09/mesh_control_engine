#pragma once
// Whole-file reads for the asset loader. Placeholder until mce_platform's filesystem/mmap lands
// (ARCHITECTURE §5.2): the loader only needs "bytes of this file, 16-byte aligned", so swapping
// in a MemoryMappedFile later changes this file and nothing that calls it.

#include "mce/containers/array.h"
#include "mce/result.h"

namespace mce::asset {

// Reads the whole file into `out` (heap storage is 16-byte aligned). `path` is a native path.
Result<void> read_file(StringView path, Array<byte>& out);

}  // namespace mce::asset
