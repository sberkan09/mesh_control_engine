// The engine's reflected components, registered with mce_refl in the cook process so scenes that
// use them cook (the scene importer resolves component types through the registry). Including a
// header is enough: MCE_COMPONENT emits the registrar next to the type.
//
// Game-module components need the module's types in the cook too; that arrives with the project
// template (M8), until then a game cooks its own scenes with a cook that links its components.

#include "mce/audio/components.h"     // AudioSource
#include "mce/ecs/hierarchy.h"        // ChildOf, WorldTransform
#include "mce/ecs/save.h"             // SaveKey
#include "mce/ecs/transform.h"        // LocalTransform
#include "mce/kit/anim3d.h"            // AttachToBone
#include "mce/kit/bodies3d.h"          // RigidBody3D, CharacterController3D, Trigger3D
#include "mce/kit/components.h"       // Sprite, TextLabel, Camera2DComponent, TilemapLayer, ParticleEmitter,
                                      // Light2D, Parallax, Post2DComponent, SequencePlayer, PathAgent
#include "mce/kit/captions.h"         // SoundCaption (M-ACCESS)
#include "mce/kit/effects.h"          // SfxTrigger, ParticleBurst, ShakeTrigger, CameraShake2D, CameraFollow2D/3D,
                                      // PhysicsEvents, NavGridSource
#include "mce/kit/joints3d.h"          // Joint3D
#include "mce/kit/kit3d.h"            // MeshRef, PointLight3D, SpotLight3D, Camera3DComponent, Animator, Environment3D
#include "mce/kit/nav3d.h"            // NavMeshSurface3D, NavArea3D, NavLink3D, NavObstacle3D, NavAgent3D
#include "mce/kit/novel.h"             // Dialogue, DialogueInput (M-NOVEL)
#include "mce/kit/novel_components.h"  // StagePart, StoryState, ...
#include "mce/kit/timeline.h"          // TimelinePlayer (M-CINEMA)
#include "mce/physics/components.h"   // RigidBody2D, Collider2D, CharacterController2D, ...
#include "mce/scene/streaming.h"      // SceneChunk, StreamingFocus, SceneChunkStatus
