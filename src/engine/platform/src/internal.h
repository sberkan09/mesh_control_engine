#pragma once
// mce_platform internals shared by platform.cpp and window.cpp.

#include "mce/platform/window.h"

namespace mce::platform::detail {

void register_window(Window* w);
void unregister_window(Window* w);

}  // namespace mce::platform::detail
