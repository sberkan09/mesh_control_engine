// 2D skeleton authoring (ROADMAP M-ANIM2D step 5; skeleton2d_editor.h).

#include "mce/editor/skeleton2d_editor.h"

#include <cmath>

#include "mce/asset/container.h"
#include "mce/asset/formats.h"
#include "mce/cook/fs.h"
#include "mce/cook/json.h"
#include "mce/ecs/transform.h"
#include "mce/editor/editor_scene.h"

namespace mce::editor {

namespace {

constexpr const char* kFormat = "mce-skeleton2d";
constexpr f64 kSameTime = 1e-6;  // keys closer than this sit at the same time
constexpr u32 kNone = ~0u;

// A mutable JSON tree over a node pool (handles are indices: a node's storage moves when the pool
// grows). Objects keep their member order; replacing a member keeps its place.
class Tree {
public:
    enum Kind : u8 { Null, Bool, Num, Str, Arr, Obj };

    u32 null() { return add(Null); }
    u32 boolean(bool b) {
        const u32 n = add(Bool);
        nodes_[n].b = b;
        return n;
    }
    u32 num(f64 v) {
        const u32 n = add(Num);
        nodes_[n].num = v;
        return n;
    }
    u32 str(StringView s) {
        const u32 n = add(Str);
        nodes_[n].str = cook::String(s);
        return n;
    }
    u32 arr() { return add(Arr); }
    u32 obj() { return add(Obj); }
    u32 nums(const f64* v, u32 count) {
        const u32 a = arr();
        for (u32 i = 0; i < count; ++i) push(a, num(v[i]));
        return a;
    }

    u32 from(cook::JsonValue v) {
        switch (v.type()) {
            case cook::JsonType::Bool:
                return boolean(v.as_bool());
            case cook::JsonType::Number:
                return num(v.as_number());
            case cook::JsonType::String:
                return str(v.as_string());
            case cook::JsonType::Array: {
                const u32 a = arr();
                for (u32 i = 0; i < v.size(); ++i) push(a, from(v.at(i)));
                return a;
            }
            case cook::JsonType::Object: {
                const u32 o = obj();
                for (u32 i = 0; i < v.size(); ++i) set(o, v.key_at(i), from(v.at(i)));
                return o;
            }
            case cook::JsonType::Null:
                break;
        }
        return null();
    }

    Kind kind(u32 n) const { return nodes_[n].kind; }
    u32 size(u32 n) const { return static_cast<u32>(nodes_[n].kids.size()); }
    u32 at(u32 n, u32 i) const { return nodes_[n].kids[i]; }
    StringView key_at(u32 n, u32 i) const { return nodes_[n].keys[i].view(); }
    f64 number(u32 n) const { return nodes_[n].num; }
    StringView string(u32 n) const { return nodes_[n].str.view(); }

    u32 get(u32 o, StringView key) const {
        if (nodes_[o].kind != Obj) return kNone;
        for (u32 i = 0; i < size(o); ++i)
            if (nodes_[o].keys[i].view() == key) return nodes_[o].kids[i];
        return kNone;
    }
    void set(u32 o, StringView key, u32 value) {
        for (u32 i = 0; i < size(o); ++i)
            if (nodes_[o].keys[i].view() == key) {
                nodes_[o].kids[i] = value;
                return;
            }
        nodes_[o].keys.push_back(cook::String(key));
        nodes_[o].kids.push_back(value);
    }
    void push(u32 a, u32 value) { nodes_[a].kids.push_back(value); }
    void insert(u32 a, u32 index, u32 value) {
        nodes_[a].kids.push_back(value);
        for (u32 i = size(a) - 1; i > index; --i) {
            const u32 t = nodes_[a].kids[i];
            nodes_[a].kids[i] = nodes_[a].kids[i - 1];
            nodes_[a].kids[i - 1] = t;
        }
    }
    // The member `key` of object `o` as an object / array, made when absent.
    u32 child(u32 o, StringView key, Kind kind) {
        u32 c = get(o, key);
        if (c == kNone || nodes_[c].kind != kind) {
            c = add(kind);
            set(o, key, c);
        }
        return c;
    }

    void write(u32 n, cook::String& out) const {
        const Node& nd = nodes_[n];
        switch (nd.kind) {
            case Null:
                out += "null";
                break;
            case Bool:
                out += nd.b ? "true" : "false";
                break;
            case Num:
                cook::append_json_number(out, nd.num);
                break;
            case Str:
                cook::append_json_string(out, nd.str.view());
                break;
            case Arr:
                out += '[';
                for (u32 i = 0; i < size(n); ++i) {
                    if (i) out += ',';
                    write(nd.kids[i], out);
                }
                out += ']';
                break;
            case Obj:
                out += '{';
                for (u32 i = 0; i < size(n); ++i) {
                    if (i) out += ',';
                    cook::append_json_string(out, nd.keys[i].view());
                    out += ':';
                    write(nd.kids[i], out);
                }
                out += '}';
                break;
        }
    }

private:
    struct Node {
        Kind kind = Null;
        bool b = false;
        f64 num = 0.0;
        cook::String str;
        Array<cook::String> keys;
        Array<u32> kids;
    };
    u32 add(Kind k) {
        nodes_.emplace_back();
        nodes_.back().kind = k;
        return static_cast<u32>(nodes_.size() - 1);
    }
    Array<Node> nodes_;
};

// The text's tree and its root object.
bool parse_tree(StringView text, Tree& tree, u32& root, cook::String& err) {
    cook::JsonDoc doc;
    cook::JsonError jerr;
    if (!doc.parse(text, jerr) || !doc.root().is_object()) {
        err = cook::String("the skeleton is not a JSON object");
        return false;
    }
    root = tree.from(doc.root());
    return true;
}

// `tree` rewritten indented, as the importers' examples are, with a final newline.
void emit(const Tree& tree, u32 root, cook::String& out) {
    cook::String compact;
    tree.write(root, compact);
    cook::JsonDoc doc;
    cook::JsonError jerr;
    doc.parse(compact.view(), jerr);
    out.clear();
    cook::write_json(doc.root(), out, false);
    out += '\n';
}

// The list `key` of the root, made when absent; false with `err` when the member is not a list.
bool list_of(Tree& t, u32 root, StringView key, u32& out, cook::String& err) {
    const u32 existing = t.get(root, key);
    if (existing != kNone && t.kind(existing) != Tree::Arr) {
        err.clear();
        err += "'";
        err += key;
        err += "' is not a list";
        return false;
    }
    out = t.child(root, key, Tree::Arr);
    return true;
}

bool name_ok(StringView what, StringView name, cook::String& err) {
    if (!name.empty()) return true;
    err.clear();
    err += "a ";
    err += what;
    err += " needs a name";
    return false;
}

u32 find_named(const Tree& t, u32 list, StringView name) {
    for (u32 i = 0; i < t.size(list); ++i) {
        const u32 n = t.get(t.at(list, i), "name");
        if (n != kNone && t.kind(n) == Tree::Str && t.string(n) == name) return i;
    }
    return kNone;
}

f64 time_of(const Tree& t, u32 key) {
    const u32 n = t.get(key, "t");
    return n != kNone && t.kind(n) == Tree::Num ? t.number(n) : 0.0;
}

}  // namespace

// ---------------------------------------------------------------- the document edits

namespace skel2d {

cook::String new_text(asset::Guid guid, StringView name, StringView root_bone) {
    Tree t;
    const u32 root = t.obj();
    t.set(root, "format", t.str(kFormat));
    t.set(root, "version", t.num(1));
    t.set(root, "guid", t.str(asset::to_string(guid).view()));
    t.set(root, "name", t.str(name.substr(0, 31)));
    const u32 bones = t.arr();
    const u32 bone = t.obj();
    t.set(bone, "name", t.str(root_bone));
    t.push(bones, bone);
    t.set(root, "bones", bones);
    cook::String out;
    emit(t, root, out);
    return out;
}

bool add_bone(cook::String& text, const BoneSpec& b, cook::String& err) {
    Tree t;
    u32 root = 0, bones = 0;
    if (!parse_tree(text.view(), t, root, err) || !name_ok("bone", b.name.view(), err) ||
        !list_of(t, root, "bones", bones, err))
        return false;
    const u32 o = t.obj();
    t.set(o, "name", t.str(b.name.view()));
    if (!b.parent.empty()) t.set(o, "parent", t.str(b.parent.view()));
    t.set(o, "x", t.num(b.x));
    t.set(o, "y", t.num(b.y));
    t.set(o, "rotation", t.num(b.rotation));
    const f64 scale[2] = {b.scale_x, b.scale_y};
    t.set(o, "scale", t.nums(scale, 2));
    t.set(o, "length", t.num(b.length));
    t.push(bones, o);
    emit(t, root, text);
    return true;
}

bool set_bone_rest(cook::String& text, StringView bone, f64 x, f64 y, f64 rotation, f64 scale_x, f64 scale_y,
                   cook::String& err) {
    Tree t;
    u32 root = 0, bones = 0;
    if (!parse_tree(text.view(), t, root, err) || !list_of(t, root, "bones", bones, err)) return false;
    const u32 i = find_named(t, bones, bone);
    if (i == kNone) {
        err.clear();
        err += "no bone named '";
        err += bone;
        err += "'";
        return false;
    }
    const u32 o = t.at(bones, i);
    t.set(o, "x", t.num(x));
    t.set(o, "y", t.num(y));
    t.set(o, "rotation", t.num(rotation));
    const f64 scale[2] = {scale_x, scale_y};
    t.set(o, "scale", t.nums(scale, 2));
    emit(t, root, text);
    return true;
}

bool add_slot(cook::String& text, const SlotSpec& s, cook::String& err) {
    Tree t;
    u32 root = 0, slots = 0;
    if (!parse_tree(text.view(), t, root, err) || !name_ok("slot", s.name.view(), err) ||
        !list_of(t, root, "slots", slots, err))
        return false;
    const u32 o = t.obj();
    t.set(o, "name", t.str(s.name.view()));
    t.set(o, "bone", t.str(s.bone.view()));
    if (!s.attachments_json.empty()) {
        cook::JsonDoc doc;
        cook::JsonError jerr;
        if (!doc.parse(s.attachments_json.view(), jerr) || !doc.root().is_array() || doc.root().size() == 0) {
            err = cook::String("'attachments' must be a JSON array of one or more attachment objects");
            return false;
        }
        t.set(o, "tint", t.nums(s.tint, 4));
        if (s.attachment_null)
            t.set(o, "attachment", t.null());
        else if (!s.attachment.empty())
            t.set(o, "attachment", t.str(s.attachment.view()));
        t.set(o, "attachments", t.from(doc.root()));
    } else {
        t.set(o, "texture", s.texture.empty() ? t.null() : t.str(s.texture.view()));
        t.set(o, "rect", t.nums(s.rect, 4));
        t.set(o, "size", t.nums(s.size, 2));
        t.set(o, "pivot", t.nums(s.pivot, 2));
        t.set(o, "offset", t.nums(s.offset, 2));
        if (s.rotation != 0.0) t.set(o, "rotation", t.num(s.rotation));
        t.set(o, "tint", t.nums(s.tint, 4));
    }
    t.push(slots, o);
    emit(t, root, text);
    return true;
}

bool add_ik(cook::String& text, const IkSpec& ik, cook::String& err) {
    Tree t;
    u32 root = 0, list = 0;
    if (!parse_tree(text.view(), t, root, err) || !name_ok("constraint", ik.name.view(), err) ||
        !list_of(t, root, "ik", list, err))
        return false;
    const u32 o = t.obj();
    t.set(o, "name", t.str(ik.name.view()));
    const u32 chain = t.arr();
    for (const cook::String& b : ik.bones) t.push(chain, t.str(b.view()));
    t.set(o, "bones", chain);
    if (!ik.target.empty()) t.set(o, "target", t.str(ik.target.view()));
    t.set(o, "mix", t.num(ik.mix));
    if (!ik.type.empty()) t.set(o, "type", t.str(ik.type.view()));
    if (!ik.bend.empty()) t.set(o, "bend", t.str(ik.bend.view()));
    if (ik.iterations != 0) t.set(o, "iterations", t.num(ik.iterations));
    if (ik.order != 0) t.set(o, "order", t.num(ik.order));
    t.push(list, o);
    emit(t, root, text);
    return true;
}

bool key(cook::String& text, const KeySpec& k, cook::String& err) {
    Tree t;
    u32 root = 0, anims = 0;
    if (!parse_tree(text.view(), t, root, err) || !name_ok("animation", k.animation.view(), err) ||
        !name_ok("key target", k.target.view(), err) || !list_of(t, root, "animations", anims, err))
        return false;
    u32 ai = find_named(t, anims, k.animation.view());
    if (ai == kNone) {
        const u32 a = t.obj();
        t.set(a, "name", t.str(k.animation.view()));
        t.push(anims, a);
        ai = t.size(anims) - 1;
    }
    const u32 anim = t.at(anims, ai);
    if (k.has_duration) t.set(anim, "duration", t.num(k.duration));
    if (k.has_loop) t.set(anim, "loop", t.boolean(k.loop));

    // The key object and the list it belongs in.
    const u32 key = t.obj();
    t.set(key, "t", t.num(k.t));
    u32 holder = 0;
    StringView channel;
    switch (k.channel) {
        case Channel::Rotate:
        case Channel::Translate:
        case Channel::Scale: {
            holder = t.child(t.child(anim, "bones", Tree::Obj), k.target.view(), Tree::Obj);
            channel = k.channel == Channel::Rotate ? "rotate" : k.channel == Channel::Translate ? "translate" : "scale";
            if (k.channel == Channel::Rotate) {
                t.set(key, "value", t.num(k.value));
            } else {
                t.set(key, "x", t.num(k.x));
                t.set(key, "y", t.num(k.y));
            }
            break;
        }
        case Channel::SlotColor:
            holder = t.child(t.child(anim, "slots", Tree::Obj), k.target.view(), Tree::Obj);
            channel = "color";
            t.set(key, "color", t.nums(k.color, 4));
            break;
        case Channel::SlotAttachment:
            holder = t.child(t.child(anim, "slots", Tree::Obj), k.target.view(), Tree::Obj);
            channel = "attachment";
            t.set(key, "name", k.attachment_null ? t.null() : t.str(k.attachment.view()));
            break;
        case Channel::IkMix:
            holder = t.child(anim, "ik", Tree::Obj);
            channel = k.target.view();
            t.set(key, "mix", t.num(k.value));
            break;
    }
    if (k.channel != Channel::SlotAttachment) {  // attachment keys are stepped and take no curve
        if (k.bezier)
            t.set(key, "curve", t.nums(k.bezier_c, 4));
        else if (!k.curve.empty())
            t.set(key, "curve", t.str(k.curve.view()));
    }
    const u32 keys = t.child(holder, channel, Tree::Arr);

    // By time: a key at the same time is replaced, else the key goes before the first later one.
    u32 at = t.size(keys);
    bool replace = false;
    for (u32 i = 0; i < t.size(keys); ++i) {
        const f64 ti = time_of(t, t.at(keys, i));
        if (std::fabs(ti - k.t) < kSameTime) {
            at = i;
            replace = true;
            break;
        }
        if (ti > k.t) {
            at = i;
            break;
        }
    }
    if (replace) {
        // Rebuild the list without the replaced key (handles of the others stay valid).
        const u32 fresh = t.arr();
        for (u32 i = 0; i < t.size(keys); ++i) t.push(fresh, i == at ? key : t.at(keys, i));
        t.set(holder, channel, fresh);
    } else {
        t.insert(keys, at, key);
    }
    emit(t, root, text);
    return true;
}

}  // namespace skel2d

// ---------------------------------------------------------------- reading a document

bool Skeleton2DInfo::read(StringView text, Skeleton2DInfo& out, cook::String& err) {
    cook::JsonDoc doc;
    cook::JsonError jerr;
    if (!doc.parse(text, jerr) || !doc.root().is_object()) {
        err = cook::String("the skeleton is not a JSON object");
        return false;
    }
    out = Skeleton2DInfo();
    const cook::JsonValue r = doc.root();
    auto num = [](cook::JsonValue o, StringView k, f64 def) {
        const cook::JsonValue v = o.get(k);
        return v.is_number() ? v.as_number() : def;
    };
    auto str = [](cook::JsonValue o, StringView k) {
        const cook::JsonValue v = o.get(k);
        return v.is_string() ? cook::String(v.as_string()) : cook::String();
    };
    out.title = str(r, "name");
    const cook::JsonValue bones = r.get("bones");
    if (!bones.is_array()) {
        err = cook::String("the skeleton has no 'bones' list");
        return false;
    }
    for (u32 i = 0; i < bones.size(); ++i) {
        const cook::JsonValue b = bones.at(i);
        if (!b.is_object()) continue;
        Bone bone;
        bone.name = str(b, "name");
        const cook::String parent = str(b, "parent");
        bone.parent = parent.empty() ? -1 : out.find_bone(parent.view());
        bone.x = num(b, "x", 0);
        bone.y = num(b, "y", 0);
        bone.rotation = num(b, "rotation", 0);
        bone.length = num(b, "length", 0);
        const cook::JsonValue s = b.get("scale");
        if (s.is_number()) {
            bone.scale_x = bone.scale_y = s.as_number();
        } else if (s.is_array() && s.size() == 2 && s.at(0).is_number() && s.at(1).is_number()) {
            bone.scale_x = s.at(0).as_number();
            bone.scale_y = s.at(1).as_number();
        }
        out.bones.push_back(static_cast<Bone&&>(bone));
    }
    const cook::JsonValue slots = r.get("slots");
    for (u32 i = 0; slots.is_array() && i < slots.size(); ++i) {
        const cook::JsonValue s = slots.at(i);
        if (!s.is_object()) continue;
        Slot slot;
        slot.name = str(s, "name");
        slot.bone = str(s, "bone");
        const cook::JsonValue att = s.get("attachments");
        slot.attachments = att.is_array() ? att.size() : 1;
        out.slots.push_back(static_cast<Slot&&>(slot));
    }
    const cook::JsonValue anims = r.get("animations");
    for (u32 i = 0; anims.is_array() && i < anims.size(); ++i) {
        const cook::JsonValue a = anims.at(i);
        if (!a.is_object()) continue;
        Animation an;
        an.name = str(a, "name");
        an.duration = num(a, "duration", 0);
        an.loop = !a.get("loop").is_bool() || a.get("loop").as_bool();
        out.animations.push_back(static_cast<Animation&&>(an));
    }
    const cook::JsonValue ik = r.get("ik");
    for (u32 i = 0; ik.is_array() && i < ik.size(); ++i)
        if (ik.at(i).is_object()) out.ik.push_back(str(ik.at(i), "name"));
    return true;
}

i32 Skeleton2DInfo::find_bone(StringView name) const {
    for (usize i = 0; i < bones.size(); ++i)
        if (bones[i].name.view() == name) return static_cast<i32>(i);
    return -1;
}

i32 Skeleton2DInfo::find_animation(StringView name) const {
    for (usize i = 0; i < animations.size(); ++i)
        if (animations[i].name.view() == name) return static_cast<i32>(i);
    return -1;
}

// ---------------------------------------------------------------- cook and pose

bool cook_skeleton2d(const EditorProject& project, const cook::AssetRegistry& registry, asset::Guid guid,
                     StringView rel, StringView text, kit::Skeleton2DRegistry& defs, cook::DiagList& diags) {
    const cook::IImporter* importer = project.importers().for_extension(".mcskel2d");
    if (!importer) {
        diags.error(Errc::InvalidArgument, rel, 0, 0, "editor.skeleton2d", "no importer reads .mcskel2d");
        return false;
    }
    Array<byte> container;
    if (!cook::cook_document(registry, *importer, guid, rel, text, container, diags)) return false;
    Result<asset::ContainerView> view = asset::ContainerView::open(container.span(), rel);
    if (!view) {
        diags.error(view.error().code, rel, 0, 0, "editor.skeleton2d", "the cooked skeleton does not open");
        return false;
    }
    const u32 chunk = view.value().find(asset::ChunkType::Skeleton2D);
    if (chunk >= view.value().chunk_count()) {
        diags.error(Errc::ImportFailed, rel, 0, 0, "editor.skeleton2d", "the cook wrote no skeleton chunk");
        return false;
    }
    if (Result<void> r = defs.define(guid, view.value().payload(chunk)); !r) {
        diags.error(r.error().code, rel, 0, 0, "editor.skeleton2d", "the cooked skeleton does not read");
        return false;
    }
    return true;
}

bool skeleton2d_pose(const kit::Skeleton2DRegistry::Def& def, StringView animation, f32 time,
                     Span<const Skeleton2DIKTarget> targets, Array<kit::Bone2DTransform>& world, cook::String& err) {
    const u32 n = static_cast<u32>(def.bones.size());
    kit::Bone2DPose offsets[kit::kSkeleton2DBones];
    kit::Skeleton2DIK ik;
    kit::skeleton2d_ik_setup(def, ik);
    if (!animation.empty()) {
        i32 index = -1;
        for (usize i = 0; i < def.animations.size(); ++i)
            if (def.string(def.animations[i].name) == animation) index = static_cast<i32>(i);
        if (index < 0) {
            err.clear();
            err += "no animation named '";
            err += animation;
            err += "'";
            return false;
        }
        kit::Skeleton2DBlend blend;
        blend.add(def, static_cast<u32>(index), time, 1.0f);
        blend.write(n, offsets);
        blend.write_ik(def, ik);
    }
    kit::Skeleton2DIKTargets tg;
    for (const Skeleton2DIKTarget& t : targets) {
        const i32 c = def.find_ik(t.constraint.view());
        if (c < 0) {
            err.clear();
            err += "no IK constraint named '";
            err += t.constraint.view();
            err += "'";
            return false;
        }
        tg.position[c] = t.position;
        tg.mix[c] = t.mix;
        tg.set |= 1u << static_cast<u32>(c);
    }
    world.resize(n);
    kit::skeleton2d_solve_pose(def, Span<const kit::Bone2DPose>(offsets, n), &ik, &tg,
                               Span<kit::Bone2DTransform>(world.data(), n));
    return true;
}

void skeleton2d_bone_lines(const kit::Skeleton2DRegistry::Def& def, Span<const kit::Bone2DTransform> world,
                           const kit::Bone2DTransform& entity, i32 selected, Array<OverlayLine>& out) {
    const usize n = def.bones.size() < world.size() ? def.bones.size() : world.size();
    for (usize i = 0; i < n; ++i) {
        const kit::Bone2DTransform m = entity * world[i];
        const f32 len = def.bones[i].length > 0.0f ? def.bones[i].length : 0.1f;
        const bool sel = static_cast<i32>(i) == selected;
        const math::Color c = sel ? math::Color{1.0f, 0.62f, 0.1f, 1.0f} : math::Color{0.35f, 0.85f, 1.0f, 0.9f};
        out.push_back({m.apply({0.0f, 0.0f}), m.apply({len, 0.0f}), c, sel ? 3.0f : 2.0f});
        // A small tick across the origin marks the joint.
        out.push_back({m.apply({0.0f, -0.03f}), m.apply({0.0f, 0.03f}), c, 1.5f});
    }
}

bool skeleton2d_entity_lines(Skeleton2DEditor& ed, const EditorScene& scene, EntityId id, Array<OverlayLine>& out) {
    if (!ed.is_open()) return false;
    const kit::Skeleton2D* sk = scene.get<kit::Skeleton2D>(id);
    if (!sk || sk->skeleton.guid != ed.guid() || !scene.get<LocalTransform>(id)) return false;
    const kit::Skeleton2DRegistry::Def* def = ed.def();
    const Span<const kit::Bone2DTransform> pose = ed.pose();
    if (!def || pose.empty()) return false;
    const WorldTransform t = scene.world_transform(id);
    const kit::Bone2DTransform entity =
        kit::Bone2DTransform::trs({t.position.x, t.position.y}, angle_z(t.rotation),
                                  {sk->flip_x ? -t.scale.x : t.scale.x, sk->flip_y ? -t.scale.y : t.scale.y});
    skeleton2d_bone_lines(*def, pose, entity,
                          ed.selected_bone() == kNoSkeleton2DBone ? -1 : static_cast<i32>(ed.selected_bone()), out);
    return true;
}

// ---------------------------------------------------------------- the editor

bool Skeleton2DEditor::load(StringView text) {
    Skeleton2DInfo info;
    cook::String why;
    if (!Skeleton2DInfo::read(text, info, why)) {
        error_ = why;
        return false;
    }
    cook::DiagList diags;
    kit::Skeleton2DRegistry fresh;
    if (!cook_skeleton2d(*project_, registry_, guid_, rel_.view(), text, fresh, diags)) {
        error_ = cook::String();
        u32 shown = 0;
        for (const cook::Diagnostic& d : diags.all()) {
            if (d.severity != cook::Severity::Error) continue;
            if (shown++ == 3) break;
            if (!error_.empty()) error_ += "; ";
            cook::format_diagnostic(d, error_);
        }
        if (error_.empty()) error_ = cook::String("the skeleton does not cook");
        return false;
    }
    defs_ = static_cast<kit::Skeleton2DRegistry&&>(fresh);
    info_ = static_cast<Skeleton2DInfo&&>(info);
    text_ = cook::String(text);
    if (bone_ >= info_.bones.size()) bone_ = kNoSkeleton2DBone;
    if (!animation_.empty() && info_.find_animation(animation_.view()) < 0) animation_ = cook::String();
    ++revision_;
    return true;
}

bool Skeleton2DEditor::open(EditorSession& session, asset::Guid guid) {
    if (guid == guid_ && dirty_) return true;
    const ProjectAsset* a = session.project().find(guid);
    if (!a || a->type != asset::ChunkType::Skeleton2D) {
        error_ = cook::String("not a 2D skeleton (.mcskel2d)");
        return false;
    }
    cook::String file;
    if (!cook::fs::read_text(session.project().asset_path(a->path.view()).view(), file)) {
        error_ = cook::String("the skeleton could not be read");
        return false;
    }
    const cook::String rel_before = rel_;
    const asset::Guid guid_before = guid_;
    rel_ = a->path;
    guid_ = guid;
    project_ = &session.project();
    cook::DiagList scan;
    registry_ = cook::AssetRegistry();
    registry_.scan(session.project().root(), session.project().importers(), false, scan);
    if (!load(file.view())) {
        rel_ = rel_before;
        guid_ = guid_before;
        return false;
    }
    if (guid != guid_before) {
        undo_.clear();
        redo_.clear();
        bone_ = kNoSkeleton2DBone;
        animation_ = cook::String();
        time_ = 0.0f;
        targets_.clear();
    }
    disk_ = file;
    dirty_ = false;
    error_ = cook::String();
    return true;
}

void Skeleton2DEditor::close() {
    guid_ = {};
    rel_ = cook::String();
    text_ = cook::String();
    disk_ = cook::String();
    info_ = Skeleton2DInfo();
    defs_ = kit::Skeleton2DRegistry();
    project_ = nullptr;
    bone_ = kNoSkeleton2DBone;
    animation_ = cook::String();
    time_ = 0.0f;
    targets_.clear();
    pose_.clear();
    dirty_ = false;
    undo_.clear();
    redo_.clear();
    ++revision_;
}

Result<asset::Guid> Skeleton2DEditor::create(EditorSession& session, StringView rel_path, StringView root_bone) {
    constexpr const char* kWhere = "editor.skeleton2d_create";
    if (!session.project().is_open()) return Error(Errc::InvalidArgument, kWhere, "no project is open");
    if (rel_path.size() < 10 || rel_path.substr(rel_path.size() - 9) != ".mcskel2d")
        return Error(Errc::InvalidArgument, kWhere, "a skeleton's path ends in .mcskel2d");
    if (cook::fs::exists(session.project().asset_path(rel_path).view()))
        return Error(Errc::AlreadyExists, kWhere, rel_path);
    const asset::Guid guid = cook::generate_guid();
    StringView stem = rel_path.substr(0, rel_path.size() - 9);
    const usize slash = stem.rfind('/');
    if (slash != StringView::npos) stem = stem.substr(slash + 1);
    const cook::String t = skel2d::new_text(guid, stem, root_bone);
    MCE_TRY(session.write_asset(rel_path, t.view()));
    if (!open(session, guid)) return Error(Errc::ImportFailed, kWhere, "the new skeleton did not cook (see the log)");
    return guid;
}

bool Skeleton2DEditor::sync(EditorSession& session) {
    if (!is_open() || dirty_) return false;
    const ProjectAsset* a = session.project().find(guid_);
    if (!a) {  // deleted (or moved away from the catalog)
        close();
        return true;
    }
    cook::String file;
    if (!cook::fs::read_text(session.project().asset_path(a->path.view()).view(), file)) return false;
    if (file.view() == disk_.view() && a->path.view() == rel_.view()) return false;
    rel_ = a->path;
    disk_ = file;
    cook::DiagList scan;
    registry_ = cook::AssetRegistry();
    registry_.scan(session.project().root(), session.project().importers(), false, scan);
    if (!load(file.view())) return false;  // keeps the last good skeleton; error_ says why
    error_ = cook::String();
    return true;
}

void Skeleton2DEditor::set_animation(StringView name) {
    animation_ = info_.find_animation(name) >= 0 ? cook::String(name) : cook::String();
    const f32 len = animation_length();
    if (time_ > len) time_ = len;
}

f32 Skeleton2DEditor::animation_length() {
    const kit::Skeleton2DRegistry::Def* d = def();
    if (!d || animation_.empty()) return 0.0f;
    for (usize i = 0; i < d->animations.size(); ++i)
        if (d->string(d->animations[i].name) == animation_.view()) return d->animations[i].duration;
    return 0.0f;
}

bool Skeleton2DEditor::take(StringView next) {
    if (!is_open()) {
        error_ = cook::String("no skeleton is open");
        return false;
    }
    const cook::String before = text_;
    if (next == before.view()) return true;
    if (!load(next)) return false;
    error_ = cook::String();
    if (!dirty_) {
        undo_.push_back(before);
        if (undo_.size() > kMaxUndo) undo_.erase(0);
    }
    redo_.clear();
    dirty_ = true;
    return true;
}

bool Skeleton2DEditor::apply_text(StringView next) { return take(next); }

bool Skeleton2DEditor::add_bone(const skel2d::BoneSpec& bone) {
    cook::String next = text_, why;
    if (!skel2d::add_bone(next, bone, why)) {
        error_ = why;
        return false;
    }
    if (!take(next.view())) return false;
    bone_ = static_cast<u32>(info_.bones.size() - 1);
    return true;
}

bool Skeleton2DEditor::set_bone_rest(u32 bone, f64 x, f64 y, f64 rotation, f64 scale_x, f64 scale_y) {
    if (bone >= info_.bones.size()) {
        error_ = cook::String("no such bone");
        return false;
    }
    cook::String next = text_, why;
    if (!skel2d::set_bone_rest(next, info_.bones[bone].name.view(), x, y, rotation, scale_x, scale_y, why)) {
        error_ = why;
        return false;
    }
    return take(next.view());
}

bool Skeleton2DEditor::add_slot(const skel2d::SlotSpec& slot) {
    cook::String next = text_, why;
    if (!skel2d::add_slot(next, slot, why)) {
        error_ = why;
        return false;
    }
    return take(next.view());
}

bool Skeleton2DEditor::add_ik(const skel2d::IkSpec& ik) {
    cook::String next = text_, why;
    if (!skel2d::add_ik(next, ik, why)) {
        error_ = why;
        return false;
    }
    return take(next.view());
}

bool Skeleton2DEditor::key(const skel2d::KeySpec& key) {
    cook::String next = text_, why;
    if (!skel2d::key(next, key, why)) {
        error_ = why;
        return false;
    }
    if (!take(next.view())) return false;
    animation_ = key.animation;
    return true;
}

Result<void> Skeleton2DEditor::save(EditorSession& session) {
    if (!dirty_) return ok();
    MCE_TRY(session.write_asset(rel_.view(), text_.view()));
    disk_ = text_;
    dirty_ = false;
    return ok();
}

bool Skeleton2DEditor::undo(EditorSession& session) {
    if (undo_.empty()) return false;
    const cook::String current = text_;
    if (!load(undo_.back().view())) return false;
    redo_.push_back(current);
    undo_.pop_back();
    dirty_ = true;
    return static_cast<bool>(save(session));
}

bool Skeleton2DEditor::redo(EditorSession& session) {
    if (redo_.empty()) return false;
    if (dirty_) {
        if (Result<void> r = save(session); !r) return false;
    }
    const cook::String current = text_;
    if (!load(redo_.back().view())) return false;
    undo_.push_back(current);
    redo_.pop_back();
    dirty_ = true;
    return static_cast<bool>(save(session));
}

const kit::Skeleton2DRegistry::Def* Skeleton2DEditor::def() { return is_open() ? defs_.resolve(guid_) : nullptr; }

Span<const kit::Bone2DTransform> Skeleton2DEditor::pose() {
    const kit::Skeleton2DRegistry::Def* d = def();
    pose_.clear();
    if (!d) return {};
    cook::String why;
    if (!skeleton2d_pose(*d, animation_.view(), time_, targets_.span(), pose_, why)) {
        error_ = why;
        pose_.clear();
    }
    return pose_.span();
}

}  // namespace mce::editor
