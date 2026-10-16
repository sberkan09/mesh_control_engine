// .luau -> CHNK_SCRIPT (M-SCRIPT 1, ADR-0041): a Luau source compiled to bytecode at cook, so the
// player loads bytecode and never links the compiler. Errors name file:line:col. Built where
// MCE_WITH_SCRIPT is on; elsewhere .luau files are not assets (the cook says so).
//
// sidecar settings: {} (none yet)
#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/script/compiler.h"

namespace mce::cook {
namespace {

constexpr const char* kStage = "cook.script";

class ScriptImporter final : public IImporter {
public:
    StringView id() const override { return "script"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::Script; }
    bool native() const override { return false; }
    bool handles_extension(StringView ext) const override { return ext == ".luau"; }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const StringView file = in.display_path;
        if (!detail::check_keys(in.settings, {}, file, kStage, diags)) return false;
        const StringView source(reinterpret_cast<const char*>(in.source.data()), in.source.size());
        script::CompileError where;
        Result<Array<u8>> bc = script::compile(in.source_path, source, &where);
        if (!bc) {
            if (bc.error().code == Errc::ParseError) diags.error(Errc::ParseError, file, where.line, where.col, kStage, where.message.view());
            else diags.error(bc.error().code, file, 0, 0, kStage, "the Luau compiler ran out of memory");
            return false;
        }
        Array<byte> payload;
        asset::write_script(in.source_path, Span<const byte>(reinterpret_cast<const byte*>(bc.value().data()), bc.value().size()),
                            payload);
        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::Script;
        d.version = asset::chunk_version(asset::SCRIPT_VERSION, 0);
        d.guid = in.guid;
        out.add(d, payload.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_script_importer() { return mem::make_unique<ScriptImporter>(mem::heap()); }

}  // namespace mce::cook
