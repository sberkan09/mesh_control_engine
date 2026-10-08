// The engine's reflected components, registered with mce_refl in the cook process so scenes that
// use them cook (the scene importer resolves component types through the registry). Including a
// header is enough: MCE_COMPONENT emits the registrar next to the type.
//
// Game-module components need the module's types in the cook too; that arrives with the project
// template (M8), until then a game cooks its own scenes with a cook that links its components.

#include "mce/audio/components.h"     // AudioSource
#include "mce/ecs/transform.h"        // LocalTransform
#include "mce/kit/components.h"       // Sprite, TextLabel, Camera2DComponent
#include "mce/physics/components.h"   // RigidBody2D, Collider2D, CharacterController2D, ...
