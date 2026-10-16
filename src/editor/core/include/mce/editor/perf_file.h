#pragma once
// .mcperf files read back (M-PERF steps 3-4): the JSON the player writes with -perf_capture
// (mce/perf_capture.h), parsed into a perf::Capture for MCP's perf_run / perf_compare and the
// editor's Profiler panel. The summary in the file is not read: perf::summarize recomputes it from
// the frames, so a file and the capture it came from give the same numbers.

#include "mce/cook/json.h"
#include "mce/perf_capture.h"

namespace mce::editor {

// False with `err` set when the text is not a version-1 mce-perf document.
bool read_mcperf(StringView text, perf::Capture& out, cook::String& err);
bool load_mcperf(StringView path, perf::Capture& out, cook::String& err);

}  // namespace mce::editor
