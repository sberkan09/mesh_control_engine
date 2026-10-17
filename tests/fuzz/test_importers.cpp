// M-FUZZ 3: the importers. Source files are what a modder, an artist or an agent drops into a project, so every
// importer must turn a damaged one into a diagnostic, never a crash, hang or MCE_ASSERT. The corpus is the
// committed sources of the samples, projects and templates (MCE_SOURCE_DIR), a few per extension; each is mutated
// MCE_FUZZ_ITERS times from a seed that depends only on its relative path and the iteration. Native text assets are
// parsed first (a JSON error is a refusal), interchange files (png, wav, glb, ...) go in as bytes.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "mce/asset/container.h"
#include "mce/asset/file_io.h"
#include "mce/cook/importer.h"
#include "mce/log.h"
#include "json_swap.h"
#include "mutator.h"
#include "test_support.h"

using namespace mce;
namespace fs = std::filesystem;

namespace {

// No project: every reference is unknown, which is what an importer sees for a dangling one.
class EmptyLookup final : public cook::IAssetLookup {
public:
    bool type_of(cook::Guid, cook::ChunkType&) const override { return false; }
    bool guid_for_path(StringView, cook::Guid&) const override { return false; }
};

u64 fnv(const std::string& s) {
    u64 h = 1469598103934665603ull;
    for (char c : s) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    return h;
}

u32 iterations_for(usize size) {
    const char* e = std::getenv("MCE_FUZZ_ITERS");
    const u32 base = e ? static_cast<u32>(std::strtoul(e, nullptr, 10)) : 40u;
    if (size <= 64 * 1024) return base;
    if (size <= 512 * 1024) return base / 4 + 1;
    return base / 16 + 1;
}

// One mutant through scan_dependencies and cook. True when the importer accepted it.
bool import_one(const cook::IImporter& imp, const std::string& rel, Span<const byte> bytes) {
    cook::JsonDoc doc;
    cook::ImportInput in;
    const std::string disp = "assets/" + rel;
    in.display_path = StringView(disp.c_str(), disp.size());
    in.source_path = StringView(rel.c_str(), rel.size());
    in.source = bytes;
    in.guid = cook::Guid{};
    static const EmptyLookup lookup;
    in.lookup = &lookup;
    if (imp.native()) {
        cook::JsonError err;
        if (!doc.parse(StringView(reinterpret_cast<const char*>(bytes.data()), bytes.size()), err)) return false;
        in.settings = doc.root();
    }
    cook::DiagList diags;
    Array<cook::DepRef> deps;
    imp.scan_dependencies(in, deps, diags);
    if (diags.error_count() != 0) return false;
    asset::ContainerWriter w;
    return imp.cook(in, w, diags) && diags.error_count() == 0;
}

}  // namespace

TEST_CASE("fuzz: importers survive mutated source files of every committed sample") {
    const fs::path root = MCE_SOURCE_DIR;
    cook::ImporterRegistry reg;
    cook::ImporterRegistry::add_builtins(reg);
    log::set_level(log::Level::Off);

    std::map<std::string, std::vector<fs::path>> by_ext;
    for (const char* sub : {"samples", "projects", "templates", "games"}) {
        if (!fs::exists(root / sub)) continue;
        for (const auto& e : fs::recursive_directory_iterator(root / sub)) {
            if (!e.is_regular_file()) continue;
            const std::string ext = e.path().extension().string();
            if (reg.for_extension(StringView(ext.c_str(), ext.size())) == nullptr) continue;
            if (e.file_size() > 1024 * 1024) continue;
            by_ext[ext].push_back(e.path());
        }
    }
    REQUIRE_MESSAGE(!by_ext.empty(), "no importable sources under " << root);

    std::map<std::string, std::pair<u64, u64>> stats;  // extension -> mutants, accepted
    std::vector<std::uint64_t> storage;
    for (auto& [ext, files] : by_ext) {
        std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
            const auto sa = fs::file_size(a), sb = fs::file_size(b);
            return sa != sb ? sa < sb : a < b;
        });
        // Copies of the same sample (every project ships Proggy) add nothing: one file per size.
        files.erase(std::unique(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) { return fs::file_size(a) == fs::file_size(b); }), files.end());
        if (files.size() > 6) files.resize(6);
        const cook::IImporter* imp = reg.for_extension(StringView(ext.c_str(), ext.size()));
        for (const fs::path& f : files) {
            Array<byte> good;
            if (!asset::read_file(f.string(), good)) continue;
            const std::string rel = fs::relative(f, root).generic_string();
            const bool untouched = import_one(*imp, rel, good.span());
            if (ext == ".ttf") CHECK_MESSAGE(untouched, rel << " must cook untouched");
            fuzz::Mutator m(fnv(rel));
            const u32 iters = iterations_for(good.size());
            for (u32 it = 0; it < iters; ++it) {
                usize n = m.mutate(good.span(), storage);
                std::string swapped;
                if (imp->native() && it % 2 == 1) {  // half of the text mutants are valid JSON with one value swapped
                    swapped = fuzz::swap_value(m, good.span());
                    n = swapped.size();
                    if (storage.size() * 8 < n) storage.resize((n + 7) / 8);
                    std::memcpy(storage.data(), swapped.data(), n);
                }
                INFO("replay: importer " << std::string(imp->id().data(), imp->id().size()) << " file " << rel << " iteration " << it);
                const bool ok = import_one(*imp, rel, Span<const byte>(reinterpret_cast<const byte*>(storage.data()), n));
                ++stats[ext].first;
                stats[ext].second += ok;
            }
        }
    }
    u64 total = 0;
    for (const auto& [ext, s] : stats) {
        std::printf("fuzz: importers %-12s %6llu mutants, %6llu accepted\n", ext.c_str(),
                    static_cast<unsigned long long>(s.first), static_cast<unsigned long long>(s.second));
        total += s.first;
    }
    CHECK(stats.size() >= 8);
    CHECK(total > 1000);
}
