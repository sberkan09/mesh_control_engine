// The engine's reflected components, registered with mce_refl in the editor process so the
// inspector, the add-component menu and scene load see them (same set as mce_cook's
// engine_components.cpp). Including a header is enough: MCE_COMPONENT emits the registrar.

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
#include "mce/kit/chunk_lod3d.h"       // ChunkLod3D (M-OPENWORLD A4)
#include "mce/kit/static_merge3d.h"    // StaticMerge3D (M-VISUAL3 5b)
#include "mce/kit/skeleton2d.h"        // Skeleton2D, Bone2DFollow, Skeleton2DAnimator, Skeleton2DSlots, Skeleton2DIK, IKTarget2D (M-ANIM2D)
#include "mce/kit/timeline.h"          // TimelinePlayer (M-CINEMA)
#include "mce/kit/ai.h"                // AiBrain, Blackboard (M-AI)
#include "mce/kit/perception.h"        // Perceiver, Stimulus (M-AI 3)
#include "mce/kit/crowd.h"             // Pedestrian3D, CrowdSpawner3D (M-OPENWORLD B3)
#include "mce/kit/roads.h"             // RoadNetwork3D (M-OPENWORLD B1)
#include "mce/kit/roadblock.h"         // Roadblock3D, RoadblockPiece3D (M-OPENWORLD-POLICE 3)
#include "mce/kit/traffic.h"           // TrafficDriver3D, TrafficSpawner3D (M-OPENWORLD B2)
#include "mce/kit/vehicles3d.h"        // Vehicle3D
#include "mce/kit/seats3d.h"           // VehicleSeat3D, Rider3D (M-OPENWORLD C1)
#include "mce/kit/combat3d.h"          // Health3D, Weapon3D, Projectile3D (M-OPENWORLD C2)
#include "mce/kit/inventory.h"         // Inventory, Equipment, ItemPickup3D (M-INVENTORY 2)
#include "mce/kit/loot.h"              // LootDrop, Container (M-INVENTORY 3)
#include "mce/kit/craft.h"              // CraftingStation (M-CRAFT 2)
#include "mce/kit/status.h"             // StatusEffects (M-STATUS 1)
#include "mce/kit/missions.h"          // Mission, Objective (M-MISSION 1)
#include "mce/kit/world_clock.h"        // WorldClock (M-DAYNIGHT 1)
#include "mce/kit/day_cycle.h"          // DayCycle (M-DAYNIGHT 2)
#include "mce/kit/timed_light.h"       // TimedLight3D (M-DAYNIGHT 3)
#include "mce/kit/vehicle_extras.h"    // VehicleHorn3D, VehicleLights3D (M-OWHORN)
#include "mce/kit/hit_marker.h"         // HitMarker3D (M-HITMARKER)
#include "mce/kit/impact_fx.h"         // ImpactFx3D (M-IMPACTFX)
#include "mce/kit/fall_death.h"        // FallDeath3D (M-FALLDEATH)
#include "mce/kit/face_velocity.h"     // FaceVelocity3D (M-FACEMOVE)
#include "mce/kit/drive_readout.h"      // DriveReadout3D (M-DRIVEHUD)
#include "mce/kit/precipitation.h"      // Precipitation3D, Lightning (M-WEATHER 3)
#include "mce/kit/weather.h"            // Weather (M-WEATHER 1)
#include "mce/kit/wet_surfaces.h"       // WetSurfaces (M-WETSURFACE 5c)
#include "mce/kit/snow_cover.h"         // SnowCover (M-SNOWCOVER 6c)
#include "mce/kit/wind_direction.h"     // WindDirection (M-WINDDIR 8a)
#include "mce/kit/thunder_delay.h"      // ThunderDelay (M-RAINFX 7e)
#include "mce/kit/rain_splash.h"        // RainSplash (M-RAINFX 7f)
#include "mce/kit/rain_ripples.h"        // RainRipples (M-RAINFX 7c)
#include "mce/kit/cloud_layer.h"         // CloudLayer (M-CLOUDS 9c)
#include "mce/kit/weather_audio.h"       // WeatherAudio (M-WEATHERAUDIO 10a)
#include "mce/kit/world_audio.h"        // EngineAudio, WeaponAudio, FootstepAudio, SirenAudio (M-OPENWORLD E3)
#include "mce/kit/lightning_bolt.h"      // LightningBolt (M-LIGHTNINGBOLT 11a)
#include "mce/kit/ground_mist.h"         // GroundMist (M-GROUNDMIST 13a)
#include "mce/kit/moon.h"                 // Moon (M-NIGHTSKY 14a)
#include "mce/kit/water.h"                // Water (M-WATER 15a)
#include "mce/kit/aurora.h"               // Aurora (M-AURORA 16a)
#include "mce/kit/rainbow.h"              // Rainbow (M-RAINBOW 17a)
#include "mce/kit/meteor.h"               // Meteor (M-METEOR 18a)
#include "mce/kit/season.h"               // Season (M-SEASONS 19a)
#include "mce/kit/water_ice.h"            // WaterIce (M-FREEZE 20a)
#include "mce/kit/leaf_fall.h"            // LeafFall (M-LEAFFALL 21a)
#include "mce/kit/firefly.h"              // Firefly (M-FIREFLIES 23a)
#include "mce/kit/pollen.h"               // Pollen (M-POLLEN 24a)
#include "mce/kit/dust.h"                 // Dust (M-DUSTWIND 25a)
#include "mce/kit/flock.h"                // Flock (M-BIRDFLOCK 26a)
#include "mce/kit/butterfly.h"            // Butterfly (M-BUTTERFLY 27a)
#include "mce/kit/dusk_bat.h"                  // DuskBat (M-BAT 30a)
#include "mce/kit/wind_gust.h"           // WindGust (M-WINDGUST 12a)
#include "mce/kit/wind_sway.h"          // WindSway (M-WINDSWAY 1)
#include "mce/kit/map_markers.h"       // MapMarker (M-MISSION 2)
#include "mce/kit/wanted3d.h"          // Wanted3D, Police3D (M-OPENWORLD C3)
#include "mce/kit/vehicle_ram.h"        // VehicleRam3D (M-VEHICLEHIT)
#include "mce/kit/impact_damage.h"       // ImpactDamage3D (M-CONTACTEVENTS)
#include "mce/kit/ui_flow.h"          // UiFlowRules (M-WORKFLOW 7)
#include "mce/physics/components.h"   // RigidBody2D, Collider2D, CharacterController2D, ...
#include "mce/physics/force2d.h"       // LaunchVelocity2D, Impulse2D, ConstantForce2D
#include "mce/physics/joint2d.h"       // Joint2D
#include "mce/scene/streaming.h"      // SceneChunk, StreamingFocus, SceneChunkStatus
#include "mce/ui/types.h"             // UiAnchors, UiStyle, UiSlice, UiStackLayout, UiGridLayout (values, not components)
