// The engine's reflected components, registered with mce_refl in the MCP server process (same set
// as the editor's and mce_cook's engine_components.cpp). Including a header is enough:
// MCE_COMPONENT emits the registrar next to the type.

#include "mce/audio/components.h"     // AudioSource
#include "mce/ecs/hierarchy.h"        // ChildOf, WorldTransform
#include "mce/ecs/replicated.h"       // NetReplicated, NetOwner (M-NET2)
#include "mce/ecs/save.h"             // SaveKey
#include "mce/ecs/transform.h"        // LocalTransform
#include "mce/kit/anim3d.h"            // AttachToBone, IKTarget3D
#include "mce/kit/bodies3d.h"          // RigidBody3D, CharacterController3D, Trigger3D
#include "mce/kit/components.h"       // Sprite, TextLabel, Camera2DComponent, TilemapLayer, ParticleEmitter,
                                      // Light2D, Parallax, Post2DComponent, SequencePlayer, PathAgent
#include "mce/kit/captions.h"         // SoundCaption (M-ACCESS)
#include "mce/kit/presence.h"         // RichPresence (M-STEAM 3)
#include "mce/kit/proclevel.h"         // ProcLevel (M-PROCGEN 3)
#include "mce/kit/cinematic.h"        // CameraRail3D, CameraLookAt3D, CameraShake3D, CameraBlend3D, Cutscene (M-CINEMA)
#include "mce/kit/video.h"            // VideoPlayer (M-CINEMA 5)
#include "mce/kit/render_textures.h"  // RenderTexture, ShowRenderTexture (M-VIEWS 3)
#include "mce/kit/views.h"            // CameraView (M-VIEWS)
#include "mce/kit/coop.h"             // CoopGame, PlayerSpawn, LocalPlayer, PlayerCamera, SharedCamera2D (M-VIEWS 5)
#include "mce/kit/effects.h"          // SfxTrigger, ParticleBurst, ShakeTrigger, CameraShake2D, CameraFollow2D/3D,
                                      // PhysicsEvents, NavGridSource
#include "mce/kit/joints3d.h"          // Joint3D
#include "mce/kit/kit3d.h"            // MeshRef, PointLight3D, SpotLight3D, Camera3DComponent, Animator, Environment3D
#include "mce/kit/nav3d.h"            // NavMeshSurface3D, NavArea3D, NavLink3D, NavObstacle3D, NavAgent3D
#include "mce/kit/novel.h"             // Dialogue, DialogueInput (M-NOVEL)
#include "mce/kit/novel_components.h"  // StagePart, StoryState, ...
#include "mce/kit/ragdolls3d.h"        // Ragdoll3D
#include "mce/kit/sensors3d.h"         // Raycast3D, Overlap3D
#include "mce/kit/static_merge3d.h"    // StaticMerge3D (M-VISUAL3 5b)
#include "mce/kit/skeleton2d.h"        // Skeleton2D, Bone2DFollow, Skeleton2DAnimator, Skeleton2DSlots, Skeleton2DIK, IKTarget2D (M-ANIM2D)
#include "mce/kit/timeline.h"          // TimelinePlayer (M-CINEMA)
#include "mce/kit/ai.h"                // AiBrain, Blackboard (M-AI)
#include "mce/kit/perception.h"        // Perceiver, Stimulus (M-AI 3)
#include "mce/kit/roads.h"             // RoadNetwork3D (M-OPENWORLD B1)
#include "mce/kit/vehicles3d.h"        // Vehicle3D
#include "mce/kit/seats3d.h"           // VehicleSeat3D, Rider3D (M-OPENWORLD C1)
#include "mce/kit/combat3d.h"          // Health3D, Weapon3D, Projectile3D (M-OPENWORLD C2)
#include "mce/kit/wanted3d.h"          // Wanted3D, Police3D (M-OPENWORLD C3)
#include "mce/kit/ui_flow.h"          // UiFlowRules (M-WORKFLOW 7)
#include "mce/physics/components.h"   // RigidBody2D, Collider2D, CharacterController2D, ...
#include "mce/scene/streaming.h"      // SceneChunk, StreamingFocus, SceneChunkStatus
#include "mce/ui/types.h"             // UiAnchors, UiStyle, UiSlice, UiStackLayout, UiGridLayout (values, not components)
