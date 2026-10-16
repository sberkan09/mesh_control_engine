// The engine's reflected components, registered with mce_refl in the editor process so the
// inspector, the add-component menu and scene load see them (same set as mce_cook's
// engine_components.cpp). Including a header is enough: MCE_COMPONENT emits the registrar.

#include "mce/audio/components.h"     // AudioSource
#include "mce/ecs/hierarchy.h"        // ChildOf, WorldTransform
#include "mce/ecs/save.h"             // SaveKey
#include "mce/ecs/transform.h"        // LocalTransform
#include "mce/kit/anim3d.h"            // AttachToBone, IKTarget3D
#include "mce/kit/bodies3d.h"          // RigidBody3D, CharacterController3D, Trigger3D
#include "mce/kit/components.h"       // Sprite, TextLabel, Camera2DComponent, TilemapLayer, ParticleEmitter,
                                      // Light2D, Parallax, Post2DComponent, SequencePlayer, PathAgent
#include "mce/kit/captions.h"         // SoundCaption (M-ACCESS)
#include "mce/kit/cinematic.h"        // CameraRail3D, CameraLookAt3D, CameraShake3D, CameraBlend3D, Cutscene (M-CINEMA)
#include "mce/kit/video.h"            // VideoPlayer (M-CINEMA 5)
#include "mce/kit/effects.h"          // SfxTrigger, ParticleBurst, ShakeTrigger, CameraShake2D, CameraFollow2D/3D,
                                      // PhysicsEvents, NavGridSource
#include "mce/kit/joints3d.h"          // Joint3D
#include "mce/kit/kit3d.h"            // MeshRef, PointLight3D, SpotLight3D, Camera3DComponent, Animator, Environment3D
#include "mce/kit/nav3d.h"            // NavMeshSurface3D, NavArea3D, NavLink3D, NavObstacle3D, NavAgent3D
#include "mce/kit/novel.h"             // Dialogue, DialogueInput (M-NOVEL)
#include "mce/kit/novel_components.h"  // StagePart, StoryState, ...
#include "mce/kit/ragdolls3d.h"        // Ragdoll3D
#include "mce/kit/sensors3d.h"         // Raycast3D, Overlap3D
#include "mce/kit/timeline.h"          // TimelinePlayer (M-CINEMA)
#include "mce/kit/vehicles3d.h"        // Vehicle3D
#include "mce/physics/components.h"   // RigidBody2D, Collider2D, CharacterController2D, ...
#include "mce/scene/streaming.h"      // SceneChunk, StreamingFocus, SceneChunkStatus
#include "mce/ui/types.h"             // UiAnchors, UiStyle, UiSlice, UiStackLayout, UiGridLayout (values, not components)
