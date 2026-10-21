#pragma once
// The engine build stamp as text: `git describe` + build type + compiler + sanitizer setting, taken
// at configure time (cmake/EngineId.cmake). Shipped builds print it beside their own version.

namespace mce {
const char* engine_describe();
}
