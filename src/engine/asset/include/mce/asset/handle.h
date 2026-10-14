#pragma once
// Asset handles and references (assets.md §0-5, §7.1).
//
//   AssetHandle<T>  generational index into the runtime table — the only cross-frame reference
//                   to loaded asset state. Stale after reload/unload; resolves to a placeholder.
//   AssetRef<T>     what components store: the GUID (serialized) plus the handle the scene
//                   loader stamps at fixup time. Reflected as FieldKind::AssetRef.

#include "mce/asset/container.h"
#include "mce/refl/field_traits.h"

namespace mce::asset {

using AssetKey = u32;  // index into the mounted manifest table
inline constexpr AssetKey kInvalidAssetKey = ~AssetKey(0);

// Asset type tags: the T in AssetHandle<T>/AssetRef<T>. `name` is the reflected type name the
// scene cook checks references against.
// clang-format off
struct Texture  { static constexpr ChunkType chunk = ChunkType::Texture;  static constexpr const char* name = "Texture"; };
struct Mesh     { static constexpr ChunkType chunk = ChunkType::Mesh;     static constexpr const char* name = "Mesh"; };
struct Material { static constexpr ChunkType chunk = ChunkType::Material; static constexpr const char* name = "Material"; };
struct Shader   { static constexpr ChunkType chunk = ChunkType::Shader;   static constexpr const char* name = "Shader"; };
struct Sfx      { static constexpr ChunkType chunk = ChunkType::Sfx;      static constexpr const char* name = "Sfx"; };
struct Font     { static constexpr ChunkType chunk = ChunkType::Font;     static constexpr const char* name = "Font"; };
struct Atlas    { static constexpr ChunkType chunk = ChunkType::Atlas;    static constexpr const char* name = "Atlas"; };
struct Scene    { static constexpr ChunkType chunk = ChunkType::Scene;    static constexpr const char* name = "Scene"; };
struct InputMap { static constexpr ChunkType chunk = ChunkType::InputMap; static constexpr const char* name = "InputMap"; };
struct Prefab   { static constexpr ChunkType chunk = ChunkType::Prefab;   static constexpr const char* name = "Prefab"; };
struct Tilemap  { static constexpr ChunkType chunk = ChunkType::Tilemap;  static constexpr const char* name = "Tilemap"; };
struct SpriteSheet { static constexpr ChunkType chunk = ChunkType::SpriteSheet; static constexpr const char* name = "SpriteSheet"; };
struct AnimGraph { static constexpr ChunkType chunk = ChunkType::AnimGraph; static constexpr const char* name = "AnimGraph"; };
struct Sequence { static constexpr ChunkType chunk = ChunkType::Sequence; static constexpr const char* name = "Sequence"; };
struct ParticlePreset { static constexpr ChunkType chunk = ChunkType::ParticlePreset; static constexpr const char* name = "ParticlePreset"; };
struct StringTable { static constexpr ChunkType chunk = ChunkType::StringTable; static constexpr const char* name = "StringTable"; };
struct StoreCatalog { static constexpr ChunkType chunk = ChunkType::StoreCatalog; static constexpr const char* name = "StoreCatalog"; };
struct GameServicesCatalogChunk { static constexpr ChunkType chunk = ChunkType::GameServicesCatalog; static constexpr const char* name = "GameServicesCatalog"; };
struct RemoteConfigChunk { static constexpr ChunkType chunk = ChunkType::RemoteConfig; static constexpr const char* name = "RemoteConfig"; };
// clang-format on

// Asset type name ("Texture") -> chunk type; false when unknown. Used by the scene cook.
bool chunk_type_for_asset_name(StringView name, ChunkType& out);

template <class T>
struct AssetHandle {
    u32 index = 0;       // AssetKey
    u32 generation = 0;  // 0 = never valid

    constexpr bool valid() const { return generation != 0; }
    friend constexpr bool operator==(AssetHandle a, AssetHandle b) { return a.index == b.index && a.generation == b.generation; }
};

// Type-erased handle for retain/release and events.
struct AnyHandle {
    u32 index = 0;
    u32 generation = 0;
    ChunkType type = ChunkType::Manifest;

    AnyHandle() = default;
    template <class T>
    AnyHandle(AssetHandle<T> h) : index(h.index), generation(h.generation), type(T::chunk) {}  // NOLINT
};

template <class T>
struct AssetRef {
    Guid guid;
    AssetHandle<T> handle;
};
static_assert(sizeof(AssetRef<Texture>) == 24);

}  // namespace mce::asset

// Reflection: AssetRef<T> fields are FieldKind::AssetRef with type_id = StringId(T::name).
template <class T>
struct mce::refl::FieldTraits<mce::asset::AssetRef<T>> {
    static constexpr bool supported = true;
    static constexpr FieldKind kind = FieldKind::AssetRef;
    static constexpr const char* type_name = T::name;
};
