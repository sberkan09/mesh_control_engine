#pragma once
// BatchRename — names for a whole selection at once (M-BATCHRENAME). A pattern is plain text with two marks:
// a run of '#' is the entity's number, zero-padded to the run's length ("Torch_##" gives Torch_01, Torch_02, ...),
// and '*' is the entity's current name ("Old_*" keeps it). Numbers count from `start` in document order. The result
// is cut to the 63 characters an entity name holds. Headless, tested in CI.

#include "mce/fixed_string.h"

namespace mce::editor {

// The name for the entity numbered `index` (0-based, in document order) whose current name is `old_name`.
FixedString<64> batch_rename_expand(StringView pattern, StringView old_name, u32 index, u32 start);

}  // namespace mce::editor
