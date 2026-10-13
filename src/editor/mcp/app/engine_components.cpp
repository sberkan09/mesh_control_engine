// The engine's reflected components, registered with mce_refl in the MCP server process (same set
// as the editor's and mce_cook's engine_components.cpp). Including a header is enough:
// MCE_COMPONENT emits the registrar next to the type.

#include "mce/audio/components.h"     // AudioSource
#include "mce/ecs/hierarchy.h"        // ChildOf, WorldTransform
#include "mce/ecs/save.h"             // SaveKey
#include "mce/ecs/transform.h"        // LocalTransform
#include "mce/kit/components.h"       // Sprite, TextLabel, Camera2DComponent, TilemapLayer, ParticleEmitter,
                                      // Light2D, Parallax, Post2DComponent, SequencePlayer, PathAgent
#include "mce/kit/kit3d.h"            // MeshRef, PointLight3D, SpotLight3D, Camera3DComponent, Animator, Environment3D
#include "mce/physics/components.h"   // RigidBody2D, Collider2D, CharacterController2D, ...
#include "mce/scene/streaming.h"      // SceneChunk, StreamingFocus, SceneChunkStatus
#include "mce/ui/types.h"             // UiAnchors, UiStyle, UiSlice, UiStackLayout, UiGridLayout (values, not components)
