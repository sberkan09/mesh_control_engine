// Behavior tree tools (M-AI step 6): bt_create, bt_add_node, bt_set, bt_trace. An AI is a .mcbt run
// by an AiBrain component (component_add it with the tree's GUID); bt_trace plays the scene headless
// and reports what the brain did.
//
// Every edit goes through the cook, as the Behavior Tree panel's do: the tree is edited as a
// cook::BtDocument (bt_text.h), cooked with cook::build_behavior_tree (so a bad edit fails with the
// cook's file:line message and nothing is written) and saved in the canonical form, which reads back
// to the same document. Nodes are named by their pre-order index, the order the cook writes them and
// the outline every tool returns lists them in.

#include "mce/cook/bt_text.h"
#include "mce/cook/cook.h"
#include "mce/cook/fs.h"
#include "mce/kit/ai.h"
#include "mce/mcp/headless_run.h"
#include "tool_args.h"
#include "tools.h"

namespace mce::mcp {

namespace {

constexpr usize kMaxDiagnostics = 10;
constexpr u64 kMaxTicks = 36000;
constexpr usize kMaxRows = 400;

bool is_mcbt(StringView rel) { return rel.size() > 5 && rel.substr(rel.size() - 5) == ".mcbt"; }

// Reads `text` as a .mcbt document; false with the cook's diagnostics in c.error.
bool read_doc(ToolCall& c, StringView rel, StringView text, cook::BtDocument& out, const char* what) {
    cook::JsonDoc doc;
    cook::JsonError err;
    if (!doc.parse(text, err) || !doc.root().is_object()) {
        c.error += what;
        c.error += ": not a JSON object";
        return false;
    }
    cook::DiagList diags;
    if (cook::read_behavior_tree_document(doc.root(), rel, out, diags)) return true;
    c.error += what;
    c.error += ":";
    append_diags(c.error, diags, kMaxDiagnostics);
    return false;
}

// The tree named by 'tree' (a path under assets/ or "guid:<uuid>"), read.
bool arg_tree(ToolCall& c, const editor::ProjectAsset*& out, cook::BtDocument& doc) {
    if (!need_project(c)) return false;
    const cook::JsonValue d = arg(c, "tree");
    if (!d.is_string()) return arg_missing(c, "tree", "a .mcbt path under assets/ or \"guid:<uuid>\"");
    const StringView ref = d.as_string();
    const editor::EditorProject& project = c.server.session().project();
    out = nullptr;
    if (ref.size() > 5 && ref.substr(0, 5) == "guid:") {
        const Result<asset::Guid> g = asset::parse_guid(ref.substr(5));
        if (g) out = project.find(g.value());
    } else {
        for (const editor::ProjectAsset& a : project.assets())
            if (a.path.view() == ref) out = &a;
    }
    if (!out) return fail(c, "argument 'tree': no such asset (asset_list type BehaviorTree lists them)");
    if (out->type != asset::ChunkType::BehaviorTree) return fail(c, "argument 'tree' is not a behavior tree (.mcbt)");
    cook::String text;
    if (!cook::fs::read_text(project.asset_path(out->path.view()).view(), text)) return fail(c, "the tree could not be read");
    return read_doc(c, out->path.view(), text.view(), doc, "the tree on disk does not read");
}

// One outline line: "<index> <indent><kind> <value>  <name>".
void outline_line(const cook::BtDocument& d, u32 cooked, u32 node, u32 depth, cook::String& out) {
    const cook::BtDocNode& n = d.nodes[node];
    cook::appendf(out, "%u ", cooked);
    for (u32 i = 0; i < depth; ++i) out += "  ";
    out += n.kind.view();
    if (!n.value.empty()) {
        out += " ";
        out += n.value.view();
    }
    if (const StringView name = n.field("name"); !name.empty()) {
        out += "  ";
        out += name;
    }
}

void depths(const cook::BtDocument& d, u32 node, u32 depth, Array<u32>& out) {
    if (node >= d.nodes.size()) return;
    out[node] = depth;
    for (u32 c : d.nodes[node].children) depths(d, c, depth + 1, out);
}

// The tree as the tools return it: its keys and its nodes in pre-order (the indices the tools take).
void write_tree(ToolCall& c, StringView rel, asset::Guid guid, const cook::BtDocument& d) {
    c.out.key("tree").str(rel).key("guid").str(asset_guid_ref(guid).view());
    c.out.key("keys").begin_array();
    for (const cook::BtDocKey& k : d.keys) {
        cook::String s(k.name.view());
        s += ": ";
        s += k.type.view();
        if (!k.def.empty()) {
            s += " = ";
            s += k.def.view();
        }
        c.out.str(s.view());
    }
    c.out.end_array();
    Array<u32> order, depth;
    d.preorder(order);
    depth.resize(d.nodes.size(), 0);
    if (!d.nodes.empty()) depths(d, 0, 0, depth);
    c.out.key("nodes").begin_array();
    for (u32 i = 0; i < order.size(); ++i) {
        cook::String line;
        outline_line(d, i, order[i], depth[order[i]], line);
        c.out.str(line.view());
    }
    c.out.end_array();
}

// Cooks `d` and writes it canonically; false (nothing written) with the cook's errors otherwise.
bool save_tree(ToolCall& c, StringView rel, asset::Guid guid, const cook::BtDocument& d, const char* what) {
    const cook::String kept(rel);
    cook::String text;
    cook::write_behavior_tree_document(d, guid, text);
    cook::JsonDoc json;
    cook::JsonError err;
    if (!json.parse(text.view(), err)) return fail(c, "internal: the written tree is not JSON");
    cook::DiagList diags;
    Array<byte> payload;
    if (!cook::build_behavior_tree(json.root(), rel, payload, diags)) {
        c.error += what;
        c.error += " (nothing was written):";
        append_diags(c.error, diags, kMaxDiagnostics);
        return false;
    }
    cook::BtDocument back;
    if (!read_doc(c, rel, text.view(), back, "internal: the written tree does not read")) return false;
    if (!asset_validate_and_write(c, rel, guid, text.view())) return false;
    // The write rescans the project, which frees the asset path `rel` may point into.
    c.out.begin_object();
    write_tree(c, kept.view(), guid, back);
    c.out.end_object();
    return true;
}

// `v` (a node object, children included) parsed into document nodes; out.nodes[0] is its root.
bool parse_subtree(ToolCall& c, cook::JsonValue v, cook::BtDocument& out, const char* name) {
    cook::String text("{\"format\":\"mce-bt\",\"version\":1,\"root\":");
    cook::write_json(v, text, false);
    text += '}';
    cook::String what("argument '");
    what += name;
    what += "'";
    return read_doc(c, "(argument)", text.view(), out, what.c_str());
}

// Appends `from`'s nodes to `to`; returns the new index of from's root.
u32 splice(const cook::BtDocument& from, cook::BtDocument& to) {
    const u32 base = static_cast<u32>(to.nodes.size());
    for (const cook::BtDocNode& n : from.nodes) {
        cook::BtDocNode m = n;
        for (u32& k : m.children) k += base;
        to.nodes.push_back(m);
    }
    return base;
}

// The document node at pre-order index `index` (argument `name`).
bool arg_node(ToolCall& c, const cook::BtDocument& d, const char* name, bool required, u32 def, u32& out) {
    Array<u32> order;
    d.preorder(order);
    u64 i = def;
    if (!arg_uint(c, name, i, 0, order.empty() ? 0 : order.size() - 1, required)) return false;
    if (order.empty()) return fail(c, "the tree has no nodes");
    out = order[static_cast<usize>(i)];
    return true;
}

// A blackboard argument: {"name": "type" | {"type", "default"} | null}.
bool apply_blackboard(ToolCall& c, cook::JsonValue bb, cook::BtDocument& d) {
    if (!bb.valid()) return true;
    if (!bb.is_object()) return arg_missing(c, "blackboard", "an object: key name -> \"type\", {\"type\", \"default\"} or null (removes it)");
    for (u32 i = 0; i < bb.size(); ++i) {
        const StringView name = bb.key_at(i);
        const cook::JsonValue v = bb.at(i);
        const u32 at = d.find_key(name);
        if (v.is_null()) {
            if (at == cook::kBtNoNode) return fail(c, "argument 'blackboard': removing a key the tree does not have");
            d.keys.erase(at);
            continue;
        }
        cook::BtDocKey k;
        k.name.assign(name);
        cook::JsonValue type = v;
        if (v.is_object()) {
            type = v.get("type");
            if (const cook::JsonValue def = v.get("default"); def.valid()) cook::write_inline_json(def, k.def);
        }
        if (!type.is_string()) return arg_missing(c, "blackboard values", "\"bool\", \"int\", \"float\", \"vec3\" or \"entity\", or {\"type\", \"default\"}");
        k.type.assign(type.as_string());
        if (at == cook::kBtNoNode) d.keys.push_back(k);
        else d.keys[at] = k;
    }
    return true;
}

bool bt_create(ToolCall& c) {
    StringView rel;
    if (!need_project(c) || !arg_string(c, "path", rel, true)) return false;
    if (!asset_rel_valid(rel) || !is_mcbt(rel))
        return fail(c, "argument 'path' must be a .mcbt path under assets/ like \"ai/guard.mcbt\" (forward slashes, no '..')");
    editor::EditorProject& project = c.server.session().project();
    if (cook::fs::exists(project.asset_path(rel).view())) return fail(c, "a file already exists there: bt_add_node and bt_set change it");
    StringView name;
    if (!arg_string(c, "name", name, false)) return false;
    cook::BtDocument d;
    d.name.assign(name);
    if (!apply_blackboard(c, arg(c, "blackboard"), d)) return false;
    const cook::JsonValue root = arg(c, "root");
    if (root.valid()) {
        cook::BtDocument sub;
        if (!parse_subtree(c, root, sub, "root")) return false;
        splice(sub, d);
    } else {
        cook::BtDocNode sel, wait;
        sel.kind.assign("selector");
        sel.children.push_back(1);
        wait.kind.assign("wait");
        wait.value.assign("1");
        d.nodes.push_back(sel);
        d.nodes.push_back(wait);
    }
    return save_tree(c, rel, cook::generate_guid(), d, "the tree is not valid");
}

bool bt_add_node(ToolCall& c) {
    const editor::ProjectAsset* a = nullptr;
    cook::BtDocument d;
    if (!arg_tree(c, a, d)) return false;
    u32 parent = 0;
    if (!arg_node(c, d, "parent", false, 0, parent)) return false;
    const cook::JsonValue node = arg(c, "node");
    if (!node.is_object()) return arg_missing(c, "node", "a .mcbt node object, e.g. {\"task\": \"move_to\", \"key\": \"home\"} (children included)");
    const cook::BtArity arity = cook::bt_kind_arity(d.nodes[parent].kind.view());
    if (arity == cook::BtArity::Leaf) return fail(c, "argument 'parent' is a leaf: a node goes under a composite or a decorator");
    if (arity == cook::BtArity::One && !d.nodes[parent].children.empty())
        return fail(c, "argument 'parent' is a decorator that has its child: bt_set the child, or add under a composite");
    cook::BtDocument sub;
    if (!parse_subtree(c, node, sub, "node")) return false;
    const u32 at = splice(sub, d);
    Array<u32>& kids = d.nodes[parent].children;
    u64 index = kids.size();
    if (arg(c, "index").valid() && !arg_uint(c, "index", index, 0, kids.size(), false)) return false;
    kids.insert(static_cast<usize>(index), at);
    return save_tree(c, a->path.view(), a->guid, d, "the edit does not make a valid tree");
}

bool bt_set(ToolCall& c) {
    const editor::ProjectAsset* a = nullptr;
    cook::BtDocument d;
    if (!arg_tree(c, a, d)) return false;
    StringView name;
    if (!arg_string(c, "name", name, false)) return false;
    if (arg(c, "name").valid()) d.name.assign(name);
    if (!apply_blackboard(c, arg(c, "blackboard"), d)) return false;
    const cook::JsonValue value = arg(c, "value"), fields = arg(c, "fields"), replace = arg(c, "replace");
    bool remove = false;
    if (!arg_bool(c, "remove", remove)) return false;
    if (value.valid() || fields.valid() || replace.valid() || remove) {
        u32 n = 0;
        if (!arg_node(c, d, "node", true, 0, n)) return false;
        if (remove) {
            const u32 parent = d.parent(n);
            if (parent == cook::kBtNoNode) return fail(c, "the root is not removed: 'replace' it instead");
            Array<u32>& kids = d.nodes[parent].children;
            for (usize i = 0; i < kids.size(); ++i)
                if (kids[i] == n) {
                    kids.erase(i);
                    break;
                }
        } else if (replace.valid()) {
            if (!replace.is_object()) return arg_missing(c, "replace", "a .mcbt node object (children included)");
            cook::BtDocument sub;
            if (!parse_subtree(c, replace, sub, "replace")) return false;
            const u32 parent = d.parent(n);
            if (parent == cook::kBtNoNode) {
                d.nodes = sub.nodes;  // the root: the new node is the tree
            } else {
                const u32 at = splice(sub, d);
                for (u32& k : d.nodes[parent].children)
                    if (k == n) k = at;
            }
        } else {
            cook::BtDocNode& node = d.nodes[n];
            if (value.valid()) {
                if (cook::bt_kind_arity(node.kind.view()) != cook::BtArity::Leaf)
                    return fail(c, "argument 'value': only a leaf (check, wait, set, emit, task) has a value; its children are bt_add_node's");
                node.value = cook::String();
                cook::write_inline_json(value, node.value);
            }
            if (fields.valid()) {
                if (!fields.is_object()) return arg_missing(c, "fields", "an object: member name -> its JSON value, or null to remove it");
                for (u32 i = 0; i < fields.size(); ++i) {
                    const StringView f = fields.key_at(i);
                    if (f == node.kind.view() || cook::bt_is_kind(f)) return fail(c, "argument 'fields': a node's kind is not a field ('replace' the node)");
                    cook::String json;
                    if (!fields.at(i).is_null()) cook::write_inline_json(fields.at(i), json);
                    node.set_field(f, json.view());
                }
            }
        }
    }
    return save_tree(c, a->path.view(), a->guid, d, "the edit does not make a valid tree");
}

// ---------------------------------------------------------------- bt_trace

struct TraceRun {
    usize index = 0;  // the entity in the scene's file order
    Entity brain;
    u64 every = 30;
    const cook::BtDocument* doc = nullptr;
    Array<u32> order;  // doc pre-order
    struct Row {
        u64 tick = 0;
        kit::BtStatus result = kit::BtStatus::Idle;
        u32 runs = 0;
        cook::String path;  // the running nodes, "0 selector > 3 guard \"chase\" > 5 task \"move_to\""
        cook::String keys;  // "alert=true target=#4 ..."
    };
    Array<Row> rows;
    Array<cook::String> changes;  // "tick 120: 3 guard \"chase\" running"
    Array<u8> last;               // per cooked node: the status last tick
    bool lost = false;
};

void trace_sample(World& w, TraceRun& t, u64 tick, bool row) {
    const kit::AiBrain* b = w.try_get<kit::AiBrain>(t.brain);
    kit::BtRegistry* reg = w.try_resource<kit::BtRegistry>();
    const kit::BtRegistry::Def* def = b && reg ? reg->resolve(*b) : nullptr;
    if (!def || def->nodes.size() != t.order.size()) {
        t.lost = true;
        return;
    }
    if (t.last.empty()) t.last.resize(t.order.size(), 0);
    for (u32 i = 0; i < t.order.size(); ++i) {
        const kit::BtStatus s = kit::bt_node_status(*b, i);
        if (static_cast<u8>(s) == t.last[i]) continue;
        t.last[i] = static_cast<u8>(s);
        if (s != kit::BtStatus::Running || t.changes.size() >= kMaxRows) continue;
        // Only a node starting to run is a change worth reading; the rest is in the rows.
        cook::String line;
        cook::appendf(line, "tick %llu: ", static_cast<unsigned long long>(tick));
        outline_line(*t.doc, i, t.order[i], 0, line);
        t.changes.push_back(line);
    }
    if (!row || t.rows.size() >= kMaxRows) return;
    TraceRun::Row r;
    r.tick = tick;
    r.result = b->result;
    r.runs = b->runs;
    for (u32 i = 0; i < t.order.size(); ++i) {
        if (kit::bt_node_status(*b, i) != kit::BtStatus::Running) continue;
        if (!r.path.empty()) r.path += " > ";
        outline_line(*t.doc, i, t.order[i], 0, r.path);
    }
    for (const cook::BtDocKey& k : t.doc->keys) {
        cook::String v;
        const StringView type = k.type.view();
        if (type == "vec3") {
            math::vec3 p;
            if (kit::bb_vector(w, t.brain, k.name.view(), p))
                cook::appendf(v, "(%.2f, %.2f, %.2f)", static_cast<double>(p.x), static_cast<double>(p.y), static_cast<double>(p.z));
        } else if (type == "entity") {
            Entity e;
            if (kit::bb_entity(w, t.brain, k.name.view(), e)) cook::appendf(v, "#%u", e.index);
        } else {
            f32 x = 0.0f;
            if (kit::bb_number(w, t.brain, k.name.view(), x)) {
                if (type == "bool") v.assign(x != 0.0f ? "true" : "false");
                else cook::appendf(v, "%g", static_cast<double>(x));
            }
        }
        if (v.empty()) continue;
        if (!r.keys.empty()) r.keys += " ";
        r.keys += k.name.view();
        r.keys += "=";
        r.keys += v.view();
    }
    t.rows.push_back(r);
}

const char* status_name(kit::BtStatus s) {
    switch (s) {
    case kit::BtStatus::Running: return "running";
    case kit::BtStatus::Success: return "success";
    case kit::BtStatus::Failure: return "failure";
    case kit::BtStatus::Idle: break;
    }
    return "idle";
}

bool bt_trace(ToolCall& c) {
    if (!need_project(c)) return false;
    editor::EntityId id = editor::kNoEntity;
    if (!tool_arg_entity(c, "entity", id)) return false;
    u64 ticks = 300, every = 30;
    if (!arg_uint(c, "ticks", ticks, 1, kMaxTicks, false) || !arg_uint(c, "every", every, 1, kMaxTicks, false)) return false;
    const Server::SceneFile& file = c.server.scene_file();
    if (file.rel.empty()) return fail(c, "the scene was never saved: scene_save it (with a path under assets/) first");
    if (scene(c).dirty()) return fail(c, "the scene has unsaved changes: scene_save first (bt_trace plays the cooked file)");
    const editor::SceneDocument& doc = scene(c).doc();
    const editor::DocEntity* e = doc.find(id);
    const editor::DocComponent* bc = e ? e->find(refl::type_of<kit::AiBrain>()) : nullptr;
    if (!bc) return fail(c, "argument 'entity' has no AiBrain (component_add one with its tree)");
    const asset::Guid tree = static_cast<const kit::AiBrain*>(bc->data())->tree.guid;
    const editor::ProjectAsset* a = c.server.session().project().find(tree);
    if (!a || a->type != asset::ChunkType::BehaviorTree) return fail(c, "the entity's AiBrain names no behavior tree of the project");
    cook::String text;
    cook::BtDocument d;
    if (!cook::fs::read_text(c.server.session().project().asset_path(a->path.view()).view(), text) ||
        !read_doc(c, a->path.view(), text.view(), d, "the tree does not read"))
        return false;

    TraceRun t;
    t.every = every;
    t.doc = &d;
    d.preorder(t.order);
    while (t.index < doc.size() && doc.at(t.index).id != id) ++t.index;
    const cook::CookReport& r = c.server.session().cook(false);
    if (!r.ok()) {
        c.error += "cook failed:";
        append_diags(c.error, r.diags, 10);
        return false;
    }
    RunHooks hooks;
    hooks.user = &t;
    hooks.start = [](World&, Span<const Entity> spawned, void* user) {
        auto* tr = static_cast<TraceRun*>(user);
        if (tr->index < spawned.size()) tr->brain = spawned[tr->index];
    };
    hooks.tick = [](const World& world, Span<const Entity>, u64 tick, void* user) {
        auto* tr = static_cast<TraceRun*>(user);
        // The hook reads only; the blackboard and registry lookups take a mutable world.
        trace_sample(const_cast<World&>(world), *tr, tick, tick % tr->every == 0 || tick == 1);
        return true;
    };
    const cook::String cooked = cook::path_join(c.server.session().project().root(), "cooked");
    Result<HeadlessRun> run = run_headless(cooked.view(), doc.guid(), ticks, nullptr, nullptr, nullptr, nullptr, nullptr,
                                           c.server.session().project().base_roots(), &hooks);
    if (!run) {
        FixedString<512> msg;
        format_error(run.error(), msg);
        c.error += "run failed: ";
        c.error += msg.view();
        return false;
    }
    if (t.brain.is_null()) return fail(c, "the entity was not spawned by the run");
    if (t.lost) return fail(c, "the brain did not run this tree (cook_errors shows why)");
    c.out.begin_object();
    c.out.key("tree").str(a->path.view()).key("ticks").num_u(run.value().ticks);
    c.out.key("rows").begin_array();
    for (const TraceRun::Row& row : t.rows) {
        c.out.begin_object();
        c.out.key("tick").num_u(row.tick).key("result").str(status_name(row.result)).key("runs").num_u(row.runs);
        c.out.key("running").str(row.path.view()).key("blackboard").str(row.keys.view());
        c.out.end_object();
    }
    c.out.end_array();
    c.out.key("started").begin_array();
    for (const cook::String& s : t.changes) c.out.str(s.view());
    c.out.end_array();
    c.out.end_object();
    return true;
}

#define MCE_BT_DOC R"j("tree":{"type":"string","description":"The tree: a .mcbt path under assets/ or \"guid:<uuid>\""})j"

const ToolDef kBtTools[] = {
    {"bt_create",
     "Make a new behavior tree (.mcbt: an AI's decisions as data) with a fresh GUID. 'blackboard' declares its keys "
     "({\"alert\": {\"type\": \"bool\", \"default\": false}, \"target\": \"entity\"}); 'root' is a .mcbt node with its "
     "children (asset_schema type BehaviorTree lists every kind and stock task; default: a selector over a wait). Checked "
     "by the cook before it is written. Run it with an AiBrain component naming its GUID (and a Perceiver to see, a "
     "NavAgent3D or PathAgent to move); bt_trace shows what it does. The result lists the nodes by index for bt_add_node "
     "and bt_set.",
     R"j({"type":"object","properties":{"path":{"type":"string","description":"New .mcbt path under assets/, e.g. ai/guard.mcbt"},)j"
     R"j("name":{"type":"string"},"blackboard":{"type":"object","description":"key name -> type or {type, default}"},)j"
     R"j("root":{"type":"object","description":"The root node, e.g. {\"selector\": [{\"guard\": {...}, \"key\": \"target\", \"abort\": \"lower\"}, ...]}"}},)j"
     R"j("required":["path"]})j",
     bt_create},
    {"bt_add_node",
     "Add a node (with its children) under a composite or an empty decorator of a tree, last or at 'index' among its "
     "children. 'parent' is a node index from the outline the tools return (0 = the root). E.g. {\"task\": \"move_to\", "
     "\"key\": \"home\", \"radius\": 0.5} or {\"sequence\": [{\"wait\": 1}, {\"emit\": \"bored\"}]}.",
     R"j({"type":"object","properties":{)j" MCE_BT_DOC
     R"j(,"parent":{"type":"integer","minimum":0,"description":"Node index (default 0, the root)"},)j"
     R"j("node":{"type":"object","description":"A .mcbt node object"},)j"
     R"j("index":{"type":"integer","minimum":0,"description":"Position among the parent's children (default last)"}},"required":["tree","node"]})j",
     bt_add_node},
    {"bt_set",
     "Change a tree: its 'name'; blackboard keys ('blackboard': name -> type, {type, default} or null to remove); and "
     "one node by index: a leaf's 'value' (a task's name, a check's key, a wait's seconds), its members ('fields': name -> "
     "value or null, e.g. {\"abort\": \"lower\", \"radius\": 2}), 'replace' it with another node object, or 'remove' it "
     "with its subtree.",
     R"j({"type":"object","properties":{)j" MCE_BT_DOC
     R"j(,"name":{"type":"string"},"blackboard":{"type":"object"},"node":{"type":"integer","minimum":0,"description":"Node index"},)j"
     R"j("value":{"description":"A leaf's own value"},"fields":{"type":"object"},"replace":{"type":"object"},)j"
     R"j("remove":{"type":"boolean"}},"required":["tree"]})j",
     bt_set},
    {"bt_trace",
     "Play the saved scene headless (as run_headless does) and report what one entity's AiBrain did: every 'every' ticks "
     "the root's result, the running path (node indices and labels) and the blackboard's values; and each tick a node "
     "started running ('started'). Use it to check an AI reacts as meant: the guard leaves its patrol when it sees the "
     "player, a utility node switches when health drops.",
     R"j({"type":"object","properties":{"entity":{"description":"Entity id (integer) or unique name"},)j"
     R"j("ticks":{"type":"integer","minimum":1,"maximum":36000,"description":"Ticks to play (default 300)"},)j"
     R"j("every":{"type":"integer","minimum":1,"description":"A row every N ticks (default 30)"}},"required":["entity"]})j",
     bt_trace},
};

#undef MCE_BT_DOC

}  // namespace

Span<const ToolDef> bt_tool_table() { return Span<const ToolDef>(kBtTools, sizeof(kBtTools) / sizeof(kBtTools[0])); }

}  // namespace mce::mcp
