// Source registry scan (assets.md §4): sidecars, inline GUIDs of native assets, GUID uniqueness,
// unregistered sources, orphaned sidecars, external-rename auto-repair.

#include <random>

#include "mce/cook/fs.h"
#include "mce/asset/file_io.h"
#include "mce/asset/xxhash.h"
#include "mce/cook/cook.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.scan";
constexpr StringView kSidecarExt = ".mceasset";

bool ends_with(StringView s, StringView suffix) { return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix; }

// Files that live in asset trees but are never assets.
bool ignorable(StringView ext) { return ext == ".md" || ext == ".txt" || ext == ".gitkeep" || ext == ".tmp"; }

String display(StringView rel) { return path_join("assets", rel); }

u64 settings_hash(const IImporter& imp, JsonValue settings) {
    String canon;
    if (settings.valid()) write_json(settings, canon, true);
    u64 h = asset::xxh64(canon.view());
    h = hash_combine(h, fnv1a64(imp.id()));
    return hash_combine(h, imp.version());
}

void write_sidecar(StringView path, Guid g, const IImporter& imp, DiagList& diags) {
    String s("{\n  \"guid\": \"");
    s += asset::to_string(g).view();
    s += "\",\n  \"importer\": \"";
    s += imp.id();
    appendf(s, "\",\n  \"importer_version\": %u,\n  \"settings\": {}\n}\n", imp.version());
    if (!fs::write_file_atomic(path, Span<const byte>(reinterpret_cast<const byte*>(s.c_str()), s.size())))
        diags.error(Errc::IoError, path, 0, 0, kStage, "could not write sidecar");
}

struct Sidecar {
    String rel;         // sidecar path relative to assets/
    String source_rel;  // the source it describes
    Guid guid;
    JsonDoc doc;
    bool valid = false;
    bool claimed = false;
};

}  // namespace

Guid generate_guid() {
    std::random_device rd;
    auto word = [&] { return (static_cast<u64>(rd()) << 32) ^ static_cast<u64>(rd()); };
    Guid g{word(), word()};
    g.hi = (g.hi & ~0xF000ull) | 0x4000ull;                      // version 4
    g.lo = (g.lo & ~0xC000000000000000ull) | 0x8000000000000000ull;  // RFC 4122 variant
    return g;
}

bool AssetRegistry::scan(StringView project_root, const ImporterRegistry& importers, bool adopt, DiagList& diags) {
    records_.clear();
    root_ = path_join(project_root, "assets");
    const u32 errors_before = diags.error_count();
    if (!fs::is_directory(root_)) {
        diags.error(Errc::FileNotFound, root_, 0, 0, kStage, "project has no assets/ directory");
        return false;
    }
    Array<String> files;
    if (auto r = fs::list_files(root_, files); !r) {
        diags.error(r.error().code, root_, 0, 0, kStage, "cannot enumerate assets/");
        return false;
    }

    // Pass 1: sidecars.
    Array<Sidecar> sidecars;
    for (const String& f : files) {
        if (!ends_with(f, kSidecarExt)) continue;
        Sidecar sc;
        sc.rel = f;
        sc.source_rel = String(f.view().substr(0, f.size() - kSidecarExt.size()));
        const String disp = display(f);
        Array<byte> bytes;
        if (!asset::read_file(path_join(root_, f), bytes)) {
            diags.error(Errc::IoError, disp, 0, 0, kStage, "cannot read sidecar");
            sidecars.push_back(std::move(sc));
            continue;
        }
        JsonError je;
        if (!sc.doc.parse(StringView(reinterpret_cast<const char*>(bytes.data()), bytes.size()), je)) {
            diags.error(Errc::ParseError, disp, je.line, je.col, kStage, je.message.view());
        } else {
            const JsonValue g = sc.doc.root().get("guid");
            auto parsed = g.is_string() ? asset::parse_guid(g.as_string()) : Result<Guid>(Error(Errc::ParseError, kStage));
            if (!parsed) {
                diags.error(Errc::ParseError, disp, g.valid() ? g.line() : 1, g.valid() ? g.col() : 1, kStage,
                            "sidecar needs a \"guid\" in canonical UUID form");
            } else {
                sc.guid = parsed.value();
                sc.valid = true;
            }
        }
        sidecars.push_back(std::move(sc));
    }
    auto sidecar_for = [&](StringView source_rel) -> Sidecar* {
        for (Sidecar& s : sidecars)
            if (s.source_rel == source_rel) return &s;
        return nullptr;
    };
    auto is_source_present = [&](StringView rel) {
        for (const String& f : files)
            if (f == rel) return true;
        return false;
    };

    // Pass 2: sources.
    for (const String& f : files) {
        if (ends_with(f, kSidecarExt)) continue;
        const String ext = lower_extension(f);
        const String disp = display(f);
        const IImporter* imp = importers.for_extension(ext);
        if (!imp) {
            if (!ignorable(ext)) diags.warning(disp, 0, 0, kStage, "no importer for this file type; ignored");
            continue;
        }
        AssetRecord rec;
        rec.source_path = f;
        if (!asset::read_file(path_join(root_, f), rec.source)) {
            diags.error(Errc::IoError, disp, 0, 0, kStage, "cannot read source file");
            continue;
        }
        rec.source_hash = asset::xxh64(rec.source.data(), rec.source.size());

        if (imp->native()) {
            JsonError je;
            if (!rec.doc.parse(StringView(reinterpret_cast<const char*>(rec.source.data()), rec.source.size()), je)) {
                diags.error(Errc::ParseError, disp, je.line, je.col, kStage, je.message.view());
                continue;
            }
            const JsonValue g = rec.doc.root().get("guid");
            auto parsed = g.is_string() ? asset::parse_guid(g.as_string()) : Result<Guid>(Error(Errc::ParseError, kStage));
            if (!parsed) {
                diags.error(Errc::MissingSidecar, disp, g.valid() ? g.line() : 1, g.valid() ? g.col() : 1, kStage,
                            "native asset needs a top-level \"guid\" in canonical UUID form");
                continue;
            }
            rec.guid = parsed.value();
            rec.importer = imp;
            rec.settings_node = rec.doc.root().index();
            rec.settings_hash = hash_combine(hash_combine(fnv1a64(imp->id()), imp->version()), imp->schema_hash());
        } else {
            Sidecar* sc = sidecar_for(f);
            if (!sc) {
                // External rename repair: an orphaned sidecar whose last cooked source had exactly
                // these bytes moves next to the file (assets.md §4.2).
                for (Sidecar& o : sidecars) {
                    if (o.claimed || !o.valid || is_source_present(o.source_rel)) continue;
                    bool match = false;
                    for (const KnownSource& k : known_) match = match || (k.guid == o.guid && k.source_hash == rec.source_hash);
                    if (!match) continue;
                    String to(f);
                    to += kSidecarExt;
                    if (fs::rename(path_join(root_, o.rel), path_join(root_, to))) {
                        String msg("external rename repaired: sidecar moved from assets/");
                        msg += o.rel;
                        diags.add(Severity::Info, Errc::Ok, disp, 0, 0, kStage, msg);
                        o.rel = to;
                        o.source_rel = f;
                        sc = &o;
                    }
                    break;
                }
            }
            if (!sc && adopt) {
                String to(f);
                to += kSidecarExt;
                Sidecar fresh;
                fresh.rel = to;
                fresh.source_rel = f;
                fresh.guid = generate_guid();
                write_sidecar(path_join(root_, to), fresh.guid, *imp, diags);
                String text("{\"guid\":\"");
                text += asset::to_string(fresh.guid).view();
                text += "\",\"importer\":\"";
                text += imp->id();
                text += "\",\"settings\":{}}";
                JsonError je;
                fresh.valid = fresh.doc.parse(text, je);
                diags.add(Severity::Info, Errc::Ok, disp, 0, 0, kStage, "adopted: new sidecar written");
                sidecars.push_back(std::move(fresh));
                sc = &sidecars.back();
            }
            if (!sc) {
                diags.error(Errc::MissingSidecar, disp, 0, 0, kStage, "unregistered asset (no .mceasset sidecar): cook with adopt (mce_cook --adopt, or the MCP cook_run with \"adopt\": true)");
                continue;
            }
            sc->claimed = true;
            if (!sc->valid) continue;  // already reported
            const JsonValue root = sc->doc.root();
            const JsonValue importer = root.get("importer");
            const String sdisp = display(sc->rel);
            if (!importer.is_string()) {
                diags.error(Errc::ParseError, sdisp, 1, 1, kStage, "sidecar needs an \"importer\" string");
                continue;
            }
            const IImporter* chosen = importers.find(importer.as_string());
            if (!chosen || chosen->native()) {
                String msg("unknown importer '");
                msg += importer.as_string();
                msg += "'";
                diags.error(Errc::NotFound, sdisp, importer.line(), importer.col(), kStage, msg);
                continue;
            }
            const JsonValue settings = root.get("settings");
            if (settings.valid() && !settings.is_object()) {
                diags.error(Errc::ParseError, sdisp, settings.line(), settings.col(), kStage, "\"settings\" must be an object");
                continue;
            }
            rec.guid = sc->guid;
            rec.importer = chosen;
            rec.sidecar_path = sc->rel;
            rec.doc = std::move(sc->doc);
            rec.settings_node = settings.valid() ? settings.index() : AssetRecord::kNoSettings;
            rec.settings_hash = settings_hash(*chosen, rec.settings());
        }
        records_.push_back(std::move(rec));
    }

    for (const Sidecar& s : sidecars) {
        if (!s.claimed && !is_source_present(s.source_rel))
            diags.warning(display(s.rel), 0, 0, kStage, "orphaned sidecar: its source file is missing");
    }

    // One GUID, one file (FR-AST-040). Reported as a batch; the first file (path order) keeps the
    // GUID and the duplicates are left out of the cook.
    for (usize i = 0; i < records_.size(); ++i) {
        for (usize j = i + 1; j < records_.size();) {
            if (records_[i].guid != records_[j].guid) {
                ++j;
                continue;
            }
            String msg("duplicate GUID ");
            msg += asset::to_string(records_[i].guid).view();
            msg += ", also used by assets/";
            msg += records_[i].source_path;
            diags.error(Errc::DuplicateGuid, display(records_[j].source_path), 0, 0, kStage, msg);
            records_.erase(j);
        }
    }
    return diags.error_count() == errors_before;
}

const AssetRecord* AssetRegistry::by_guid(Guid g) const {
    for (const AssetRecord& r : records_)
        if (r.guid == g) return &r;
    return nullptr;
}

Result<Guid> AssetRegistry::guid_for_path(StringView rel_path) const {
    for (const AssetRecord& r : records_)
        if (r.source_path == rel_path) return r.guid;
    return Error(Errc::NotFound, "cook.registry.path", rel_path);
}

Result<void> AssetRegistry::move(Guid g, StringView new_rel_path) {
    AssetRecord* rec = nullptr;
    for (AssetRecord& r : records_)
        if (r.guid == g) rec = &r;
    if (!rec) return Error(Errc::NotFound, "cook.registry.move", asset::to_string(g).view());
    for (const AssetRecord& r : records_)
        if (r.source_path == new_rel_path) return Error(Errc::AlreadyExists, "cook.registry.move", new_rel_path);
    MCE_TRY(fs::rename(path_join(root_, rec->source_path), path_join(root_, new_rel_path)));
    if (!rec->sidecar_path.empty()) {
        String to(new_rel_path);
        to += kSidecarExt;
        MCE_TRY(fs::rename(path_join(root_, rec->sidecar_path), path_join(root_, to)));
        rec->sidecar_path = to;
    }
    rec->source_path = String(new_rel_path);
    return ok();
}

}  // namespace mce::cook
