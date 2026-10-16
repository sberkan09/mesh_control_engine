#pragma once
// 2D skeletons (M-ANIM2D 1): cut-out characters made of textured quads (slots) riding a bone tree.
// A .mcskel2d asset (asset::Skeleton2DView) holds the bones' rest pose and the slots; a Skeleton2D
// component poses one on its entity and the sprite extract draws its slots.
//
//   kit::install_skeleton2d(world, &assets);   // registry + "kit_skeleton2d" (install_stock does it)
//   ... an entity with LocalTransform + Skeleton2D{skeleton = guid of hero.mcskel2d}
//
// Pose. Each bone's local pose is its rest pose (from the asset) combined with the component's
// per-bone offset `pose[i]`: x, y and rotation add, scale_x and scale_y multiply. The default
// offset is zero (scale one), so an untouched skeleton stands in its rest pose. A .mcscene lists
// all 64 `pose` entries (`{}` keeps a bone at rest); tools set one with "pose[i].rotation". A bone's world transform is its parent's world transform times
// its local translate * rotate * scale (Spine and Godot inherit the same way, so a parent's
// non-uniform scale shears rotated children). Bones are in parent-before-child order, so one pass
// poses the tree. World transforms are in the skeleton's space: the entity's LocalTransform (or
// WorldTransform) places the skeleton, flip_x / flip_y mirror it about the entity's origin.
//
// The "kit_skeleton2d" system (FixedUpdate_Post) writes every Skeleton2D's `world` and moves each
// Bone2DFollow entity onto its bone. Everything lives in components, so poses hash, replay and
// save: `pose` carries the Save flag, `world` is derived and recomputed every tick.
//
// Drawing. kit::SpriteExtract draws each visible Skeleton2D's slots in slot order as sprites: at
// entity * flip * bone world * slot offset and rotation, with the slot's texture rect, size, pivot
// and tint (times the component's tint), on the component's sorting layer and order. Slots that
// share a texture draw in slot order; within one layer the 2D batch groups by texture before
// order (rendering-2d D11), so give a skeleton one atlas texture for an exact slot order. A
// sprite cannot shear: a slot under a rotated, non-uniformly scaled bone keeps the axis lengths
// and loses the shear.
//
// Limits: 64 bones (the component holds one pose per bone; the cook refuses more) and 256 slots.
//
// Animation (M-ANIM2D 2). The .mcskel2d's "animations" are clips of per-bone keys (rotate,
// translate, scale; offsets from the rest pose like `pose`) and timed events. A
// Skeleton2DAnimator next to the Skeleton2D plays them: "kit_skeleton2d_animators" runs just
// before "kit_skeleton2d" in FixedUpdate_Post, advances the playhead, cross-fades and writes the
// animated channels into `pose`. A channel (one bone's rotation, translation or scale) that no
// clip being sampled keys is not written: gameplay and timelines own it. With a `graph` (an
// .mcanim AnimGraph, the 3D Animator's format) the graph's states name the skeleton's animations.

#include "mce/asset/asset_system.h"
#include "mce/asset/formats.h"
#include "mce/containers/array.h"
#include "mce/ecs/world.h"
#include "mce/fixed_string.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

inline constexpr u32 kSkeleton2DBones = asset::kSkeleton2DMaxBones;

// A bone's local pose, or (in Skeleton2D::pose) its offset from the rest pose. Metres and radians.
struct Bone2DPose {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 rotation = 0.0f;  // radians, counter-clockwise
    f32 scale_x = 1.0f;
    f32 scale_y = 1.0f;
    friend bool operator==(const Bone2DPose&, const Bone2DPose&) = default;
};
MCE_REFL_STRUCT(Bone2DPose, x, y, (rotation, Help{"radians, counter-clockwise"}), scale_x, scale_y)

// A 2D affine transform: p' = (a * p.x + c * p.y + x, b * p.x + d * p.y + y). (a, b) is the
// transformed +x axis, (c, d) the transformed +y axis, (x, y) the origin.
struct Bone2DTransform {
    f32 a = 1.0f;
    f32 b = 0.0f;
    f32 c = 0.0f;
    f32 d = 1.0f;
    f32 x = 0.0f;
    f32 y = 0.0f;

    math::vec2 apply(math::vec2 p) const { return {a * p.x + c * p.y + x, b * p.x + d * p.y + y}; }
    // Translate * rotate (radians) * scale.
    static Bone2DTransform trs(math::vec2 t, f32 radians, math::vec2 s);
    friend Bone2DTransform operator*(const Bone2DTransform& p, const Bone2DTransform& q);  // p after q
    friend bool operator==(const Bone2DTransform&, const Bone2DTransform&) = default;
};
MCE_REFL_STRUCT(Bone2DTransform, a, b, c, d, x, y)

// Plays a .mcskel2d on its entity (kit::install_skeleton2d) and draws its slots through the sprite
// extract. `pose[i]` offsets bone i from its rest pose (written by gameplay, timelines through
// "Skeleton2D.pose[i].rotation", by a Skeleton2DAnimator, and later by IK); `world` is engine-written.
struct Skeleton2D {
    asset::AssetRef<asset::Skeleton2D> skeleton;
    math::Color tint;     // multiplies every slot's tint
    i32 order = 0;        // refines stacking inside a layer (as Sprite)
    i16 layer = 0;        // authoritative stacking (as Sprite)
    bool flip_x = false;  // mirror the skeleton about the entity's origin
    bool flip_y = false;
    bool visible = true;
    u8 pad_[3] = {};
    u32 bone_count = 0;   // engine-written: the bones `world` holds
    Bone2DPose pose[kSkeleton2DBones];
    Bone2DTransform world[kSkeleton2DBones];  // engine-written: each bone in the skeleton's space
};
MCE_COMPONENT(Skeleton2D, (skeleton, Help{"a .mcskel2d skeleton"}), (tint, Help{"multiplies every slot's tint"}),
              (order, Help{"refines stacking inside a layer"}), (layer, Range{-100, 100}),
              (flip_x, Help{"mirror the skeleton about the entity's origin"}), flip_y, (visible, Help{"draw the slots"}),
              (bone_count, ReadOnly, Transient),
              (pose, Save, Help{"per-bone offset from the rest pose: x, y, rotation (radians) add; scale_x, scale_y multiply"}),
              (world, ReadOnly, Transient, Help{"engine-written: each bone's transform in the skeleton's space"}))

// Moves this entity onto a bone of a Skeleton2D every tick (weapons, effects, later IK targets):
// its LocalTransform becomes the bone's transform times `offset` and `rotation`. When this entity
// is a ChildOf the skeleton's entity the transform is in the skeleton's space, otherwise in the
// world (the skeleton entity's placement included). z keeps its value.
struct Bone2DFollow {
    Entity skeleton;               // the entity with the Skeleton2D
    FixedString<32> bone;          // the bone's name
    math::vec2 offset;             // metres in the bone's space
    f32 rotation = 0.0f;           // radians, added to the bone's
    bool follow_rotation = true;
    bool follow_scale = false;
    u8 pad_[2] = {};
};
MCE_COMPONENT(Bone2DFollow, (skeleton, Help{"the entity with the Skeleton2D"}), (bone, Help{"the bone's name"}),
              (offset, Help{"metres in the bone's space"}), (rotation, Help{"radians added to the bone's"}), follow_rotation,
              follow_scale)

// Plays the animations of its entity's Skeleton2D asset (M-ANIM2D 2), modelled on the 3D Animator
// (kit3d.h). `clip` names an animation; a new name starts it at 0 and cross-fades from the old
// over `fade` seconds. `time` advances by dt * speed every tick. A clip loops when its asset says
// "loop" (the default) and `loop` is on; otherwise it plays once, holds its last pose and sets
// `finished`; `restart` plays it again.
//
// Each tick the animator samples the clip (and the clip fading out, weight 1 - `blend`) and
// writes the blended value of every channel those clips key into Skeleton2D::pose. A clip that
// does not key a channel the other one keys contributes the rest offset (zero, scale one) to the
// blend. Channels no sampled clip keys keep what gameplay wrote. Rotations blend linearly as
// offsets (radians), scales linearly as factors.
//
// With `graph` (an .mcanim AnimGraph) the graph drives the animator exactly as it drives the 3D
// Animator: `state` is the state playing, `params` its parameters in the order it lists them,
// transitions on parameter conditions or a one-shot's end cross-fade over their own fade, 1D and
// 2D blend states mix their clips at the same phase (`blend_clip`, `blend_weight`), and `restart`
// returns to the start state. A state's clip names one of the skeleton's animations; clip lengths
// come from the skeleton. Layers, root motion and IK chains of a graph are ignored in 2D.
//
// Events. A clip's events (and a graph state's) raise kit::AnimEvent3D{entity, StringId of the
// name} (anim3d.h) as the playhead crosses them, once per crossing (a loop wrap crosses again);
// `last_event` and `events_fired` record them. Every field is saved, so a save restores a
// cross-fade mid-way.
struct Skeleton2DAnimator {
    FixedString<32> clip;
    FixedString<32> state;                    // runtime: the graph state playing
    asset::AssetRef<asset::AnimGraph> graph;  // optional state graph (.mcanim)
    u64 playing = 0;                          // runtime: StringId of the animation `time` belongs to
    u64 previous = 0;                         // runtime: the animation fading out
    u64 blend_clip = 0;                       // runtime: a blend state's second animation
    math::vec4 params;                        // the graph's parameters, in its order
    FixedString<24> last_event;               // runtime: the last event's name
    f32 speed = 1.0f;
    f32 fade = 0.2f;                          // cross-fade seconds (0 = cut); a graph transition sets its own
    f32 time = 0.0f;                          // seconds into `clip`
    f32 previous_time = 0.0f;                 // runtime: seconds into `previous`
    f32 blend = 1.0f;                         // runtime: 0..1 weight of `clip` against `previous`
    f32 blend_weight = 0.0f;                  // runtime: 0..1 weight of `blend_clip` against `clip`
    u32 events_fired = 0;                     // runtime
    bool loop = true;                         // without a graph: false plays `clip` once
    bool finished = false;                    // runtime: a one-shot clip or state reached its end
    bool restart = false;                     // set to replay from the start (the engine clears it)
    u8 pad_ = 0;
};
MCE_COMPONENT(Skeleton2DAnimator, (clip, Save, Help{"an animation of the entity's Skeleton2D"}),
              (state, ReadOnly, Transient, Save), (graph, Help{"an AnimGraph (.mcanim) whose states name the animations"}),
              (playing, ReadOnly, Transient, Save), (previous, ReadOnly, Transient, Save),
              (blend_clip, ReadOnly, Transient, Save), (params, Save, Help{"the graph's parameters, in the order it lists them"}),
              (last_event, ReadOnly, Transient, Save), (speed, Range{-8, 8}, Save), (fade, Range{0, 5}, Save), (time, Save),
              (previous_time, ReadOnly, Transient, Save), (blend, ReadOnly, Transient, Save),
              (blend_weight, ReadOnly, Transient, Save), (events_fired, ReadOnly, Transient, Save),
              (loop, Save, Help{"without a graph: off plays the clip once"}), (finished, ReadOnly, Transient, Save),
              (restart, Save, Help{"replay from the start (the engine clears it)"}))

class Skeleton2DRegistry {
public:
    Skeleton2DRegistry() = default;
    explicit Skeleton2DRegistry(asset::AssetSystem* assets) : assets_(assets) {}
    void set_assets(asset::AssetSystem* assets) { assets_ = assets; }
    asset::AssetSystem* assets() const { return assets_; }

    // A skeleton copied out of its chunk.
    struct Def {
        asset::Guid guid;
        asset::AssetKey key = asset::kInvalidAssetKey;
        u32 generation = 0;
        FixedString<32> name;
        Array<asset::Skeleton2DBone> bones;
        Array<asset::Skeleton2DSlot> slots;
        Array<asset::Skeleton2DAnimation> animations;
        Array<asset::Skeleton2DTimeline> timelines;
        Array<asset::Skeleton2DKey> keys;
        Array<asset::Skeleton2DEvent> events;
        Array<u64> animation_ids;  // StringId of each animation's name
        Array<char> strings;
        StringView string(u32 offset) const;
        i32 find_bone(StringView name) const;  // -1 when none
        i32 find_animation(u64 id) const;      // by StringId of the name; -1 when none
        f32 duration(u64 id) const;            // an animation's length; 0 when none
    };

    // The definition for a GUID: its cooked asset (copied out once, re-read on hot reload), or one
    // registered with define. Null when it does not resolve (warned once).
    const Def* resolve(asset::Guid guid);
    // Registers (or replaces) the skeleton for `guid` from a CHNK_SKEL2D payload, ahead of the
    // asset system: tools preview unsaved edits through it, tests skip the cook.
    Result<void> define(asset::Guid guid, Span<const byte> payload);

    // The AnimGraph for a GUID (a Skeleton2DAnimator's graph): one registered with define_graph,
    // else its cooked asset (reloaded when it hot-reloads). False when it does not resolve.
    bool graph(asset::Guid guid, asset::AnimGraphView& out);
    // Registers (or replaces) a CHNK_ANIMG payload for `guid`, ahead of the asset system.
    Result<void> define_graph(asset::Guid guid, Span<const byte> payload);

private:
    struct Graph {
        asset::Guid guid;
        asset::AnyHandle handle;
        Array<byte> bytes;  // a defined graph's payload
        bool defined = false;
        bool failed = false;
    };
    asset::AssetSystem* assets_ = nullptr;
    Array<Def> defs_;
    Array<asset::Guid> warned_;
    Array<Graph> graphs_;
};

// ---------------------------------------------------------------- pure pose math

// Local translate * rotate * scale of a pose.
Bone2DTransform bone2d_local(const Bone2DPose& p);

// A bone's local pose: its rest pose with `offset` applied (x, y, rotation add; scales multiply).
Bone2DPose bone2d_offset(const asset::Skeleton2DBone& rest, const Bone2DPose& offset);

// World transforms of `bones` (parent-before-child, as Skeleton2DView::read guarantees) from their
// local poses: world[i] = world[parent] * local(local[i]), a root's world = its local.
// `local` and `world` hold at least bones.size() entries.
void skeleton2d_world_pose(Span<const asset::Skeleton2DBone> bones, Span<const Bone2DPose> local, Span<Bone2DTransform> world);

// The same from rest poses and per-bone offsets (Skeleton2D::pose); `offsets` may be shorter than
// `bones` (the rest take no offset).
void skeleton2d_pose_offsets(Span<const asset::Skeleton2DBone> bones, Span<const Bone2DPose> offsets, Span<Bone2DTransform> world);

// One slot as a sprite: what kit::SpriteExtract hands the 2D batch (it adds the sprite set).
struct Skeleton2DQuad {
    asset::Guid texture;     // null: a solid quad
    math::vec4 rect;         // u0, v0, u1, v1
    math::vec2 position;     // world position of the pivot
    f32 rotation = 0.0f;     // radians
    math::vec2 scale;        // world size in metres (negative when mirrored)
    math::vec2 pivot;
    math::Color tint;
    u32 slot = 0;            // index into the skeleton's slots
};

// The visible slots of `sk` (posed from `def` and sk.pose, not sk.world, so an un-ticked editor
// scene draws too) in draw order, placed by `entity` (the entity's 2D transform). Appends to `out`.
void skeleton2d_quads(const Skeleton2DRegistry::Def& def, const Skeleton2D& sk, const Bone2DTransform& entity, Array<Skeleton2DQuad>& out);

// The 2D transform of an entity placement (position.xy, rotation about +Z, scale.xy).
Bone2DTransform transform_2d(const math::vec3& position, const math::quat& rotation, const math::vec3& scale);

// ---------------------------------------------------------------- systems

// Registers Skeleton2D, Bone2DFollow and Skeleton2DAnimator and the AnimEvent3D event, sets the
// Skeleton2DRegistry resource and adds, in this order, to FixedUpdate_Post:
//   "kit_skeleton2d_animators"  Skeleton2DAnimator time, cross-fades, graphs, events; writes pose
//   "kit_skeleton2d"            every Skeleton2D's world pose, then the Bone2DFollow entities
// Idempotent (re-points the assets).
void install_skeleton2d(World& world, asset::AssetSystem* assets);

// The systems themselves (exposed for tests and custom pipelines).
void kit_skeleton2d(Frame& f);
void kit_skeleton2d_animators(Frame& f);

// A weighted blend of animation samples, as the animator builds it each tick (exposed for tests and
// tools that preview a clip). add() samples animation `anim` of `def` at `t` seconds (a looping
// animation wraps past its end, a one-shot clamps) and adds each keyed channel's value times `w`.
// write() sets the keyed channels of `pose`: rotation and translation are the weighted sums,
// scale the weighted sum plus one (the rest factor) times the weight of the samples that did not
// key it. Channels no sample keyed keep their value.
struct Skeleton2DBlend {
    Bone2DPose acc[kSkeleton2DBones];      // weighted sums (scale starts at 0)
    math::vec3 weight[kSkeleton2DBones];   // per bone: rotate, translate, scale weight that keyed it
    u8 keyed[kSkeleton2DBones] = {};       // per bone: bit 0 rotate, bit 1 translate, bit 2 scale

    Skeleton2DBlend();
    void add(const Skeleton2DRegistry::Def& def, u32 anim, f32 t, f32 w);
    void write(u32 bone_count, Bone2DPose* pose) const;
};

// A bone's world transform in the skeleton's space by name, from `e`'s Skeleton2D (as the last
// tick posed it). False when `e` has no Skeleton2D, it does not resolve, or no bone has the name.
bool skeleton2d_bone(World& world, Entity e, StringView bone, Bone2DTransform& out);

}  // namespace mce::kit
