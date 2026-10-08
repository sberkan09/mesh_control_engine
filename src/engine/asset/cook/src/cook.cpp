// CookService::cook — scan, graph build, topological order, stage execution on the job system,
// content-addressed outputs, manifest + DB commit, pack (assets.md §5.1).
//
// Determinism (FR-AST-030) rests on: pure stages; stages writing disjoint outputs (parallelism
// changes wall time, never bytes); manifest and pack sorted by GUID; diagnostics merged in
// topological order; no clock, path or environment in any output.

#include <algorithm>
#include <cstdio>

#include "cook_db.h"
#include "mce/cook/fs.h"
#include "mce/asset/file_io.h"
#include "mce/asset/formats.h"
#include "mce/asset/xxhash.h"
#include "mce/cook/cook.h"
#include "mce/jobs.h"

namespace mce::cook {

namespace {

constexpr const char* kStageGraph = "cook.graph";
constexpr const char* kStageWrite = "cook.write";

String display(const AssetRecord& r) { return path_join("assets", r.source_path); }

// cooked/<aa>/<guid-hex>/<content-hash>.mce (assets.md §3.1)
String output_rel_path(Guid g, u64 hash) {
    const FixedString<40> hex = asset::to_hex(g);
    String s;
    appendf(s, "%.2s/%s/%016llx.mce", hex.c_str(), hex.c_str(), static_cast<unsigned long long>(hash));
    return s;
}

class RegistryLookup final : public IAssetLookup {
public:
    explicit RegistryLookup(const AssetRegistry& r) : reg_(r) {}
    bool type_of(Guid g, ChunkType& out) const override {
        const AssetRecord* rec = reg_.by_guid(g);
        if (!rec) return false;
        out = rec->importer->type();
        return true;
    }
    bool guid_for_path(StringView rel_path, Guid& out) const override {
        auto g = reg_.guid_for_path(rel_path);
        if (!g) return false;
        out = g.value();
        return true;
    }
    bool native_document(Guid g, JsonValue& out, StringView& source_path) const override {
        const AssetRecord* rec = reg_.by_guid(g);
        if (!rec || !rec->importer->native()) return false;
        out = rec->settings();
        source_path = rec->source_path.view();
        return out.valid();
    }

private:
    const AssetRegistry& reg_;
};

struct Node {
    AssetRecord* rec = nullptr;
    Array<u32> deps;        // node indices this asset depends on (deduplicated, sorted)
    Array<u32> dependents;  // reverse edges
    bool bad_refs = false;  // a missing/mistyped reference was reported
    bool emitted = false;
    u32 level = 0;
    AssetStatus status = AssetStatus::Failed;
    u64 output_hash = 0;
    Array<byte> output;     // freshly cooked container bytes
    DiagList diags;         // stage-local, merged in order
};

struct StageJob {
    Node* node;
    const AssetRegistry* registry;
    const IAssetLookup* lookup;
    const CookDb* db;
    const Array<Node>* nodes;
    String cooked_root;
    bool force;
};

ImportInput input_for(const AssetRecord& rec, const String& disp, const IAssetLookup* lookup) {
    ImportInput in;
    in.guid = rec.guid;
    in.display_path = disp;
    in.source_path = rec.source_path;
    in.source = rec.source.span();
    in.settings = rec.settings();
    in.lookup = lookup;
    return in;
}

void run_stage(void* ctx) {
    StageJob& job = *static_cast<StageJob*>(ctx);
    Node& n = *job.node;
    const AssetRecord& rec = *n.rec;
    const DbRecord* prev = job.db->find(rec.guid);

    // Cache check: source, settings, importer, tool version and every dependency's output hash.
    if (!job.force && prev && prev->source_hash == rec.source_hash && prev->settings_hash == rec.settings_hash &&
        prev->importer_id == fnv1a64(rec.importer->id()) && prev->importer_version == rec.importer->version() &&
        prev->tool_version == kCookToolVersion && prev->deps.size() == n.deps.size() &&
        fs::exists(path_join(job.cooked_root, output_rel_path(rec.guid, prev->output_hash)))) {
        bool deps_same = true;
        for (usize i = 0; i < n.deps.size() && deps_same; ++i) {
            const Node& d = (*job.nodes)[n.deps[i]];
            deps_same = prev->deps[i].guid == d.rec->guid && prev->deps[i].output_hash == d.output_hash;
        }
        if (deps_same) {
            n.status = AssetStatus::UpToDate;
            n.output_hash = prev->output_hash;
            return;
        }
    }

    const String disp = display(rec);
    asset::ContainerWriter writer;
    if (!rec.importer->cook(input_for(rec, disp, job.lookup), writer, n.diags)) {
        if (n.diags.error_count() == 0) n.diags.error(Errc::ImportFailed, disp, 0, 0, "cook.stage", "importer failed");
        n.status = AssetStatus::Failed;
        return;
    }
    writer.finish(n.output);
    n.output_hash = asset::xxh64(n.output.data(), n.output.size());
    n.status = AssetStatus::Cooked;
}

// Reports one cycle through `start` among not-yet-emitted nodes.
void report_cycle(Array<Node>& nodes, u32 start, Array<u8>& reported, DiagList& diags) {
    Array<u32> path;
    Array<u8> on_path;
    on_path.resize(nodes.size(), 0);
    u32 cur = start;
    while (!on_path[cur]) {
        on_path[cur] = 1;
        path.push_back(cur);
        u32 next = cur;
        for (u32 d : nodes[cur].deps)
            if (!nodes[d].emitted) {
                next = d;
                break;
            }
        if (next == cur) return;
        cur = next;
    }
    usize first = path.find(cur);
    bool already = true;
    for (usize i = first; i < path.size(); ++i) already = already && reported[path[i]];
    if (already) return;
    String msg("dependency cycle: ");
    for (usize i = first; i < path.size(); ++i) {
        reported[path[i]] = 1;
        msg += "assets/";
        msg += nodes[path[i]].rec->source_path;
        msg += " -> ";
    }
    msg += "assets/";
    msg += nodes[cur].rec->source_path;
    diags.error(Errc::DependencyCycle, display(*nodes[cur].rec), 0, 0, kStageGraph, msg);
}

}  // namespace

CookService::CookService() { ImporterRegistry::add_builtins(importers_); }
CookService::~CookService() = default;

CookReport CookService::cook(StringView project_root, const CookOptions& opt) {
    CookReport report;
    const String cooked_root = path_join(project_root, "cooked");
    const String db_path = path_join(cooked_root, ".mcedb");

    CookDb db;
    db.load(db_path);
    Array<AssetRegistry::KnownSource> known;
    for (const DbRecord& r : db.records()) known.push_back(AssetRegistry::KnownSource{r.guid, r.source_hash});

    // 1. Scan.
    AssetRegistry registry;
    registry.set_known_sources(known.span());
    registry.scan(project_root, importers_, opt.adopt, report.diags);
    RegistryLookup lookup(registry);

    // 2. Graph build: importers declare references; missing and mistyped refs are reported as a
    //    batch with file:line:col.
    Array<Node> nodes;
    nodes.resize(registry.size());
    HashMap<Guid, u32> node_of;
    for (u32 i = 0; i < registry.size(); ++i) {
        nodes[i].rec = &registry.at(i);
        node_of.insert_or_assign(registry.at(i).guid, i);
    }
    for (u32 i = 0; i < nodes.size(); ++i) {
        AssetRecord& rec = *nodes[i].rec;
        const String disp = display(rec);
        rec.importer->scan_dependencies(input_for(rec, disp, &lookup), rec.deps, report.diags);
        for (const DepRef& d : rec.deps) {
            const u32* target = node_of.find(d.guid);
            if (!target) {
                String msg("missing reference ");
                msg += asset::to_string(d.guid).view();
                if (!d.where.empty()) {
                    msg += " (";
                    msg += d.where.view();
                    msg += ')';
                }
                report.diags.error(Errc::MissingRef, disp, d.line, d.col, kStageGraph, msg);
                nodes[i].bad_refs = true;
                continue;
            }
            const ChunkType actual = nodes[*target].rec->importer->type();
            if (d.expected != ChunkType::Manifest && actual != d.expected) {
                String msg;
                appendf(msg, "reference %s is a %s asset, expected %s", asset::to_string(d.guid).c_str(), asset::chunk_type_name(actual),
                        asset::chunk_type_name(d.expected));
                if (!d.where.empty()) {
                    msg += " (";
                    msg += d.where.view();
                    msg += ')';
                }
                report.diags.error(Errc::MissingRef, disp, d.line, d.col, kStageGraph, msg);
                nodes[i].bad_refs = true;
                continue;
            }
            if (*target != i && !nodes[i].deps.contains(*target)) nodes[i].deps.push_back(*target);
        }
        std::sort(nodes[i].deps.begin(), nodes[i].deps.end(),
                  [&](u32 a, u32 b) { return nodes[a].rec->guid < nodes[b].rec->guid; });
        for (u32 d : nodes[i].deps) nodes[d].dependents.push_back(i);
    }

    // 3. Topological order (Kahn), ties broken by GUID so the order is reproducible.
    Array<u32> indegree;
    indegree.resize(nodes.size(), 0);
    for (u32 i = 0; i < nodes.size(); ++i) indegree[i] = static_cast<u32>(nodes[i].deps.size());
    Array<u32> ready, order;
    for (u32 i = 0; i < nodes.size(); ++i)
        if (indegree[i] == 0) ready.push_back(i);
    auto by_guid_desc = [&](u32 a, u32 b) { return nodes[b].rec->guid < nodes[a].rec->guid; };
    while (!ready.empty()) {
        std::sort(ready.begin(), ready.end(), by_guid_desc);  // smallest GUID last
        const u32 n = ready.back();
        ready.pop_back();
        nodes[n].emitted = true;
        order.push_back(n);
        for (u32 d : nodes[n].deps) nodes[n].level = std::max(nodes[n].level, nodes[d].level + 1);
        for (u32 dep : nodes[n].dependents)
            if (--indegree[dep] == 0) ready.push_back(dep);
    }
    {
        Array<u8> reported;
        reported.resize(nodes.size(), 0);
        for (u32 i = 0; i < nodes.size(); ++i)
            if (!nodes[i].emitted) report_cycle(nodes, i, reported, report.diags);
    }

    // 4. Stage execution, level by level on the job system (a level's stages only read the
    //    output hashes of earlier levels; each writes only its own Node).
    u32 max_level = 0;
    for (u32 n : order) max_level = std::max(max_level, nodes[n].level);
    Array<StageJob> jobs_storage;
    jobs_storage.reserve(order.size());
    for (u32 level = 0; level <= max_level && !order.empty(); ++level) {
        jobs::Counter counter;
        const usize first_job = jobs_storage.size();
        for (u32 n : order) {
            Node& node = nodes[n];
            if (node.level != level) continue;
            bool blocked = node.bad_refs;
            for (u32 d : node.deps) blocked = blocked || nodes[d].status == AssetStatus::Failed || nodes[d].status == AssetStatus::Blocked;
            if (blocked) {
                node.status = AssetStatus::Blocked;
                continue;
            }
            jobs_storage.push_back(StageJob{&node, &registry, &lookup, &db, &nodes, cooked_root, opt.force});
        }
        for (usize j = first_job; j < jobs_storage.size(); ++j) jobs::run(&run_stage, &jobs_storage[j], &counter);
        jobs::wait_for(&counter);
    }
    for (u32 i = 0; i < nodes.size(); ++i)
        if (!nodes[i].emitted) nodes[i].status = AssetStatus::Blocked;

    // 5. Commit: content-addressed outputs (new file, never rename-over), manifest, DB.
    Array<DbRecord> fresh;
    Array<asset::ManifestItem> items;
    Array<u32> report_order = order;
    for (u32 i = 0; i < nodes.size(); ++i)
        if (!nodes[i].emitted) report_order.push_back(i);
    for (u32 n : report_order) {
        Node& node = nodes[n];
        const AssetRecord& rec = *node.rec;
        report.diags.append(node.diags);
        AssetReport ar;
        ar.guid = rec.guid;
        ar.path = rec.source_path;
        ar.type = rec.importer->type();
        ar.status = node.status;

        u64 manifest_hash = 0;
        if (node.status == AssetStatus::Cooked) {
            const String out_path = path_join(cooked_root, output_rel_path(rec.guid, node.output_hash));
            if (!fs::exists(out_path)) {
                if (auto w = fs::write_file_atomic(out_path, node.output.span()); !w) {
                    report.diags.error(w.error().code, out_path, 0, 0, kStageWrite, "cannot write cooked output");
                    node.status = ar.status = AssetStatus::Failed;
                }
            }
            if (node.status == AssetStatus::Cooked) {
                manifest_hash = node.output_hash;
                DbRecord r;
                r.guid = rec.guid;
                r.source_hash = rec.source_hash;
                r.settings_hash = rec.settings_hash;
                r.importer_id = fnv1a64(rec.importer->id());
                r.importer_version = rec.importer->version();
                r.tool_version = kCookToolVersion;
                r.output_hash = node.output_hash;
                r.type = rec.importer->type();
                r.source_path = rec.source_path;
                for (u32 d : node.deps) r.deps.push_back(DbDep{nodes[d].rec->guid, nodes[d].output_hash});
                fresh.push_back(std::move(r));
                ++report.cooked;
            }
        } else if (node.status == AssetStatus::UpToDate) {
            manifest_hash = node.output_hash;
            ++report.up_to_date;
        } else {
            ++report.failed;
            // Old assets keep serving: the last good output stays in the manifest (assets.md §7.5).
            const DbRecord* prev = db.find(rec.guid);
            if (prev && fs::exists(path_join(cooked_root, output_rel_path(rec.guid, prev->output_hash)))) manifest_hash = prev->output_hash;
        }
        ar.output_hash = manifest_hash;
        if (manifest_hash != 0) items.push_back(asset::ManifestItem{rec.guid, manifest_hash, rec.importer->type(), rec.source_path});
        report.assets.push_back(std::move(ar));
    }

    if (!fs::exists(path_join(project_root, "assets"))) return report;  // nothing to commit

    Array<byte> manifest_payload;
    asset::write_manifest(items.span(), manifest_payload);
    asset::ContainerWriter mw;
    asset::ContainerWriter::ChunkDesc md;
    md.type = ChunkType::Manifest;
    md.version = asset::chunk_version(asset::MANIFEST_VERSION, 0);
    mw.add(md, manifest_payload.span());
    Array<byte> manifest_file;
    mw.finish(manifest_file);
    report.manifest_hash = asset::xxh64(manifest_file.data(), manifest_file.size());
    const String manifest_path = path_join(cooked_root, "manifest.mce");
    if (auto w = fs::write_file_atomic(manifest_path, manifest_file.span()); !w)
        report.diags.error(w.error().code, manifest_path, 0, 0, kStageWrite, "cannot write manifest");
    if (!db.commit(db_path, fresh.span())) report.diags.warning(db_path, 0, 0, kStageWrite, "cannot write cook DB (next cook re-cooks)");

    // 6. Pack (FR-SHP-010): manifest chunk first, then every live asset's chunks in GUID order.
    if (opt.pack) {
        if (!report.ok()) {
            report.diags.error(Errc::ImportFailed, path_join(project_root, "assets.mce"), 0, 0, "cook.pack", "pack not written: the cook has errors");
            return report;
        }
        asset::ContainerWriter pw;
        pw.add(md, manifest_payload.span());
        std::sort(items.begin(), items.end(), [](const asset::ManifestItem& a, const asset::ManifestItem& b) { return a.guid < b.guid; });
        for (const asset::ManifestItem& it : items) {
            const String path = path_join(cooked_root, output_rel_path(it.guid, it.content_hash));
            Array<byte> bytes;
            auto view = asset::read_file(path, bytes) ? asset::ContainerView::open(bytes.span(), path)
                                                      : Result<asset::ContainerView>(Error(Errc::FileNotFound, "cook.pack", path));
            if (!view) {
                report.diags.error(view.error().code, path, 0, 0, "cook.pack", "cannot read cooked output for the pack");
                return report;
            }
            for (u32 c = 0; c < view.value().chunk_count(); ++c) {
                const asset::TocEntry& e = view.value().entry(c);
                asset::ContainerWriter::ChunkDesc d;
                d.type = e.type;
                d.version = e.chunk_version;
                d.flags = e.flags;
                d.user_flags = e.user_flags;
                d.guid = e.guid();
                pw.add(d, view.value().payload(c));
            }
        }
        Array<byte> pack;
        pw.finish(pack);
        report.pack_hash = asset::xxh64(pack.data(), pack.size());
        const String pack_path = path_join(project_root, "assets.mce");
        if (auto w = fs::write_file_atomic(pack_path, pack.span()); !w)
            report.diags.error(w.error().code, pack_path, 0, 0, "cook.pack", "cannot write pack");
    }
    return report;
}

bool cook_document(const AssetRegistry& registry, const IImporter& importer, Guid guid, StringView source_rel_path,
                   StringView text, Array<byte>& out, DiagList& diags) {
    const String disp = path_join("assets", source_rel_path);
    JsonDoc doc;
    JsonError je;
    if (!doc.parse(text, je)) {
        diags.error(Errc::ParseError, disp, je.line, je.col, "cook.document", je.message.view());
        return false;
    }
    const RegistryLookup lookup(registry);
    ImportInput in;
    in.guid = guid;
    in.display_path = disp;
    in.source_path = source_rel_path;
    in.source = Span<const byte>(reinterpret_cast<const byte*>(text.data()), text.size());
    in.settings = doc.root();
    in.lookup = &lookup;
    // The graph build's reference checks: a missing or mistyped asset blocks the document.
    Array<DepRef> deps;
    const u32 errors_before = diags.error_count();
    importer.scan_dependencies(in, deps, diags);
    for (const DepRef& d : deps) {
        const AssetRecord* target = registry.by_guid(d.guid);
        String msg;
        if (!target)
            appendf(msg, "missing reference %s", asset::to_string(d.guid).c_str());
        else if (d.expected != ChunkType::Manifest && target->importer->type() != d.expected)
            appendf(msg, "reference %s is a %s asset, expected %s", asset::to_string(d.guid).c_str(),
                    asset::chunk_type_name(target->importer->type()), asset::chunk_type_name(d.expected));
        else
            continue;
        if (!d.where.empty()) {
            msg += " (";
            msg += d.where.view();
            msg += ')';
        }
        diags.error(Errc::MissingRef, disp, d.line, d.col, kStageGraph, msg);
    }
    if (diags.error_count() != errors_before) return false;
    asset::ContainerWriter writer;
    if (!importer.cook(in, writer, diags)) {
        if (diags.error_count() == 0) diags.error(Errc::ImportFailed, disp, 0, 0, "cook.document", "importer failed");
        return false;
    }
    writer.finish(out);
    return true;
}

}  // namespace mce::cook
