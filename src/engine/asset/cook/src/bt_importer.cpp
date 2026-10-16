// .mcbt -> CHNK_BT: one behavior tree (M-AI 1, ADR-0045). The blackboard declares the keys the
// tree reads and writes; the root is one node, and every node is an object with exactly one kind
// key (plus that kind's extras and an optional "name" the debugger shows). Mistakes are cook
// errors naming file:line:col.
//
// {
//   "format": "mce-bt", "version": 1, "guid": "<uuid>", "name": "guard",
//   "blackboard": {
//     "alert":  {"type": "bool", "default": false},
//     "home":   {"type": "vec3", "default": [0, 0, 0]},
//     "target": "entity"                                         // the type alone: no default
//   },
//   "root": {"selector": [
//     {"sequence": [
//       {"check": "alert"},                                      // is_set, or "op"/"value"
//       {"timeout": {"task": "chase", "key": "target"}, "seconds": 5},
//       {"set": "alert", "value": false}
//     ], "name": "attack"},
//     {"sequence": [{"wait": 2}, {"emit": "bored"}]}
//   ]}
// }
//
// Kinds: sequence / selector / parallel ("policy": "all" | "one") take an array of children;
// invert / succeed / fail / repeat ("count") / retry ("count") / cooldown ("seconds") / timeout
// ("seconds") / guard ("key", "op", "value") take one child; check (a key, "op", "value"),
// wait (seconds), set (a key and one of "value", "add", "clear": true, "from": key), emit (a
// signal) and task (a registered task's name, with "key", "value" and "vector" arguments) are
// leaves. Ops: "is_set", "not_set", "==", "!=", "<", "<=", ">", ">=" (the default is "==" when
// there is a "value", else "is_set").

#include <cstdio>
#include <cstring>

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/string_id.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.bt";
constexpr u32 kBtFormatVersion = 1;

using asset::BtKey;
using asset::BtKeyType;
using asset::BtNode;
using asset::BtNodeKind;
using asset::BtOp;
using asset::BtSetOp;

struct Kind {
    const char* key;
    BtNodeKind kind;
    const char* extras[4];
};

constexpr Kind kKinds[] = {
    {"sequence", BtNodeKind::Sequence, {}},
    {"selector", BtNodeKind::Selector, {}},
    {"parallel", BtNodeKind::Parallel, {"policy"}},
    {"invert", BtNodeKind::Invert, {}},
    {"succeed", BtNodeKind::Succeed, {}},
    {"fail", BtNodeKind::Fail, {}},
    {"repeat", BtNodeKind::Repeat, {"count"}},
    {"retry", BtNodeKind::Retry, {"count"}},
    {"cooldown", BtNodeKind::Cooldown, {"seconds"}},
    {"timeout", BtNodeKind::Timeout, {"seconds"}},
    {"guard", BtNodeKind::Guard, {"key", "op", "value"}},
    {"check", BtNodeKind::Check, {"op", "value"}},
    {"wait", BtNodeKind::Wait, {}},
    {"set", BtNodeKind::Set, {"value", "add", "clear", "from"}},
    {"emit", BtNodeKind::Emit, {}},
    {"task", BtNodeKind::Task, {"key", "value", "vector"}},
};

constexpr const char* kOps[] = {"is_set", "not_set", "==", "!=", "<", "<=", ">", ">="};

struct Builder {
    StringView file;
    DiagList& diags;
    Array<BtKey> keys;
    Array<FixedString<64>> key_names;
    Array<BtNode> nodes;
    Array<char> strings;
    u32 slots = 0;

    void error(JsonValue at, StringView msg) { detail::error_at(diags, Errc::ParseError, file, at, kStage, msg); }

    u32 intern(StringView s) {
        const u32 at = static_cast<u32>(strings.size());
        for (char c : s) strings.push_back(c);
        strings.push_back('\0');
        return at;
    }

    // The key a node names: its index, or kBtNoKey after an error.
    u16 key_of(JsonValue v) {
        if (!v.is_string()) {
            error(v, "expected a blackboard key's name");
            return asset::kBtNoKey;
        }
        for (usize i = 0; i < key_names.size(); ++i)
            if (key_names[i].view() == v.as_string()) return static_cast<u16>(i);
        String msg;
        appendf(msg, "'%.*s' is not a key of the blackboard", static_cast<int>(v.as_string().size()), v.as_string().data());
        error(v, msg);
        return asset::kBtNoKey;
    }

    bool numeric(u16 k) const {
        return k != asset::kBtNoKey && (keys[k].type == BtKeyType::Bool || keys[k].type == BtKeyType::Int || keys[k].type == BtKeyType::Float);
    }

    // A number for a numeric key: true / false for Bool, an integer for Int.
    bool number_for(u16 k, JsonValue v, f32& out) {
        if (k == asset::kBtNoKey) return false;
        const BtKeyType t = keys[k].type;
        if (t == BtKeyType::Bool) {
            if (!v.is_bool()) {
                error(v, "a bool key takes true or false");
                return false;
            }
            out = v.as_bool() ? 1.0f : 0.0f;
            return true;
        }
        if (!v.is_number() || (t == BtKeyType::Int && !v.is_integer())) {
            error(v, t == BtKeyType::Int ? "an int key takes an integer" : "a float key takes a number");
            return false;
        }
        out = static_cast<f32>(v.as_number());
        return true;
    }

    bool vec3(JsonValue v, f32 out[3]) {
        if (!v.is_array() || v.size() != 3) {
            error(v, "expected [x, y, z]");
            return false;
        }
        for (u32 i = 0; i < 3; ++i) {
            if (!v.at(i).is_number()) {
                error(v.at(i), "expected a number");
                return false;
            }
            out[i] = static_cast<f32>(v.at(i).as_number());
        }
        return true;
    }

    void read_blackboard(JsonValue bb) {
        if (!bb.valid()) return;
        if (!bb.is_object()) {
            error(bb, "'blackboard' must be an object of key: type or key: {\"type\", \"default\"}");
            return;
        }
        u32 used[3] = {};
        for (u32 i = 0; i < bb.size(); ++i) {
            const StringView name = bb.key_at(i);
            const JsonValue v = bb.get(name);
            if (name.empty() || name.size() >= 64) {
                error(v, "a key's name is 1 to 63 bytes");
                continue;
            }
            for (const FixedString<64>& n : key_names)
                if (n.view() == name) error(v, "a key is declared twice");
            JsonValue type = v, def;
            if (v.is_object()) {
                if (!detail::check_keys(v, {"type", "default"}, file, kStage, diags)) continue;
                type = v.get("type");
                def = v.get("default");
            }
            static constexpr const char* kTypes[] = {"bool", "int", "float", "vec3", "entity"};
            i32 t = -1;
            for (u32 j = 0; j < 5; ++j)
                if (type.is_string() && type.as_string() == kTypes[j]) t = static_cast<i32>(j);
            if (t < 0) {
                error(type.valid() ? type : v, "a key's type is bool, int, float, vec3 or entity");
                continue;
            }
            BtKey k{};
            k.id = StringId(name).value;
            k.type = static_cast<BtKeyType>(t);
            const u32 arr = t <= 2 ? 0 : t == 3 ? 1 : 2;
            const u32 cap = arr == 0 ? asset::kBtNumbers : arr == 1 ? asset::kBtVectors : asset::kBtEntities;
            if (used[arr] >= cap) {
                String msg;
                appendf(msg, "a blackboard holds at most %u bool/int/float, %u vec3 and %u entity keys", asset::kBtNumbers,
                        asset::kBtVectors, asset::kBtEntities);
                error(v, msg);
                continue;
            }
            k.slot = static_cast<u8>(used[arr]++);
            k.name = intern(name);
            keys.push_back(k);
            FixedString<64> n;
            n.assign(name);
            key_names.push_back(n);
            if (def.valid()) {
                const u16 idx = static_cast<u16>(keys.size() - 1);
                if (k.type == BtKeyType::Entity) error(def, "an entity key has no default");
                else if (k.type == BtKeyType::Vec3 ? vec3(def, keys.back().value) : number_for(idx, def, keys.back().value[0]))
                    keys.back().has_default = 1;
            }
        }
    }

    const Kind* kind_of(JsonValue node) {
        if (!node.is_object()) {
            error(node, "a node must be an object with one kind key");
            return nullptr;
        }
        const Kind* found = nullptr;
        for (u32 i = 0; i < node.size(); ++i)
            for (const Kind& k : kKinds)
                if (node.key_at(i) == k.key) {
                    if (found) {
                        error(node, "a node has exactly one kind key");
                        return nullptr;
                    }
                    found = &k;
                }
        if (!found) {
            error(node,
                  "unknown node: expected sequence, selector, parallel, invert, succeed, fail, repeat, retry, cooldown, timeout, "
                  "guard, check, wait, set, emit or task");
            return nullptr;
        }
        for (u32 i = 0; i < node.size(); ++i) {
            const StringView key = node.key_at(i);
            bool ok = key == found->key || key == "name";
            for (const char* e : found->extras) ok |= e && key == e;
            if (!ok) {
                String msg;
                appendf(msg, "'%.*s' is not a key of a %s node", static_cast<int>(key.size()), key.data(), found->key);
                error(node.get(key), msg);
                return nullptr;
            }
        }
        return found;
    }

    // Check / Guard: the condition from "op" and "value" against key `n.key`.
    void condition(JsonValue node, BtNode& n) {
        const JsonValue op = node.get("op"), value = node.get("value");
        BtOp o = value.valid() ? BtOp::Eq : BtOp::IsSet;
        if (op.valid()) {
            i32 found = -1;
            for (u32 j = 0; j < 8; ++j)
                if (op.is_string() && op.as_string() == kOps[j]) found = static_cast<i32>(j);
            if (found < 0) {
                error(op, "'op' is is_set, not_set, ==, !=, <, <=, > or >=");
                return;
            }
            o = static_cast<BtOp>(found);
        }
        n.op = static_cast<u8>(o);
        if (n.key == asset::kBtNoKey) return;
        if (o == BtOp::IsSet || o == BtOp::NotSet) {
            if (value.valid()) error(value, "is_set and not_set take no 'value'");
            return;
        }
        if (!numeric(n.key)) {
            error(op.valid() ? op : node, "only bool, int and float keys compare; vec3 and entity keys take is_set or not_set");
            return;
        }
        if (!value.valid()) {
            error(node, "a comparison needs a 'value'");
            return;
        }
        number_for(n.key, value, n.a);
    }

    // Cooldown / Timeout: "seconds", required and > 0.
    void seconds(JsonValue node, const char* kind, BtNode& n) {
        const JsonValue v = node.get("seconds");
        if (!v.valid()) {
            String msg;
            appendf(msg, "a %s node needs 'seconds'", kind);
            error(node, msg);
            return;
        }
        if (!v.is_number() || v.as_number() <= 0) {
            error(v, "'seconds' must be a number > 0");
            return;
        }
        n.a = static_cast<f32>(v.as_number());
    }

    // Emits `node` and its subtree in pre-order; returns false on an error that stops the walk.
    bool emit(JsonValue node, u32 depth) {
        if (depth >= asset::kBtMaxDepth) {
            String msg;
            appendf(msg, "the tree is deeper than %u nodes", asset::kBtMaxDepth);
            error(node, msg);
            return false;
        }
        const Kind* k = kind_of(node);
        if (!k) return false;
        if (nodes.size() >= asset::kBtMaxNodes) {
            String msg;
            appendf(msg, "a tree has at most %u nodes", asset::kBtMaxNodes);
            error(node, msg);
            return false;
        }
        const u32 index = static_cast<u32>(nodes.size());
        BtNode n{};
        n.kind = k->kind;
        n.slot = asset::kBtNoSlot;
        n.key = n.key2 = asset::kBtNoKey;
        n.name = n.text = asset::kBtNoString;
        if (const JsonValue name = node.get("name"); name.valid()) {
            if (!name.is_string() || name.as_string().empty()) error(name, "'name' must be a non-empty string");
            else n.name = intern(name.as_string());
        }
        const JsonValue v = node.get(k->key);
        const bool timed = n.kind == BtNodeKind::Repeat || n.kind == BtNodeKind::Retry || n.kind == BtNodeKind::Cooldown ||
                           n.kind == BtNodeKind::Timeout || n.kind == BtNodeKind::Wait || n.kind == BtNodeKind::Task;
        if (timed) {
            if (slots >= asset::kBtMaxSlots) {
                String msg;
                appendf(msg, "a tree has at most %u repeat, retry, cooldown, timeout, wait and task nodes", asset::kBtMaxSlots);
                error(node, msg);
            } else {
                n.slot = static_cast<u8>(slots++);
            }
        }
        switch (n.kind) {
        case BtNodeKind::Parallel:
            if (const JsonValue p = node.get("policy"); p.valid()) {
                if (p.is_string() && p.as_string() == "one") n.op = static_cast<u8>(asset::BtPolicy::One);
                else if (!p.is_string() || p.as_string() != "all") error(p, "'policy' is \"all\" or \"one\"");
            }
            break;
        case BtNodeKind::Repeat:
        case BtNodeKind::Retry:
            if (const JsonValue c = node.get("count"); c.valid()) {
                if (!c.is_integer() || c.as_number() < 0 || c.as_number() > 1e6) error(c, "'count' must be an integer from 0 (forever) up");
                else n.a = static_cast<f32>(c.as_number());
            }
            break;
        case BtNodeKind::Cooldown:
        case BtNodeKind::Timeout: seconds(node, k->key, n); break;
        case BtNodeKind::Guard:
            n.key = key_of(node.get("key"));
            if (!node.get("key").valid()) error(node, "a guard node needs 'key'");
            condition(node, n);
            break;
        case BtNodeKind::Check:
            n.key = key_of(v);
            condition(node, n);
            break;
        case BtNodeKind::Wait:
            if (!v.is_number() || v.as_number() < 0) error(v, "'wait' must be seconds >= 0");
            else n.a = static_cast<f32>(v.as_number());
            break;
        case BtNodeKind::Set: {
            n.key = key_of(v);
            const JsonValue value = node.get("value"), add = node.get("add"), clear = node.get("clear"), from = node.get("from");
            const u32 given = u32(value.valid()) + u32(add.valid()) + u32(clear.valid()) + u32(from.valid());
            if (given != 1) {
                error(node, "a set node takes exactly one of 'value', 'add', 'clear' and 'from'");
                break;
            }
            if (n.key == asset::kBtNoKey) break;
            const BtKeyType t = keys[n.key].type;
            if (value.valid()) {
                if (t == BtKeyType::Entity) {
                    if (value.is_string() && value.as_string() == "self") n.op = static_cast<u8>(BtSetOp::Self);
                    else error(value, "an entity key's value is \"self\" (the running entity)");
                } else if (t == BtKeyType::Vec3) {
                    vec3(value, n.v);
                } else {
                    number_for(n.key, value, n.a);
                }
            } else if (add.valid()) {
                n.op = static_cast<u8>(BtSetOp::Add);
                if (!numeric(n.key) || t == BtKeyType::Bool) error(add, "'add' is for int and float keys");
                else number_for(n.key, add, n.a);
            } else if (clear.valid()) {
                n.op = static_cast<u8>(BtSetOp::Clear);
                if (!clear.is_bool() || !clear.as_bool()) error(clear, "expected true");
            } else {
                n.op = static_cast<u8>(BtSetOp::Copy);
                n.key2 = key_of(from);
                if (n.key2 != asset::kBtNoKey && keys[n.key2].type != t) error(from, "'from' must name a key of the same type");
            }
            break;
        }
        case BtNodeKind::Emit:
        case BtNodeKind::Task:
            if (!v.is_string() || v.as_string().empty()) {
                error(v, n.kind == BtNodeKind::Emit ? "'emit' names a signal" : "'task' names a registered task");
                break;
            }
            n.id = StringId(v.as_string()).value;
            n.text = intern(v.as_string());
            if (n.kind == BtNodeKind::Task) {
                if (const JsonValue key = node.get("key"); key.valid()) n.key = key_of(key);
                if (const JsonValue val = node.get("value"); val.valid()) {
                    if (!val.is_number()) error(val, "a task's 'value' is a number");
                    else n.a = static_cast<f32>(val.as_number());
                }
                if (const JsonValue vec = node.get("vector"); vec.valid()) vec3(vec, n.v);
            }
            break;
        default: break;
        }
        nodes.push_back(n);

        // Children: composites take an array, decorators one node.
        u32 children = 0;
        if (n.kind <= BtNodeKind::Parallel) {
            if (!v.is_array() || v.size() == 0) {
                String msg;
                appendf(msg, "'%s' must be an array of 1 to %u nodes", k->key, asset::kBtMaxChildren);
                error(v, msg);
                return false;
            }
            if (v.size() > asset::kBtMaxChildren) {
                String msg;
                appendf(msg, "a node has at most %u children", asset::kBtMaxChildren);
                error(v, msg);
                return false;
            }
            for (u32 i = 0; i < v.size(); ++i) {
                if (!emit(v.at(i), depth + 1)) return false;
                ++children;
            }
        } else if (n.kind <= BtNodeKind::Guard) {
            if (!emit(v, depth + 1)) return false;
            children = 1;
        }
        nodes[index].child_count = static_cast<u8>(children);
        nodes[index].end = static_cast<u16>(nodes.size());
        return true;
    }
};

class BtImporter final : public IImporter {
public:
    StringView id() const override { return "bt"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::BehaviorTree; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ".mcbt"; }

    void scan_dependencies(const ImportInput&, Array<DepRef>&, DiagList&) const override {}

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue doc = in.settings;
        const StringView file = in.display_path;
        const u32 errors_before = diags.error_count();
        if (!detail::check_keys(doc, {"format", "version", "guid", "name", "blackboard", "root"}, file, kStage, diags)) return false;
        const JsonValue fmt = doc.get("format");
        if (!fmt.is_string() || fmt.as_string() != "mce-bt") {
            detail::error_at(diags, Errc::ParseError, file, fmt.valid() ? fmt : doc, kStage, "\"format\" must be \"mce-bt\"");
            return false;
        }
        const JsonValue ver = doc.get("version");
        if (!ver.is_integer() || ver.as_number() != kBtFormatVersion) {
            String msg;
            appendf(msg, "unsupported behavior tree version (this cook reads version %u)", kBtFormatVersion);
            detail::error_at(diags, Errc::VersionTooNew, file, ver.valid() ? ver : doc, kStage, msg);
            return false;
        }
        asset::BtHeader header{};
        if (const JsonValue name = doc.get("name"); name.valid()) {
            if (!name.is_string() || name.as_string().size() >= sizeof(header.name))
                detail::error_at(diags, Errc::ParseError, file, name, kStage, "'name' must be a string of at most 31 bytes");
            else std::memcpy(header.name, name.as_string().data(), name.as_string().size());
        }
        Builder b{file, diags, {}, {}, {}, {}, 0};
        b.read_blackboard(doc.get("blackboard"));
        const JsonValue root = doc.get("root");
        if (!root.valid()) {
            detail::error_at(diags, Errc::ParseError, file, doc, kStage, "a behavior tree needs a 'root' node");
            return false;
        }
        b.emit(root, 0);
        if (diags.error_count() != errors_before) return false;

        header.slot_count = b.slots;
        Array<byte> payload;
        asset::write_behavior_tree(header, b.keys.span(), b.nodes.span(), b.strings.span(), payload);
        if (Result<asset::BtView> check = asset::BtView::read(payload.span()); !check) {
            detail::error_at(diags, Errc::InvalidArgument, file, doc, kStage, "internal: the cooked tree does not read back");
            return false;
        }
        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::BehaviorTree;
        d.version = asset::chunk_version(asset::BEHAVIOR_TREE_VERSION, 0);
        d.guid = in.guid;
        out.add(d, payload.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_bt_importer() { return mem::make_unique<BtImporter>(mem::heap()); }

}  // namespace mce::cook
