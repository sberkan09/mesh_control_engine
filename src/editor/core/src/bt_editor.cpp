// Behavior tree authoring and debugging (ROADMAP M-AI step 5; bt_editor.h).

#include "mce/editor/bt_editor.h"

#include <cmath>
#include <cstdio>
#include <limits>

#include "mce/cook/cook.h"
#include "mce/cook/fs.h"
#include "mce/cook/json.h"

namespace mce::editor {

using cook::BtArity;
using cook::BtDocKey;
using cook::BtDocNode;
using cook::BtDocument;

namespace {

const refl::TypeRecord& brain_type() { return refl::type_of<kit::AiBrain>(); }

// A leaf named only by its kind value, as JSON text.
BtDocNode leaf(StringView kind, StringView value) {
    BtDocNode n;
    n.kind.assign(kind);
    n.value.assign(value);
    return n;
}

cook::String quoted(StringView s) {
    cook::String out;
    cook::append_json_string(out, s);
    return out;
}

// The default value a key's type takes in a set node.
StringView zero_of(StringView type) {
    if (type == "bool") return "false";
    if (type == "vec3") return "[0, 0, 0]";
    return "0";
}

// Appends `n` (and its subtree, already in `from`) to `to`; returns its new index.
u32 copy_subtree(const BtDocument& from, u32 n, BtDocument& to) {
    const u32 at = static_cast<u32>(to.nodes.size());
    to.nodes.push_back(from.nodes[n]);
    to.nodes[at].children.clear();
    for (u32 c : from.nodes[n].children) {
        const u32 k = copy_subtree(from, c, to);
        to.nodes[at].children.push_back(k);
    }
    return at;
}

}  // namespace

// ---------------------------------------------------------------- the document

bool BtEditor::read_text(StringView text, BtDocument& out) {
    cook::JsonDoc doc;
    cook::JsonError jerr;
    if (!doc.parse(text, jerr) || !doc.root().is_object()) {
        error_ = cook::String("not a JSON object");
        return false;
    }
    cook::DiagList diags;
    if (cook::read_behavior_tree_document(doc.root(), rel_.view(), out, diags)) return true;
    error_ = cook::String();
    u32 shown = 0;
    for (const cook::Diagnostic& d : diags.all()) {
        if (d.severity != cook::Severity::Error) continue;
        if (shown++ == 3) break;
        if (!error_.empty()) error_ += "; ";
        error_ += d.message.view();
    }
    if (error_.empty()) error_ = cook::String("the tree does not read");
    return false;
}

bool BtEditor::check(const BtDocument& next, BtDocument& out, cook::String& text, Array<byte>& payload,
                     Array<cook::String>& problems) {
    text = cook::String();
    cook::write_behavior_tree_document(next, guid_, text);
    if (!read_text(text.view(), out)) return false;
    cook::JsonDoc json;
    cook::JsonError jerr;
    (void)json.parse(text.view(), jerr);
    cook::DiagList diags;
    problems.clear();
    if (!cook::build_behavior_tree(json.root(), rel_.view(), payload, diags)) {
        payload.clear();
        for (const cook::Diagnostic& d : diags.all()) {
            if (d.severity != cook::Severity::Error) continue;
            cook::String p;
            cook::appendf(p, "%u:%u: %.*s", d.line, d.col, static_cast<int>(d.message.size()), d.message.view().data());
            problems.push_back(p);
        }
        if (problems.empty()) problems.push_back(cook::String("the tree does not cook"));
    }
    return true;
}

cook::String BtEditor::text() const {
    cook::String t;
    cook::write_behavior_tree_document(doc_, guid_, t);
    return t;
}

void BtEditor::set_doc(const BtDocument& d, Array<byte>&& payload, Array<cook::String>&& problems) {
    doc_ = d;
    doc_.preorder(order_);
    chunk_ = static_cast<Array<byte>&&>(payload);
    problems_ = static_cast<Array<cook::String>&&>(problems);
    cooks_ = problems_.empty() && !chunk_.empty();
    if (selected_ >= doc_.nodes.size()) selected_ = kNoBtNode;
    ++revision_;
}

bool BtEditor::open(EditorSession& session, asset::Guid guid) {
    if (guid == guid_ && dirty_) return true;
    const ProjectAsset* a = session.project().find(guid);
    if (!a || a->type != asset::ChunkType::BehaviorTree) {
        error_ = cook::String("not a behavior tree (.mcbt)");
        return false;
    }
    cook::String file;
    if (!cook::fs::read_text(session.project().asset_path(a->path.view()).view(), file)) {
        error_ = cook::String("the tree could not be read");
        return false;
    }
    const cook::String rel_before = rel_;
    const asset::Guid guid_before = guid_;
    rel_ = a->path;
    guid_ = guid;
    BtDocument first, d;
    cook::String t;
    Array<byte> payload;
    Array<cook::String> problems;
    if (!read_text(file.view(), first) || !check(first, d, t, payload, problems)) {
        rel_ = rel_before;
        guid_ = guid_before;
        return false;
    }
    if (guid != guid_before) {
        undo_.clear();
        redo_.clear();
        selected_ = kNoBtNode;
    }
    disk_ = file;
    dirty_ = false;
    error_ = cook::String();
    set_doc(d, static_cast<Array<byte>&&>(payload), static_cast<Array<cook::String>&&>(problems));
    return true;
}

void BtEditor::close() {
    guid_ = {};
    rel_ = cook::String();
    disk_ = cook::String();
    doc_ = BtDocument();
    order_.clear();
    chunk_.clear();
    problems_.clear();
    cooks_ = false;
    selected_ = kNoBtNode;
    dirty_ = false;
    undo_.clear();
    redo_.clear();
    ++revision_;
}

Result<asset::Guid> BtEditor::create(EditorSession& session, StringView rel_path) {
    constexpr const char* kWhere = "editor.bt_create";
    if (!session.project().is_open()) return Error(Errc::InvalidArgument, kWhere, "no project is open");
    if (rel_path.size() < 6 || rel_path.substr(rel_path.size() - 5) != ".mcbt")
        return Error(Errc::InvalidArgument, kWhere, "a behavior tree's path ends in .mcbt");
    if (cook::fs::exists(session.project().asset_path(rel_path).view())) return Error(Errc::AlreadyExists, kWhere, rel_path);
    BtDocument d;
    BtDocNode root;
    root.kind.assign("selector");
    root.children.push_back(1);
    d.nodes.push_back(root);
    d.nodes.push_back(leaf("wait", "1"));
    const asset::Guid guid = cook::generate_guid();
    cook::String t;
    cook::write_behavior_tree_document(d, guid, t);
    MCE_TRY(session.write_asset(rel_path, t.view()));
    if (!open(session, guid)) return Error(Errc::ImportFailed, kWhere, "the new tree did not cook (see the log)");
    return guid;
}

bool BtEditor::sync(EditorSession& session) {
    if (!is_open() || dirty_) return false;
    const ProjectAsset* a = session.project().find(guid_);
    if (!a) {
        close();
        return true;
    }
    cook::String file;
    if (!cook::fs::read_text(session.project().asset_path(a->path.view()).view(), file)) return false;
    if (file.view() == disk_.view() && a->path.view() == rel_.view()) return false;
    rel_ = a->path;
    disk_ = file;
    BtDocument first, d;
    cook::String t;
    Array<byte> payload;
    Array<cook::String> problems;
    if (!read_text(file.view(), first) || !check(first, d, t, payload, problems)) return false;
    error_ = cook::String();
    set_doc(d, static_cast<Array<byte>&&>(payload), static_cast<Array<cook::String>&&>(problems));
    return true;
}

void BtEditor::select(u32 node) { selected_ = node < doc_.nodes.size() ? node : kNoBtNode; }

u32 BtEditor::cooked_index(u32 node) const {
    for (u32 i = 0; i < order_.size(); ++i)
        if (order_[i] == node) return i;
    return kNoBtNode;
}

u32 BtEditor::node_at(u32 cooked) const { return cooked < order_.size() ? order_[cooked] : kNoBtNode; }

// ---------------------------------------------------------------- edits

bool BtEditor::apply(const BtDocument& next, u32 keep) {
    if (!is_open()) {
        error_ = cook::String("no tree is open");
        return false;
    }
    BtDocument back;
    cook::String t;
    Array<byte> payload;
    Array<cook::String> problems;
    if (!check(next, back, t, payload, problems)) return false;
    if (!problems.empty() && cooks_) {
        error_ = problems[0];
        return false;
    }
    error_ = cook::String();
    // The node to select, renumbered: the read-back document is in pre-order.
    u32 select_after = kNoBtNode;
    if (keep != kNoBtNode) {
        Array<u32> order;
        next.preorder(order);
        for (u32 i = 0; i < order.size(); ++i)
            if (order[i] == keep) select_after = i;
    }
    const cook::String before = text();
    if (t.view() == before.view()) {
        if (select_after != kNoBtNode) selected_ = select_after;
        return true;
    }
    if (!dirty_) {
        undo_.push_back(before);
        if (undo_.size() > kMaxUndo) undo_.erase(0);
    }
    redo_.clear();
    dirty_ = true;
    set_doc(back, static_cast<Array<byte>&&>(payload), static_cast<Array<cook::String>&&>(problems));
    selected_ = select_after < doc_.nodes.size() ? select_after : kNoBtNode;
    return true;
}

bool BtEditor::add_node(u32 parent, StringView kind, u32 at) {
    if (!child_ok(parent)) {
        error_ = cook::String("no such node");
        return false;
    }
    if (!cook::bt_is_kind(kind)) {
        error_ = cook::String("not a node kind");
        return false;
    }
    const BtArity parent_arity = cook::bt_kind_arity(doc_.nodes[parent].kind.view());
    if (parent_arity == BtArity::Leaf) {
        error_ = cook::String("a leaf has no children");
        return false;
    }
    if (parent_arity == BtArity::One && !doc_.nodes[parent].children.empty()) {
        error_ = cook::String("a decorator has one child: wrap it or remove the child first");
        return false;
    }
    BtDocument next = doc_;
    const u32 index = static_cast<u32>(next.nodes.size());
    BtDocNode n;
    n.kind.assign(kind);
    const StringView first_key = doc_.keys.empty() ? StringView() : doc_.keys[0].name.view();
    const BtArity arity = cook::bt_kind_arity(kind);
    if (arity == BtArity::Leaf) {
        if (kind == "wait") n.value.assign("1");
        else if (kind == "emit") n.value.assign("\"signal\"");
        else if (kind == "task") {
            n.value.assign("\"log\"");
            n.set_field("text", "\"hello\"");
        } else if (kind == "check" || kind == "set") {
            if (first_key.empty()) {
                error_ = cook::String("a check or set names a blackboard key: add one first");
                return false;
            }
            n.value = quoted(first_key);
            if (kind == "set") {
                const StringView type = doc_.keys[0].type.view();
                if (type == "entity") n.set_field("clear", "true");
                else n.set_field("value", zero_of(type));
            }
        }
    } else {
        if (kind == "guard") {
            if (first_key.empty()) {
                error_ = cook::String("a guard names a blackboard key: add one first");
                return false;
            }
            n.set_field("key", quoted(first_key).view());
        }
        if (kind == "cooldown" || kind == "timeout") n.set_field("seconds", "1");
        if (kind == "token") n.set_field("pool", "\"attack\"");
        n.children.push_back(index + 1);
    }
    next.nodes.push_back(n);
    if (arity != BtArity::Leaf) {
        if (kind == "utility") {
            BtDocNode score;
            score.kind.assign("score");
            score.children.push_back(index + 2);
            next.nodes.push_back(score);
        }
        next.nodes.push_back(leaf("wait", "0"));
    }
    Array<u32>& kids = next.nodes[parent].children;
    if (at >= kids.size()) kids.push_back(index);
    else kids.insert(at, index);
    return apply(next, index);
}

bool BtEditor::remove_node(u32 node) {
    if (!child_ok(node)) {
        error_ = cook::String("no such node");
        return false;
    }
    const u32 parent = doc_.parent(node);
    if (parent == kNoBtNode) {
        error_ = cook::String("the root is not removed");
        return false;
    }
    BtDocument next = doc_;
    Array<u32>& kids = next.nodes[parent].children;
    for (usize i = 0; i < kids.size(); ++i)
        if (kids[i] == node) {
            kids.erase(i);
            break;
        }
    return apply(next, parent);
}

bool BtEditor::move_node(u32 node, i32 by) {
    const u32 parent = child_ok(node) ? doc_.parent(node) : kNoBtNode;
    if (parent == kNoBtNode) {
        error_ = cook::String("only a child moves among its siblings");
        return false;
    }
    BtDocument next = doc_;
    Array<u32>& kids = next.nodes[parent].children;
    i64 from = 0;
    for (usize i = 0; i < kids.size(); ++i)
        if (kids[i] == node) from = static_cast<i64>(i);
    i64 to = from + by;
    if (to < 0) to = 0;
    if (to >= static_cast<i64>(kids.size())) to = static_cast<i64>(kids.size()) - 1;
    if (to == from) return true;
    kids.erase(static_cast<usize>(from));
    kids.insert(static_cast<usize>(to), node);
    return apply(next, node);
}

bool BtEditor::wrap_node(u32 node, StringView kind) {
    if (!child_ok(node)) {
        error_ = cook::String("no such node");
        return false;
    }
    if (!cook::bt_is_kind(kind) || cook::bt_kind_arity(kind) == BtArity::Leaf) {
        error_ = cook::String("a node is wrapped in a decorator or a composite");
        return false;
    }
    BtDocument next = doc_;
    const u32 index = static_cast<u32>(next.nodes.size());
    BtDocNode w;
    w.kind.assign(kind);
    w.children.push_back(node);
    if (kind == "guard") {
        if (doc_.keys.empty()) {
            error_ = cook::String("a guard names a blackboard key: add one first");
            return false;
        }
        w.set_field("key", quoted(doc_.keys[0].name.view()).view());
    }
    if (kind == "cooldown" || kind == "timeout") w.set_field("seconds", "1");
    if (kind == "token") w.set_field("pool", "\"attack\"");
    next.nodes.push_back(w);
    const u32 parent = doc_.parent(node);
    if (parent == kNoBtNode) {
        // The root: the wrapper becomes node 0.
        BtDocument rooted;
        rooted.name = next.name;
        rooted.keys = next.keys;
        copy_subtree(next, index, rooted);
        return apply(rooted, 0);
    }
    for (u32& c : next.nodes[parent].children)
        if (c == node) c = index;
    return apply(next, index);
}

bool BtEditor::unwrap_node(u32 node) {
    if (!child_ok(node) || doc_.nodes[node].children.size() != 1) {
        error_ = cook::String("only a node with one child is unwrapped");
        return false;
    }
    const u32 child = doc_.nodes[node].children[0];
    const u32 parent = doc_.parent(node);
    BtDocument next = doc_;
    if (parent == kNoBtNode) {
        BtDocument rooted;
        rooted.name = next.name;
        rooted.keys = next.keys;
        copy_subtree(next, child, rooted);
        return apply(rooted, 0);
    }
    for (u32& c : next.nodes[parent].children)
        if (c == node) c = child;
    return apply(next, child);
}

bool BtEditor::set_value(u32 node, StringView json) {
    if (!child_ok(node) || cook::bt_kind_arity(doc_.nodes[node].kind.view()) != BtArity::Leaf) {
        error_ = cook::String("only a leaf has a value");
        return false;
    }
    BtDocument next = doc_;
    next.nodes[node].value.assign(json);
    return apply(next, node);
}

bool BtEditor::set_field(u32 node, StringView name, StringView json) {
    if (!child_ok(node)) {
        error_ = cook::String("no such node");
        return false;
    }
    if (name.empty() || name == doc_.nodes[node].kind.view()) {
        error_ = cook::String("a field needs a name other than the node's kind");
        return false;
    }
    BtDocument next = doc_;
    next.nodes[node].set_field(name, json);
    return apply(next, node);
}

bool BtEditor::add_key(StringView name, StringView type, StringView def) {
    if (doc_.find_key(name) != kNoBtNode) {
        error_ = cook::String("a key is declared twice");
        return false;
    }
    BtDocument next = doc_;
    BtDocKey k;
    k.name.assign(name);
    k.type.assign(type);
    k.def.assign(def);
    next.keys.push_back(k);
    return apply(next, selected_);
}

bool BtEditor::set_key(u32 key, const BtDocKey& k) {
    if (key >= doc_.keys.size()) {
        error_ = cook::String("no such key");
        return false;
    }
    BtDocument next = doc_;
    const cook::String old = next.keys[key].name;
    next.keys[key] = k;
    // A rename follows into the nodes that name the key by value or by a field.
    if (old.view() != k.name.view()) {
        const cook::String from = quoted(old.view()), to = quoted(k.name.view());
        for (BtDocNode& n : next.nodes) {
            if (n.value.view() == from.view()) n.value = to;
            for (cook::BtDocField& f : n.fields)
                if (f.json.view() == from.view() && f.name.view() != "name" && f.name.view() != "text" && f.name.view() != "event")
                    f.json = to;
        }
    }
    return apply(next, selected_);
}

bool BtEditor::remove_key(u32 key) {
    if (key >= doc_.keys.size()) {
        error_ = cook::String("no such key");
        return false;
    }
    BtDocument next = doc_;
    next.keys.erase(key);
    return apply(next, selected_);
}

bool BtEditor::set_name(StringView name) {
    BtDocument next = doc_;
    next.name.assign(name);
    return apply(next, selected_);
}

Result<void> BtEditor::save(EditorSession& session) {
    if (!dirty_) return ok();
    const cook::String t = text();
    MCE_TRY(session.write_asset(rel_.view(), t.view()));
    disk_ = t;
    dirty_ = false;
    return ok();
}

bool BtEditor::undo(EditorSession& session) {
    if (undo_.empty()) return false;
    BtDocument first, d;
    cook::String t;
    Array<byte> payload;
    Array<cook::String> problems;
    if (!read_text(undo_.back().view(), first) || !check(first, d, t, payload, problems)) return false;
    redo_.push_back(text());
    undo_.pop_back();
    set_doc(d, static_cast<Array<byte>&&>(payload), static_cast<Array<cook::String>&&>(problems));
    dirty_ = true;
    return static_cast<bool>(save(session));
}

bool BtEditor::redo(EditorSession& session) {
    if (redo_.empty()) return false;
    BtDocument first, d;
    cook::String t;
    Array<byte> payload;
    Array<cook::String> problems;
    if (!read_text(redo_.back().view(), first) || !check(first, d, t, payload, problems)) return false;
    if (dirty_) {
        if (Result<void> r = save(session); !r) return false;
    }
    undo_.push_back(text());
    redo_.pop_back();
    set_doc(d, static_cast<Array<byte>&&>(payload), static_cast<Array<cook::String>&&>(problems));
    dirty_ = true;
    return static_cast<bool>(save(session));
}

void BtEditor::users(const EditorScene& scene, Array<EntityId>& out) const {
    out.clear();
    if (!is_open()) return;
    for (const DocEntity& e : scene.doc().entities()) {
        const DocComponent* c = e.find(brain_type());
        if (c && static_cast<const kit::AiBrain*>(c->data())->tree.guid == guid_) out.push_back(e.id);
    }
}

// ---------------------------------------------------------------- the debugger

void bt_play_brains(EditorSession& session, const BtEditor& ed, Array<Entity>& out) {
    out.clear();
    World* w = session.play().active() ? session.play().world() : nullptr;
    if (!w || !ed.is_open() || !w->try_resource<kit::BtRegistry>()) return;
    w->query<kit::AiBrain>().each([&](Entity e, const kit::AiBrain& b) {
        if (b.tree.guid == ed.guid()) out.push_back(e);
    });
}

bool bt_live(EditorSession& session, const BtEditor& ed, Entity brain, BtLive& out) {
    out = BtLive();
    World* w = session.play().active() ? session.play().world() : nullptr;
    if (!w || !ed.is_open() || !w->alive(brain) || !w->try_resource<kit::BtRegistry>()) return false;
    const kit::AiBrain* b = w->try_get<kit::AiBrain>(brain);
    if (!b || b->tree.guid != ed.guid()) return false;
    const cook::BtDocument& d = ed.doc();
    out.brain = brain;
    out.active = b->active;
    out.result = b->result;
    out.runs = b->runs;
    out.time = b->time;
    out.status.resize(d.nodes.size(), kit::BtStatus::Idle);
    out.score.resize(d.nodes.size(), std::numeric_limits<f32>::quiet_NaN());
    kit::BtRegistry* reg = w->try_resource<kit::BtRegistry>();
    const kit::BtRegistry::Def* def = reg ? reg->resolve(*b) : nullptr;
    // The running tree matches the document when it has the same nodes in the same order.
    if (def && def->nodes.size() == d.nodes.size()) {
        out.current = true;
        for (u32 i = 0; i < def->nodes.size() && out.current; ++i) {
            const u32 n = ed.node_at(i);
            out.current = n != kNoBtNode && static_cast<u32>(def->nodes[i].kind) == [&] {
                const Span<const char* const> names = cook::bt_kind_names();
                for (u32 k = 0; k < names.size(); ++k)
                    if (d.nodes[n].kind.view() == names[k]) return k;
                return ~0u;
            }();
        }
    }
    if (out.current) {
        Array<f32> scores;
        for (u32 i = 0; i < def->nodes.size(); ++i) {
            const u32 n = ed.node_at(i);
            out.status[n] = kit::bt_node_status(*b, i);
            if (def->nodes[i].kind != asset::BtNodeKind::Utility || !kit::bt_utility_scores(*w, brain, i, scores)) continue;
            for (usize c = 0; c < scores.size() && c < d.nodes[n].children.size(); ++c) out.score[d.nodes[n].children[c]] = scores[c];
        }
    }
    for (const cook::BtDocKey& k : d.keys) {
        BtLiveKey lk;
        lk.name = k.name;
        lk.type = k.type;
        const StringView t = k.type.view();
        if (t == "vec3") {
            math::vec3 v;
            if ((lk.set = kit::bb_vector(*w, brain, k.name.view(), v)))
                cook::appendf(lk.value, "(%.2f, %.2f, %.2f)", static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z));
        } else if (t == "entity") {
            Entity e;
            if ((lk.set = kit::bb_entity(*w, brain, k.name.view(), e))) cook::appendf(lk.value, "#%u", e.index);
        } else {
            f32 v = 0.0f;
            if ((lk.set = kit::bb_number(*w, brain, k.name.view(), v))) {
                if (t == "bool") lk.value.assign(v != 0.0f ? "true" : "false");
                else if (t == "int") cook::appendf(lk.value, "%d", static_cast<int>(v));
                else cook::appendf(lk.value, "%g", static_cast<double>(v));
            }
        }
        out.keys.push_back(lk);
    }
    return true;
}

bool bt_push_to_play(EditorSession& session, const BtEditor& ed) {
    World* w = session.play().active() ? session.play().world() : nullptr;
    if (!w || !ed.is_open() || !ed.cooks()) return false;
    kit::BtRegistry* reg = w->try_resource<kit::BtRegistry>();
    if (!reg || !reg->define(ed.guid(), ed.chunk())) return false;
    // A tree with other bytes restarts its brains (AiBrain::tree_hash); a restart makes sure.
    w->query<kit::AiBrain>().each([&](Entity, kit::AiBrain& b) {
        if (b.tree.guid == ed.guid()) b.restart = true;
    });
    return true;
}

}  // namespace mce::editor
