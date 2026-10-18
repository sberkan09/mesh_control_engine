// M-FUZZ 5: the importers the committed samples cannot feed (the walls' exemptions of step 4): .mcenv baked
// environments, .mchlod HLOD proxies and .glsl shader stages. No sample ships one, so each corpus is built here.
// The .mcenv is a small RGBM cube-free array written by write_envmap. The .mchlod wraps the mesh chunk a tent OBJ
// cooks to, written by write_hlod_file; its trailing hash is recomputed after half of the mutations so the damaged
// payload reaches MeshView::read instead of stopping at the hash check. The .glsl is a fragment stage with a variant
// and bound resources, mutated as bytes, so glslang and the binding-bank check see damaged source.
#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "import_support.h"
#include "mce/asset/container.h"
#include "mce/cook/envmap.h"
#include "mce/cook/hlod.h"
#include "mce/cook/importer.h"
#include "mce/hash.h"
#include "mce/log.h"
#include "mutator.h"
#include "test_support.h"

using namespace mce;

namespace {

u32 iterations() {
    const char* e = std::getenv("MCE_FUZZ_ITERS");
    return e ? static_cast<u32>(std::strtoul(e, nullptr, 10)) : 300u;
}

const cook::IImporter& importer_for(const char* ext) {
    static cook::ImporterRegistry reg;
    static const bool added = (cook::ImporterRegistry::add_builtins(reg), true);
    (void)added;
    const cook::IImporter* imp = reg.for_extension(ext);
    REQUIRE_MESSAGE(imp != nullptr, "no importer for " << ext);
    return *imp;
}

Span<const byte> as_bytes(const std::string& s) { return Span<const byte>(reinterpret_cast<const byte*>(s.data()), s.size()); }

// Mutates `good` `n` times under `seed` and runs each mutant through `imp`; returns how many cooked.
u64 mutate_all(const cook::IImporter& imp, const std::string& rel, const std::string& good, u64 seed, u32 n, bool reseal_hlod) {
    fuzz::Mutator m(seed);
    std::vector<std::uint64_t> storage;
    u64 cooked = 0;
    for (u32 it = 0; it < n; ++it) {
        usize len = m.mutate(as_bytes(good), storage);
        if (reseal_hlod && it % 2 == 1 && len >= 16) {  // keep the hash honest so the payload is what gets read
            const u64 h = fnv1a64(storage.data(), len - 8);
            std::memcpy(reinterpret_cast<byte*>(storage.data()) + len - 8, &h, 8);
        }
        INFO("replay: " << rel << " iteration " << it << " seed " << seed);
        cooked += fuzz::import_bytes(imp, rel, Span<const byte>(reinterpret_cast<const byte*>(storage.data()), len));
    }
    return cooked;
}

}  // namespace

TEST_CASE("fuzz: .mcenv baked environments survive mutation") {
    log::set_level(log::Level::Off);
    const cook::IImporter& imp = importer_for(".mcenv");
    const u32 w = 8, h = 4, layers = 2;
    std::vector<u8> rgba(size_t(w) * h * 4 * layers);
    for (size_t i = 0; i < rgba.size(); ++i) rgba[i] = static_cast<u8>((i * 37 + 11) & 0xff);
    Array<byte> file;
    cook::write_envmap(w, h, layers, Span<const u8>(rgba.data(), rgba.size()), file);
    const std::string good(reinterpret_cast<const char*>(file.data()), file.size());
    REQUIRE_MESSAGE(fuzz::import_bytes(imp, "env/sky.mcenv", as_bytes(good)), "the environment must cook untouched");
    const u32 n = iterations();
    const u64 cooked = mutate_all(imp, "env/sky.mcenv", good, 0x656e766du, n * 2, false);
    std::printf("fuzz: .mcenv %u mutants (%llu cooked)\n", n * 2, static_cast<unsigned long long>(cooked));
    CHECK(cooked > 0);  // a damaged texel is still a valid environment
}

TEST_CASE("fuzz: .mchlod proxies survive mutation") {
    log::set_level(log::Level::Off);
    const cook::IImporter* obj = &importer_for(".obj");
    const std::string tent =
        "o tent\nv -1 0 -1\nv 1 0 -1\nv 1 0 1\nv -1 0 1\nv 0 1.5 0\n"
        "f 1 2 5\nf 2 3 5\nf 3 4 5\nf 4 1 5\nf 4 3 2 1\n";
    asset::ContainerWriter w;
    REQUIRE_MESSAGE(fuzz::import_bytes(*obj, "models/tent.obj", as_bytes(tent), &w), "the tent must cook");
    Array<byte> cooked;
    w.finish(cooked);
    auto c = asset::ContainerView::open(cooked.span());
    REQUIRE(c.has_value());
    const u32 mi = c.value().find(asset::ChunkType::Mesh);
    REQUIRE(mi < c.value().chunk_count());
    Array<byte> file;
    cook::write_hlod_file(c.value().payload(mi), file);
    const std::string good(reinterpret_cast<const char*>(file.data()), file.size());

    const cook::IImporter& imp = importer_for(".mchlod");
    REQUIRE_MESSAGE(fuzz::import_bytes(imp, "hlod/area.mchlod", as_bytes(good)), "the proxy must cook untouched");
    const u32 n = iterations();
    const u64 cooked_n = mutate_all(imp, "hlod/area.mchlod", good, 0x686c6f64u, n * 2, true);
    std::printf("fuzz: .mchlod %u mutants (%llu cooked)\n", n * 2, static_cast<unsigned long long>(cooked_n));
    CHECK(cooked_n > 0);
}

TEST_CASE("fuzz: .glsl shader stages survive mutation") {
    log::set_level(log::Level::Off);
    const cook::IImporter& imp = importer_for(".glsl");
    const std::string good =
        "#version 450 core\n"
        "// @variant LIT\n"
        "layout(location = 0) in vec2 uv;\n"
        "layout(location = 0) out vec4 color;\n"
        "layout(std140, binding = 2) uniform Material { vec4 tint; float gloss; };\n"
        "layout(binding = 6) uniform sampler2D albedo;\n"
        "void main() {\n"
        "#if LIT\n"
        "    color = texture(albedo, uv) * tint * gloss;\n"
        "#else\n"
        "    color = vec4(uv, 0.0, 1.0) * tint;\n"
        "#endif\n"
        "}\n";
    REQUIRE_MESSAGE(fuzz::import_bytes(imp, "shaders/fuzz.frag.glsl", as_bytes(good)), "the stage must cook untouched");
    const u32 n = iterations() / 2;
    const u64 cooked = mutate_all(imp, "shaders/fuzz.frag.glsl", good, 0x676c736cu, n, false);
    // Line-level damage keeps more mutants well formed: a dropped or repeated line, a binding or location number changed.
    fuzz::Mutator lm(0x6c696e65u);
    u64 line_cooked = 0;
    for (u32 it = 0; it < n; ++it) {
        std::vector<std::string> lines;
        for (usize at = 0; at < good.size();) {
            const usize nl = good.find('\n', at);
            lines.push_back(good.substr(at, nl - at + 1));
            at = nl + 1;
        }
        const usize k = 1 + lm.below(lines.size() - 1);  // never the #version line
        switch (lm.below(3)) {
            case 0: lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(k)); break;
            case 1: lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(k), lines[k]); break;
            default:
                for (char& ch : lines[k])
                    if (ch >= '0' && ch <= '9') ch = static_cast<char>('0' + lm.below(10));
                break;
        }
        std::string mutant;
        for (const std::string& l : lines) mutant += l;
        INFO("replay: .glsl line iteration " << it);
        line_cooked += fuzz::import_bytes(imp, "shaders/fuzz.frag.glsl", as_bytes(mutant));
    }
    std::printf("fuzz: .glsl %u byte mutants (%llu cooked), %u line mutants (%llu cooked)\n", n,
                static_cast<unsigned long long>(cooked), n, static_cast<unsigned long long>(line_cooked));
}
