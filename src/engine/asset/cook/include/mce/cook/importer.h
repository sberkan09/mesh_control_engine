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
};

// Read-only view of the rest of the registry an importer may consult (types of referenced assets).
class IAssetLookup {
public:
    virtual bool type_of(Guid g, ChunkType& out) const = 0;
    // The asset registered at `rel_path` (relative to assets/, forward slashes): how interchange
    // formats that reference siblings by URI (glTF images) name their dependencies.
    virtual bool guid_for_path(StringView rel_path, Guid& out) const = 0;

protected:
    ~IAssetLookup() = default;
};

struct ImportInput {
    Guid guid;
    StringView display_path;   // "assets/textures/hero.png" — diagnostics only, never cooked
    StringView source_path;    // "textures/hero.png" (relative to assets/) — resolves relative URIs
    Span<const byte> source;   // source file bytes
    JsonValue settings;        // interchange: sidecar "settings" (may be invalid = none);
                               // native: the parsed document root
    const IAssetLookup* lookup = nullptr;
};

class IImporter {
public:
    virtual ~IImporter() = default;
    virtual StringView id() const = 0;  // sidecar "importer"
    virtual u32 version() const = 0;    // bump => every asset of this importer re-cooks
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

    // Registry with every built-in importer (texture, wav, font, material, scene, glTF).
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
mem::UniquePtr<IImporter> make_gltf_importer();

// Parses "guid:<canonical>" (the typed asset-reference spelling in native text assets).
bool parse_guid_ref(StringView text, Guid& out);

}  // namespace mce::cook
