// M-FUZZ 4: the mesh importers the committed samples cannot feed. samples/import_demo ships a glTF, an ASCII FBX, an
// MTL and an OBJ (the importers test mutates those), but nothing in the tree carries a meshopt-compressed glTF and
// the sample OBJ is tiny, so the corpora are built here too: a small OBJ with a material library, and GLB files whose vertex and index
// buffers are EXT_meshopt_compression streams (plain, and with the EXPONENTIAL filter). Each is mutated byte-wise
// and, for the OBJ, line by line, then run through the importer; the meshopt decoder sees damaged streams and damaged
// stream descriptions (count, stride, mode, filter).
#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <string>
#include <vector>

#include "meshoptimizer.h"
#include "mce/asset/container.h"
#include "mce/cook/importer.h"
#include "import_support.h"
#include "mce/log.h"
#include "mutator.h"
#include "test_support.h"

using namespace mce;

namespace {

u32 iterations() {
    const char* e = std::getenv("MCE_FUZZ_ITERS");
    return e ? static_cast<u32>(std::strtoul(e, nullptr, 10)) : 300u;
}

void put32(std::string& s, u32 v) {
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

std::string make_glb(std::string json, std::string bin) {
    while (json.size() % 4) json.push_back(' ');
    while (bin.size() % 4) bin.push_back('\0');
    std::string out = "glTF";
    put32(out, 2);
    put32(out, static_cast<u32>(12 + 8 + json.size() + 8 + bin.size()));
    put32(out, static_cast<u32>(json.size()));
    out += "JSON";
    out += json;
    put32(out, static_cast<u32>(bin.size()));
    out += std::string("BIN\0", 4);
    out += bin;
    return out;
}

// A grid of `n` x `n` quads: positions only (stride 12) and u16 triangle indices, both meshopt streams.
std::string meshopt_glb(bool filtered) {
    const u32 n = 6, verts = (n + 1) * (n + 1);
    std::vector<float> pos;
    for (u32 y = 0; y <= n; ++y)
        for (u32 x = 0; x <= n; ++x) {
            pos.push_back(static_cast<float>(x));
            pos.push_back(static_cast<float>((x * 7 + y * 3) % 5) * 0.25f);
            pos.push_back(static_cast<float>(y));
        }
    std::vector<unsigned int> idx;
    for (u32 y = 0; y < n; ++y)
        for (u32 x = 0; x < n; ++x) {
            const u32 a = y * (n + 1) + x, b = a + 1, c = a + n + 1, d = c + 1;
            for (u32 v : {a, c, b, b, c, d}) idx.push_back(v);
        }
    if (filtered) {
        std::vector<unsigned int> enc(pos.size());
        meshopt_encodeFilterExp(enc.data(), verts, 12, 15, pos.data(), meshopt_EncodeExpSeparate);
        std::memcpy(pos.data(), enc.data(), enc.size() * sizeof(unsigned int));
    }
    std::string vs(meshopt_encodeVertexBufferBound(verts, 12), '\0');
    vs.resize(meshopt_encodeVertexBuffer(reinterpret_cast<unsigned char*>(vs.data()), vs.size(), pos.data(), verts, 12));
    std::string is(meshopt_encodeIndexBufferBound(idx.size(), verts), '\0');
    is.resize(meshopt_encodeIndexBuffer(reinterpret_cast<unsigned char*>(is.data()), is.size(), idx.data(), idx.size()));

    std::string bin(4, '\0');
    auto put = [&](const std::string& d) {
        while (bin.size() % 4) bin.push_back('\0');
        const size_t off = bin.size();
        bin += d;
        return off;
    };
    const size_t vo = put(vs), io = put(is);
    const size_t vdec = size_t(verts) * 12, idec = idx.size() * 2;
    const std::string ext = "\"extensions\":{\"EXT_meshopt_compression\":";
    std::string j = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
                    "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
                    "\"extensionsUsed\":[\"EXT_meshopt_compression\"],\"extensionsRequired\":[\"EXT_meshopt_compression\"],";
    j += "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "},{\"byteLength\":" + std::to_string(vdec + idec) +
         "," + ext + "{\"fallback\":true}}}],";
    j += "\"bufferViews\":[{\"buffer\":1,\"byteOffset\":0,\"byteLength\":" + std::to_string(vdec) + "," + ext +
         "{\"buffer\":0,\"byteOffset\":" + std::to_string(vo) + ",\"byteLength\":" + std::to_string(vs.size()) +
         ",\"byteStride\":12,\"count\":" + std::to_string(verts) + ",\"mode\":\"ATTRIBUTES\",\"filter\":\"" +
         (filtered ? "EXPONENTIAL" : "NONE") + "\"}}},";
    j += "{\"buffer\":1,\"byteOffset\":" + std::to_string(vdec) + ",\"byteLength\":" + std::to_string(idec) + "," + ext +
         "{\"buffer\":0,\"byteOffset\":" + std::to_string(io) + ",\"byteLength\":" + std::to_string(is.size()) +
         ",\"byteStride\":2,\"count\":" + std::to_string(idx.size()) + ",\"mode\":\"TRIANGLES\"}}}],";
    j += "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":" + std::to_string(verts) +
         ",\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[" + std::to_string(n) + ",1," + std::to_string(n) + "]},"
         "{\"bufferView\":1,\"componentType\":5123,\"count\":" + std::to_string(idx.size()) + ",\"type\":\"SCALAR\"}]}";
    return make_glb(j, bin);
}

const char* const kObj =
    "# a tent with a material library\n"
    "mtllib tent.mtl\n"
    "o tent\n"
    "v -1 0 -1\nv 1 0 -1\nv 1 0 1\nv -1 0 1\nv 0 1.5 0\n"
    "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvt 0.5 0.5\n"
    "vn 0 -1 0\nvn 0 0.6 -0.8\nvn 0.8 0.6 0\nvn 0 0.6 0.8\nvn -0.8 0.6 0\n"
    "usemtl canvas\n"
    "f 1/1/1 4/4/1 3/3/1 2/2/1\n"
    "usemtl rope\n"
    "f 1/1/2 2/2/2 5/5/2\n"
    "f 2/2/3 3/3/3 5/5/3\n"
    "f 3/3/4 4/4/4 5/5/4\n"
    "f 4/4/5 1/1/5 5/5/5\n"
    "f -5 -4 -1\n"
    "s off\n"
    "l 1 2\n";

void fuzz_corpus(const char* label, const std::string& ext, const std::string& rel, const std::string& good, bool byte_level_only) {
    cook::ImporterRegistry reg;
    cook::ImporterRegistry::add_builtins(reg);
    log::set_level(log::Level::Off);
    const cook::IImporter* imp = reg.for_extension(StringView(ext.c_str(), ext.size()));
    REQUIRE(imp != nullptr);
    const Span<const byte> src(reinterpret_cast<const byte*>(good.data()), good.size());
    REQUIRE_MESSAGE(fuzz::import_bytes(*imp, rel, src), label << " must cook untouched");

    fuzz::Mutator m(0x6d657368u ^ static_cast<std::uint32_t>(good.size()));
    std::vector<std::uint64_t> storage;
    u64 accepted = 0;
    const u32 n = iterations();
    for (u32 it = 0; it < n; ++it) {
        usize len = m.mutate(src, storage);
        if (!byte_level_only && it % 2 == 1) {  // half of the text mutants swap two lines, which keeps each line's grammar intact
            std::vector<std::string> lines;
            for (usize pos = 0; pos < good.size();) {
                const usize e = good.find('\n', pos);
                lines.push_back(good.substr(pos, e == std::string::npos ? std::string::npos : e - pos + 1));
                pos = e == std::string::npos ? good.size() : e + 1;
            }
            std::swap(lines[m.below(lines.size())], lines[m.below(lines.size())]);
            std::string t;
            for (const std::string& l : lines) t += l;
            len = t.size();
            if (storage.size() * 8 < len) storage.resize((len + 7) / 8);
            std::memcpy(storage.data(), t.data(), len);
        }
        INFO("replay: " << label << " iteration " << it);
        accepted += fuzz::import_bytes(*imp, rel, Span<const byte>(reinterpret_cast<const byte*>(storage.data()), len));
    }
    std::printf("fuzz: %-22s %6u mutants, %6llu accepted\n", label, n, static_cast<unsigned long long>(accepted));
}

}  // namespace

TEST_CASE("fuzz: OBJ models survive mutated source") {
    fuzz_corpus("obj tent", ".obj", "models/tent.obj", kObj, false);
}

TEST_CASE("fuzz: meshopt-compressed glTF survives mutated streams and descriptions") {
    fuzz_corpus("glb meshopt", ".glb", "models/grid.glb", meshopt_glb(false), true);
    fuzz_corpus("glb meshopt filter", ".glb", "models/grid_exp.glb", meshopt_glb(true), true);
}
