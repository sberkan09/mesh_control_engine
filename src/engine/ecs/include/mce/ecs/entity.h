#pragma once
// Entity handles and component identity (ecs-gameplay.md §2.1, §3.1).

#include <type_traits>

#include "mce/containers/bitset.h"
#include "mce/refl/field_traits.h"
#include "mce/type_name.h"

namespace mce {

// The ONLY thing gameplay code may hold across frames. 8 bytes, trivially copyable.
struct Entity {
    u32 index = 0;
    u32 generation = 0;  // 0 = null; bumped on destroy so stale handles fail, never alias

    constexpr bool is_null() const { return generation == 0; }
    friend constexpr bool operator==(Entity a, Entity b) { return a.index == b.index && a.generation == b.generation; }
    static const Entity null;
};
inline constexpr Entity Entity::null{};

template <>
struct Hash<Entity> {
    u64 operator()(Entity e) const { return mix64((u64(e.generation) << 32) | e.index); }
};

}  // namespace mce

// Reflection: Entity fields are FieldKind::EntityRef. Scenes spell them "$<entity index>" (the
// cook stores the scene-local index; the scene loader stamps the spawned entity).
template <>
struct mce::refl::FieldTraits<mce::Entity> {
    static constexpr bool supported = true;
    static constexpr FieldKind kind = FieldKind::EntityRef;
    static constexpr const char* type_name = "Entity";
};

namespace mce {
namespace ecs {

using ComponentId = u16;
inline constexpr usize kMaxComponents = 256;
inline constexpr ComponentId kInvalidComponent = 0xFFFF;
using ComponentMask = BitSet<kMaxComponents>;

// Components are reflected plain structs: trivially copyable, no heap, no destructors. Row moves
// are memcpy and state_hash hashes their bytes, so avoid padding (MCE_COMPONENT in mce_refl,
// M2, switches hashing to per-field).
template <class T>
inline constexpr bool is_component_v = std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T> &&
                                       !std::is_const_v<T> && !std::is_reference_v<T>;

struct ComponentInfo {
    u64 type_hash = 0;
    StringView name;
    u32 size = 0;
    u32 align = 0;
    bool is_tag = false;  // empty struct: lives in the mask only, no column
    u64 refl_id = 0;      // StringId of the mce_refl type name (0 = not reflected); how cooked
                          // scene/prefab columns find their ComponentId
};

// One component value for the type-erased spawn paths (prefab/scene instantiation).
struct RawComponent {
    ComponentId id = kInvalidComponent;
    const void* data = nullptr;  // component_size bytes; ignored for tags
};

template <class T>
ComponentInfo component_info() {
    static_assert(is_component_v<T>, "components must be trivially copyable plain structs");
    ComponentInfo info;
    info.type_hash = kTypeHash<T>;
    info.name = type_name<T>();
    info.is_tag = std::is_empty_v<T>;
    info.size = info.is_tag ? 0u : static_cast<u32>(sizeof(T));
    info.align = static_cast<u32>(alignof(T));
    if constexpr (refl::ReflectedStruct<T>) info.refl_id = refl::type_id<T>().value;
    return info;
}

}  // namespace ecs
}  // namespace mce
