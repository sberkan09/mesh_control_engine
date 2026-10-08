#pragma once
// Scene and prefab instantiation (v0.5, ecs-gameplay.md §2.11/§3.8, FR-ENT-060/070).
//
// A cooked CHNK_SCENE or CHNK_PREFAB payload (asset::SceneView — one layout for both) becomes
// live rows: per template entity a bulk copy of its component rows, entity references remapped
// from template-local indices to the fresh handles, asset references optionally stamped by the
// caller, then the instance overrides written through reflection property paths. Nested prefabs
// were flattened by the cook, so instantiation never consults another prefab (no runtime
// inheritance — `IsA` is rejected, research §9).
//
// This lives in mce_ecs_scene, above both mce_ecs and mce_asset: the ECS itself includes no
// asset header. Component identity: every chunk column names its type by mce_refl name; the
// World must have that component registered (register_components<T>()) — an unregistered type
// is an Errc::NotFound naming the column, never a silent drop.
//
// Inside a phase, instantiation records Commands (the entities appear at the sync point, handles
// valid at once); outside a phase the rows exist on return.

#include <cstring>

#include "mce/asset/formats.h"
#include "mce/asset/handle.h"
#include "mce/ecs/world.h"

namespace mce::ecs {

// One instance override: `path` = "<Component>.<property path>", e.g. "LocalTransform.position"
// or "Health.hp". Applied to template entity `entity` (0 = the prefab root) of every replica.
// A component the template entity lacks is added (default-constructed) before the write.
struct PrefabOverride {
    u32 entity = 0;
    FixedString<120> path;
    refl::FieldKind kind = refl::FieldKind::Bool;
    u32 size = 0;
    StringId type_id;  // Struct/Enum/AssetRef/EntityRef leaf type
    alignas(16) u8 value[64] = {};

    template <class V>
    static PrefabOverride of(StringView path, const V& v, u32 entity = 0) {
        using Traits = refl::FieldTraits<V>;
        static_assert(Traits::supported, "override value type is not reflectable");
        static_assert(sizeof(V) <= sizeof(value), "override values are at most 64 bytes");
        static_assert(std::is_trivially_copyable_v<V>, "override values are plain data");
        PrefabOverride o;
        o.entity = entity;
        o.path.assign(path);
        o.kind = Traits::kind;
        o.size = static_cast<u32>(sizeof(V));
        if constexpr (Traits::type_name != nullptr) o.type_id = StringId(Traits::type_name);
        std::memcpy(o.value, &v, sizeof(V));
        return o;
    }
};

// Stamps the runtime handle of an asset reference in place (`field` points at the AssetRef<T>
// inside the new row: the Guid is already there). Return false to fail the instantiation.
using AssetResolveFn = bool (*)(void* ctx, const asset::SceneAssetRef& ref, void* field);

struct SpawnParams {
    Span<const PrefabOverride> overrides;
    // Template roots (entities without a ChildOf) are attached under this parent when non-null.
    Entity parent = Entity::null;
    u32 count = 1;  // replicas; handles are appended replica by replica, template order inside
    AssetResolveFn resolve_asset = nullptr;
    void* resolve_ctx = nullptr;
};

// Instantiates every template entity `p.count` times. Returns the number of entities spawned;
// `out` (optional) receives their handles — replica k's root is out[k * entity_count].
// Errors (nothing is spawned): Errc::NotFound (unregistered component / override target),
// Errc::InvalidArgument (size mismatch, bad override path or value type, bad template index).
Result<u32> instantiate(World& w, const asset::SceneView& tmpl, const SpawnParams& p = {}, Array<Entity>* out = nullptr);
Result<u32> instantiate(World& w, Span<const byte> chunk_payload, const SpawnParams& p = {}, Array<Entity>* out = nullptr);

// A scene load is one instantiation of the scene template with no overrides.
inline Result<u32> load_scene(World& w, const asset::SceneView& scene, Array<Entity>* out = nullptr,
                              AssetResolveFn resolve = nullptr, void* resolve_ctx = nullptr) {
    SpawnParams p;
    p.resolve_asset = resolve;
    p.resolve_ctx = resolve_ctx;
    return instantiate(w, scene, p, out);
}

}  // namespace mce::ecs
