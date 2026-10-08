#pragma once
// mce/gameplay.h — the engine surface a game module includes (scripting.md §2.3): core types,
// the ECS, the gameplay-facing components/resources of input, physics and audio, and the module
// contract. Renderer, RHI, platform, asset internals and the cook library are not part of it.
//
// M3 note: the include-path starvation that makes this a hard wall (a module target that can
// only see these headers) arrives with the M4 packaging; until then it is the convention.

#include "mce/audio/components.h"
#include "mce/core.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/ecs.h"
#include "mce/kit/components.h"
#include "mce/module.h"
#include "mce/physics/components.h"
#include "mce/physics/module.h"
