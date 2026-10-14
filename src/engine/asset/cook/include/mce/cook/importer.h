#pragma once
// Importers: interchange/native source bytes + settings -> cooked chunks (assets.md §5.2).
// Every stage is pure — bytes in + settings -> bytes out, no global state, no clock, no paths in
// the output — which is what makes cook determinism (FR-AST-030) structural. Importers are
// cook-time only and never linked into a runtime target (FR-AST-010).

#include "mce/asset/container.h"
#include "mce/cook/diagnostics.h"
#include "mce/cook/json.h"

namespace mce::cook {

using asset::ChunkType;
using asset::Guid;

// A reference an asset makes to another asset, with where in the source it was written.
struct DepRef {
    Guid guid;
    ChunkType expected = ChunkType::Manifest;  // Manifest = any type
    u32 line = 0;
    u32 col = 0;
    FixedString<128> where;  // human context ("textures.base_color", "entities[2].Sprite.atlas")
    // A link only: the target must exist with the expected type, but this asset's cooked bytes do
    // not depend on the target's (a scene naming the next scene keeps just its GUID). No ordering
    // edge, so links may loop (title -> level -> win -> title).
    bool link = false;
};

// Read-only view of the rest of the registry an importer may consult (types of referenced assets).
class IAssetLookup {
public:
    virtual bool type_of(Guid g, ChunkType& out) const = 0;
    // The asset registered at `rel_path` (relative to assets/, forward slashes): how interchange
    // formats that reference siblings by URI (glTF images) name their dependencies.
    virtual bool guid_for_path(StringView rel_path, Guid& out) const = 0;
    // The parsed document of a native asset and its path relative to assets/: how prefab
    // instances are flattened at cook (FR-ENT-070). False when unknown or not a native asset.
    virtual bool native_document(Guid g, JsonValue& out, StringView& source_path) const {
        (void)g;
        (void)out;
        (void)source_path;
        return false;
    }
    // Every project asset of `type`, sorted by GUID: how a font finds the string tables whose
    // glyphs it bakes (M-MOBILE 11a). Base-pack assets are not listed.
    virtual void assets_of_type(ChunkType type, Array<Guid>& out) const {
        (void)type;
        out.clear();
    }
    // A project asset's source bytes and its path relative to assets/; false when unknown. An
    // importer reads only assets it declared as dependencies, so a change re-cooks it.
    virtual bool source_bytes(Guid g, Span<const byte>& out, StringView& source_path) const {
        (void)g;
        (void)out;
        (void)source_path;
        return false;
    }

protected:
    ~IAssetLookup() = default;
};

// How texture importers store color data (CookOptions::textures; M-MOBILE step 8): RGBA8 for
// desktops, ASTC 4x4 blocks for phones (a quarter of the memory and bandwidth).
enum class TextureCompression : u8 { None, Astc4x4 };

struct ImportInput {
    Guid guid;
    StringView display_path;   // "assets/textures/hero.png" — diagnostics only, never cooked
    StringView source_path;    // "textures/hero.png" (relative to assets/) — resolves relative URIs
    Span<const byte> source;   // source file bytes
    JsonValue settings;        // interchange: sidecar "settings" (may be invalid = none);
                               // native: the parsed document root
    const IAssetLookup* lookup = nullptr;
    TextureCompression texture_compression = TextureCompression::None;
};

class IImporter {
public:
    virtual ~IImporter() = default;
    virtual StringView id() const = 0;  // sidecar "importer"
    virtual u32 version() const = 0;    // bump => every asset of this importer re-cooks
    // Hash of the runtime layouts the output bakes in (the scene cook: every reflected component's
    // size and fields). Part of the re-cook key, so a changed component re-cooks its scenes.
    virtual u64 schema_hash() const { return 0; }
    virtual ChunkType type() const = 0; // primary chunk type it produces
    // Native assets (.mcscene, .mcmat) carry their GUID inline; interchange sources use a sidecar.
    virtual bool native() const = 0;
    virtual bool handles_extension(StringView lower_ext) const = 0;

    // Declares the asset's references (graph build). Errors go to `diags`.
    virtual void scan_dependencies(const ImportInput& in, Array<DepRef>& out, DiagList& diags) const {
        (void)in;
        (void)out;
        (void)diags;
    }
    // Produces the asset's chunks. Returns false (with diagnostics) on failure.
    virtual bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const = 0;
};

class ImporterRegistry {
public:
    ImporterRegistry() = default;
    ImporterRegistry(const ImporterRegistry&) = delete;
    ImporterRegistry& operator=(const ImporterRegistry&) = delete;

    // Engine importers only (the set is open for engine types, closed for users — assets.md §7.1).
    void add(mem::UniquePtr<IImporter> importer);
    const IImporter* find(StringView id) const;
    const IImporter* for_extension(StringView lower_ext) const;
    Span<const mem::UniquePtr<IImporter>> all() const { return importers_.span(); }

    // Registry with every built-in importer (texture, wav, font, material, scene, glTF, prefab,
    // tilemap, sprite sheet, particles, ...).
    static void add_builtins(ImporterRegistry& reg);

private:
    Array<mem::UniquePtr<IImporter>> importers_;
};

// Built-in importer factories.
mem::UniquePtr<IImporter> make_texture_importer();
mem::UniquePtr<IImporter> make_wav_importer();
mem::UniquePtr<IImporter> make_font_importer();
mem::UniquePtr<IImporter> make_material_importer();
mem::UniquePtr<IImporter> make_scene_importer();
mem::UniquePtr<IImporter> make_prefab_importer();  // .mcprefab -> CHNK_PREFAB (v0.5)
mem::UniquePtr<IImporter> make_gltf_importer();
mem::UniquePtr<IImporter> make_tilemap_importer();  // .mctilemap -> CHNK_TILE (v0.5)
mem::UniquePtr<IImporter> make_spritesheet_importer();  // .mcsheet -> CHNK_SHEET (M-DATA-REACH 5)
mem::UniquePtr<IImporter> make_animgraph_importer();    // .mcanim -> CHNK_ANIMG (M-DATA-REACH 7)
mem::UniquePtr<IImporter> make_particle_importer();  // .mcpart -> CHNK_PART (v0.5)
mem::UniquePtr<IImporter> make_sequence_importer();  // .mcseq -> CHNK_SEQ (v0.5)
mem::UniquePtr<IImporter> make_input_map_importer(); // .mcinput -> CHNK_INPUT (FR-INP-020)
mem::UniquePtr<IImporter> make_store_importer();     // .mcstore -> CHNK_STORE (M-MONETIZE)
mem::UniquePtr<IImporter> make_services_importer();  // .mcservices -> CHNK_GSVC (M-LIVEOPS game services)
mem::UniquePtr<IImporter> make_config_importer();    // .mcconfig -> CHNK_RCFG (M-LIVEOPS remote config)
mem::UniquePtr<IImporter> make_string_importer();    // assets/loc/<locale>.csv -> CHNK_STR (M-MOBILE 11a)
mem::UniquePtr<IImporter> make_envmap_importer();  // .mcenv -> CHNK_TEX (M-VISUAL 2: baked reflection probes)
mem::UniquePtr<IImporter> make_shader_importer();  // .vert/.frag/.comp.glsl -> CHNK_SHD (v0.5)

// Parses "guid:<canonical>" (the typed asset-reference spelling in native text assets).
bool parse_guid_ref(StringView text, Guid& out);

}  // namespace mce::cook
