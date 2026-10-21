#include "mce/asset/mods.h"

#include <algorithm>

#include "mce/asset/file_io.h"

namespace mce::asset {

namespace {

constexpr const char* kStage = "asset.mods";

StringView trim(StringView s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

void skip(ModList& out, StringView what, StringView reason) {
    ModSkipped s;
    s.what.assign(what);
    s.reason.assign(reason);
    out.skipped.push_back(s);
}

const ModEntry* find_id(Span<const ModEntry> mods, StringView id) {
    for (const ModEntry& m : mods)
        if (m.id.view() == id) return &m;
    return nullptr;
}

}  // namespace

bool is_valid_mod_id(StringView id) {
    if (id.empty() || id.size() > 64 || id.front() == '.') return false;
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

Result<ModEntry> read_mod_pack(StringView path) {
    Array<byte> bytes;
    MCE_TRY(read_file(path, bytes));
    MCE_TRY_ASSIGN(ContainerView view, ContainerView::open(bytes.span(), path, OpenOptions{false}));
    const u32 mi = view.find(ChunkType::Manifest);
    if (mi == view.chunk_count()) return Error(Errc::MissingRef, "asset.mods.manifest", path);
    MCE_TRY(ContainerView::check_version(view.entry(mi), MANIFEST_VERSION, path));
    MCE_TRY_ASSIGN(ManifestView manifest, ManifestView::read(view.payload(mi)));
    const u32 ii = view.find(ChunkType::ModInfo);
    if (ii == view.chunk_count()) return Error(Errc::MissingRef, "asset.mods.info", path);
    MCE_TRY(ContainerView::check_version(view.entry(ii), MOD_INFO_VERSION, path));
    MCE_TRY_ASSIGN(ModInfoView info, ModInfoView::read(view.payload(ii)));
    if (!is_valid_mod_id(info.data.id)) return Error(Errc::InvalidArgument, "asset.mods.id", path);
    ModEntry e;
    e.id.assign(info.data.id);
    e.name.assign(info.data.name.empty() ? info.data.id : info.data.name);
    e.version.assign(info.data.version);
    e.game.assign(info.data.game);
    e.path.assign(path);
    e.asset_count = manifest.size();
    if (e.path.truncated() || e.game.truncated()) return Error(Errc::InvalidArgument, kStage, path);
    return e;
}

ModList resolve_mods(Span<const StringView> packs, const StringView* load_order, StringView game_id) {
    ModList out;

    // Every readable pack for this game, by file name; the first of a duplicate id wins.
    Array<StringView> sorted;
    for (StringView p : packs) sorted.push_back(p);
    std::sort(sorted.begin(), sorted.end());
    Array<ModEntry> found;
    for (StringView p : sorted) {
        Result<ModEntry> e = read_mod_pack(p);
        if (!e) {
            FixedString<192> why("not a mod pack: ");
            why.append(errc_name(e.error().code));
            skip(out, p, why.view());
            continue;
        }
        const ModEntry& m = e.value();
        if (!game_id.empty() && !m.game.empty() && m.game.view() != game_id) {
            FixedString<192> why("made for another game: ");
            why.append(m.game.view());
            skip(out, p, why.view());
            continue;
        }
        if (const ModEntry* dup = find_id(found.span(), m.id.view())) {
            FixedString<192> why("duplicate mod id '");
            why.append(m.id.view());
            why.append("' (already in ");
            why.append(dup->path.view());
            why.append(")");
            skip(out, p, why.view());
            continue;
        }
        found.push_back(m);
    }

    auto add = [&](const ModEntry& m) {
        if (out.mods.size() >= kMaxMods) {
            skip(out, m.path.view(), "too many mods");
            return;
        }
        out.mods.push_back(m);
    };

    if (!load_order) {
        for (const ModEntry& m : found) add(m);
        return out;
    }

    StringView text = *load_order;
    while (!text.empty()) {
        const usize nl = text.find('\n');
        StringView line = nl == StringView::npos ? text : text.substr(0, nl);
        text = nl == StringView::npos ? StringView() : text.substr(nl + 1);
        if (const usize hash = line.find('#'); hash != StringView::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;
        if (find_id(out.mods.span(), line)) {
            skip(out, line, "listed twice in load_order.txt");
            continue;
        }
        const ModEntry* m = find_id(found.span(), line);
        if (!m) {
            skip(out, line, "in load_order.txt, but no pack in the mods folder has this id");
            continue;
        }
        add(*m);
    }
    for (const ModEntry& m : found)
        if (!find_id(out.mods.span(), m.id.view())) skip(out, m.path.view(), "not listed in load_order.txt");
    return out;
}

}  // namespace mce::asset
