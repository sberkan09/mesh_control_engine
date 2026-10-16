// .mcskel2d -> CHNK_SKEL2D: a 2D cut-out skeleton (M-ANIM2D 1). A bone tree and the slots (textured
// quads) drawn on its bones; kit::Skeleton2D (kit/skeleton2d.h) poses and draws it. Slot textures
// are dependencies (a missing or non-texture reference is a cook error naming file:line:col).
//
// {
//   "format": "mce-skeleton2d", "version": 1, "guid": "<uuid>", "name": "hero",
//   "bones": [                                  // parents before children; names unique
//     {"name": "root"},
//     {"name": "torso", "parent": "root", "x": 0, "y": 0.5, "rotation": 0, "scale": [1, 1], "length": 0.6}],
//   "slots": [                                  // draw order: the first is drawn first (behind)
//     {"name": "body", "bone": "torso", "texture": "guid:<texture uuid>" | null,
//      "rect": [0, 0, 1, 1], "size": [0.4, 0.7], "pivot": [0.5, 0.5], "offset": [0, 0.3],
//      "rotation": 0, "tint": [1, 1, 1, 1]}],
//   "animations": [                             // M-ANIM2D 2; names unique
//     {"name": "walk", "duration": 1, "loop": true,
//      "bones": {"torso": {"rotate": [{"t": 0, "value": 0, "curve": "linear"}, {"t": 0.5, "value": 15}],
//                          "translate": [{"t": 0, "x": 0, "y": 0, "curve": [0.25, 0, 0.75, 1]}],
//                          "scale": [{"t": 0, "x": 1, "y": 1, "curve": "stepped"}]}},
//      "events": [{"t": 0.5, "name": "step"}]}]
// }
//
// Metres and degrees in the text, Y up; the chunk stores radians. A bone's x, y and rotation are
// its rest pose in its parent's space; "scale" is [sx, sy] or one number. A slot's offset and
// rotation place the quad's pivot in its bone's space. A null or absent texture draws a solid quad
// in the slot's tint. At most 64 bones (kit::Skeleton2D holds one pose per bone) and 256 slots.
//
// Animation keys are offsets from the rest pose (as Skeleton2D::pose): "value" is degrees added
// to the bone's rotation, translate x, y metres added, scale x, y factors (default 1) multiplying.
// Keys are in increasing time order. A key's "curve" shapes the interval to the next key:
// "linear" (default), "stepped", or a cubic bezier [cx1, cy1, cx2, cy2] in the normalized key
// interval (cx1 and cx2 in 0..1). "duration" defaults to the last key or event time; "loop"
// defaults to true. Event names are 1 to 23 characters.

#include <cmath>
#include <cstdio>
#include <cstring>

#include "importer_common.h"
#include "mce/asset/formats.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.skeleton2d";
constexpr u32 kFormatVersion = 1;
constexpr usize kMaxName = 31;  // kit::Bone2DFollow::bone is a FixedString<32>
constexpr f64 kDegToRad = 3.14159265358979323846 / 180.0;

using asset::Skeleton2DBone;
using asset::Skeleton2DSlot;

struct Reader {
    StringView file;
    DiagList& diags;

    bool fail(JsonValue at, StringView msg) {
        detail::error_at(diags, Errc::ParseError, file, at, kStage, msg);
        return false;
    }

    // A finite number at `key` (absent: `def`).
    bool number(JsonValue obj, StringView key, f64 def, f32& out) {
        const JsonValue v = obj.get(key);
        if (!v.valid()) {
            out = static_cast<f32>(def);
            return true;
        }
        if (!v.is_number() || !std::isfinite(v.as_number())) {
            String msg;
            appendf(msg, "'%.*s' must be a number", static_cast<int>(key.size()), key.data());
            return fail(v, msg.view());
        }
        out = static_cast<f32>(v.as_number());
        return true;
    }

    // An array of `n` finite numbers at `key` (absent: `def`). With `scalar`, one number fills all.
    bool numbers(JsonValue obj, StringView key, u32 n, const f32* def, f32* out, bool scalar = false) {
        const JsonValue v = obj.get(key);
        if (!v.valid()) {
            std::memcpy(out, def, sizeof(f32) * n);
            return true;
        }
        if (scalar && v.is_number() && std::isfinite(v.as_number())) {
            for (u32 i = 0; i < n; ++i) out[i] = static_cast<f32>(v.as_number());
            return true;
        }
        bool ok = v.is_array() && v.size() == n;
        for (u32 i = 0; ok && i < n; ++i) {
            ok = v.at(i).is_number() && std::isfinite(v.at(i).as_number());
            if (ok) out[i] = static_cast<f32>(v.at(i).as_number());
        }
        if (ok) return true;
        String msg;
        appendf(msg, "'%.*s' must be an array of %u numbers", static_cast<int>(key.size()), key.data(), n);
        return fail(v, msg.view());
    }

    bool name(JsonValue obj, const char* what, StringView& out) {
        const JsonValue v = obj.get("name");
        if (!v.is_string() || v.as_string().empty() || v.as_string().size() > kMaxName) {
            String msg;
            appendf(msg, "a %s's 'name' must be a string of 1 to %u characters", what, static_cast<u32>(kMaxName));
            return fail(v.valid() ? v : obj, msg.view());
        }
        out = v.as_string();
        return true;
    }
};

u32 add_string(Array<char>& pool, StringView s) {
    const u32 at = static_cast<u32>(pool.size());
    for (char c : s) pool.push_back(c);
    pool.push_back('\0');
    return at;
}

struct Animations {
    Array<asset::Skeleton2DAnimation> animations;
    Array<asset::Skeleton2DTimeline> timelines;
    Array<asset::Skeleton2DKey> keys;
    Array<asset::Skeleton2DEvent> events;
};

constexpr usize kMaxEventName = 23;  // kit::Skeleton2DAnimator::last_event is a FixedString<24>

// "animation 'walk', bone 'arm': " (bone empty: "animation 'walk': ") before `msg`.
bool anim_fail(Reader& r, JsonValue at, StringView anim, StringView bone, const char* msg) {
    String text;
    appendf(text, "animation '%.*s'", static_cast<int>(anim.size()), anim.data());
    if (!bone.empty()) appendf(text, ", bone '%.*s'", static_cast<int>(bone.size()), bone.data());
    appendf(text, ": %s", msg);
    return r.fail(at, text.view());
}

// A key's "curve": absent or "linear", "stepped", or [cx1, cy1, cx2, cy2] with cx in 0..1.
bool read_curve(Reader& r, JsonValue key, StringView anim, StringView bone, asset::Skeleton2DKey& k) {
    const JsonValue c = key.get("curve");
    k.curve = asset::Skeleton2DCurve::Linear;
    if (!c.valid() || (c.is_string() && c.as_string() == "linear")) return true;
    if (c.is_string() && c.as_string() == "stepped") {
        k.curve = asset::Skeleton2DCurve::Stepped;
        return true;
    }
    bool ok = c.is_array() && c.size() == 4;
    for (u32 i = 0; ok && i < 4; ++i) {
        ok = c.at(i).is_number() && std::isfinite(c.at(i).as_number());
        if (ok) k.bezier[i] = static_cast<f32>(c.at(i).as_number());
    }
    if (!ok)
        return anim_fail(r, c, anim, bone,
                         "a key's 'curve' must be \"linear\", \"stepped\" or a bezier [cx1, cy1, cx2, cy2]");
    if (k.bezier[0] < 0.0f || k.bezier[0] > 1.0f || k.bezier[2] < 0.0f || k.bezier[2] > 1.0f)
        return anim_fail(r, c, anim, bone, "a bezier curve's cx1 and cx2 must be in 0..1");
    k.curve = asset::Skeleton2DCurve::Bezier;
    return true;
}

bool read_animations(Reader& r, JsonValue list, const Array<StringView>& bone_names, Array<char>& pool, Animations& out) {
    if (!list.valid()) return true;
    if (!list.is_array()) return r.fail(list, "'animations' must be an array");
    if (list.size() > asset::kSkeleton2DMaxAnimations) {
        String msg;
        appendf(msg, "a skeleton has at most %u animations", asset::kSkeleton2DMaxAnimations);
        return r.fail(list, msg.view());
    }
    Array<StringView> names;
    for (u32 ai = 0; ai < list.size(); ++ai) {
        const JsonValue a = list.at(ai);
        if (!a.is_object())
            return r.fail(a, "an animation is {\"name\", \"duration\", \"loop\", \"bones\", \"events\"}");
        if (!detail::check_keys(a, {"name", "duration", "loop", "bones", "events"}, r.file, kStage, r.diags)) return false;
        StringView name;
        if (!r.name(a, "animation", name)) return false;
        for (const StringView& other : names)
            if (other == name) return r.fail(a.get("name"), "two animations share this name");
        names.push_back(name);

        asset::Skeleton2DAnimation anim{};
        anim.name = add_string(pool, name);
        anim.first_timeline = static_cast<u32>(out.timelines.size());
        anim.first_event = static_cast<u32>(out.events.size());
        anim.flags = asset::Skeleton2DAnimationFlags::Loop;
        if (const JsonValue l = a.get("loop"); l.valid()) {
            if (!l.is_bool()) return anim_fail(r, l, name, {}, "'loop' must be true or false");
            if (!l.as_bool()) anim.flags = 0;
        }
        f32 last = 0.0f;  // the last key or event time

        const JsonValue bones = a.get("bones");
        if (bones.valid() && !bones.is_object())
            return anim_fail(r, bones, name, {}, "'bones' must be an object of bone name: {\"rotate\", \"translate\", \"scale\"}");
        for (u32 bi = 0; bones.valid() && bi < bones.size(); ++bi) {
            const StringView bone = bones.key_at(bi);
            const JsonValue channels = bones.at(bi);
            i32 found = -1;
            for (u32 j = 0; j < bone_names.size(); ++j)
                if (bone_names[j] == bone) found = static_cast<i32>(j);
            if (found < 0) return anim_fail(r, channels, name, bone, "no bone has that name");
            for (u32 j = 0; j < bi; ++j)
                if (bones.key_at(j) == bone) return anim_fail(r, channels, name, bone, "the bone is listed twice");
            if (!channels.is_object())
                return anim_fail(r, channels, name, bone, "a bone's keys are {\"rotate\", \"translate\", \"scale\"}");
            if (!detail::check_keys(channels, {"rotate", "translate", "scale"}, r.file, kStage, r.diags)) return false;
            static constexpr const char* kChannel[] = {"rotate", "translate", "scale"};
            for (u32 ch = 0; ch < 3; ++ch) {
                const JsonValue keys = channels.get(kChannel[ch]);
                if (!keys.valid()) continue;
                if (!keys.is_array() || keys.size() == 0) {
                    String msg;
                    appendf(msg, "'%s' must be an array of at least one key", kChannel[ch]);
                    return anim_fail(r, keys, name, bone, msg.c_str());
                }
                asset::Skeleton2DTimeline tl{};
                tl.bone = static_cast<u16>(found);
                tl.channel = static_cast<asset::Skeleton2DChannel>(ch);
                tl.first_key = static_cast<u32>(out.keys.size());
                tl.key_count = keys.size();
                if (out.keys.size() + keys.size() > asset::kSkeleton2DMaxKeys)
                    return anim_fail(r, keys, name, bone, "the skeleton has too many keys");
                f32 prev = 0.0f;
                for (u32 ki = 0; ki < keys.size(); ++ki) {
                    const JsonValue key = keys.at(ki);
                    if (!key.is_object()) return anim_fail(r, key, name, bone, "a key is an object with \"t\"");
                    const bool rotate = ch == 0;
                    if (rotate ? !detail::check_keys(key, {"t", "value", "curve"}, r.file, kStage, r.diags)
                               : !detail::check_keys(key, {"t", "x", "y", "curve"}, r.file, kStage, r.diags))
                        return false;
                    asset::Skeleton2DKey k{};
                    if (!key.get("t").valid()) return anim_fail(r, key, name, bone, "a key needs a time 't'");
                    if (!r.number(key, "t", 0.0, k.t)) return false;
                    if (k.t < 0.0f) return anim_fail(r, key.get("t"), name, bone, "a key's 't' must be 0 or more");
                    if (ki > 0 && !(k.t > prev)) {
                        String msg;
                        appendf(msg, "%s keys must be in increasing time order (t %g after %g)", kChannel[ch],
                                static_cast<f64>(k.t), static_cast<f64>(prev));
                        return anim_fail(r, key.get("t"), name, bone, msg.c_str());
                    }
                    prev = k.t;
                    if (rotate) {
                        f32 deg = 0.0f;
                        if (!r.number(key, "value", 0.0, deg)) return false;
                        k.value[0] = static_cast<f32>(deg * kDegToRad);
                    } else {
                        const f64 def = ch == 2 ? 1.0 : 0.0;
                        if (!r.number(key, "x", def, k.value[0]) || !r.number(key, "y", def, k.value[1])) return false;
                    }
                    if (!read_curve(r, key, name, bone, k)) return false;
                    out.keys.push_back(k);
                }
                if (prev > last) last = prev;
                if (out.timelines.size() >= asset::kSkeleton2DMaxTimelines)
                    return anim_fail(r, keys, name, bone, "the skeleton has too many timelines");
                out.timelines.push_back(tl);
                ++anim.timeline_count;
            }
        }

        const JsonValue events = a.get("events");
        if (events.valid() && !events.is_array()) return anim_fail(r, events, name, {}, "'events' must be an array");
        f32 prev = 0.0f;
        for (u32 ei = 0; events.valid() && ei < events.size(); ++ei) {
            const JsonValue ev = events.at(ei);
            if (!ev.is_object()) return anim_fail(r, ev, name, {}, "an event is {\"t\", \"name\"}");
            if (!detail::check_keys(ev, {"t", "name"}, r.file, kStage, r.diags)) return false;
            asset::Skeleton2DEvent e{};
            if (!ev.get("t").valid()) return anim_fail(r, ev, name, {}, "an event needs a time 't'");
            if (!r.number(ev, "t", 0.0, e.t)) return false;
            if (e.t < 0.0f || (ei > 0 && e.t < prev))
                return anim_fail(r, ev.get("t"), name, {}, "events must be in time order, at 0 or more");
            prev = e.t;
            const JsonValue en = ev.get("name");
            if (!en.is_string() || en.as_string().empty() || en.as_string().size() > kMaxEventName)
                return anim_fail(r, en.valid() ? en : ev, name, {}, "an event's 'name' must be a string of 1 to 23 characters");
            e.name = add_string(pool, en.as_string());
            e.id = fnv1a64(en.as_string());
            if (out.events.size() >= asset::kSkeleton2DMaxEvents)
                return anim_fail(r, ev, name, {}, "the skeleton has too many events");
            out.events.push_back(e);
            ++anim.event_count;
            if (e.t > last) last = e.t;
        }

        anim.duration = last;
        if (const JsonValue d = a.get("duration"); d.valid()) {
            if (!r.number(a, "duration", 0.0, anim.duration)) return false;
            if (anim.duration < 0.0f) return anim_fail(r, d, name, {}, "'duration' must be 0 or more");
        }
        out.animations.push_back(anim);
    }
    return true;
}

class Skeleton2DImporter final : public IImporter {
public:
    StringView id() const override { return "skeleton2d"; }
    u32 version() const override { return 2; }  // 2: animations (M-ANIM2D 2)
    ChunkType type() const override { return ChunkType::Skeleton2D; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ".mcskel2d"; }

    void scan_dependencies(const ImportInput& in, Array<DepRef>& out, DiagList& diags) const override {
        const JsonValue slots = in.settings.get("slots");
        if (!slots.is_array()) return;
        for (u32 i = 0; i < slots.size(); ++i) {
            const JsonValue tex = slots.at(i).is_object() ? slots.at(i).get("texture") : JsonValue();
            if (!tex.valid() || tex.is_null()) continue;
            Guid g;
            if (!tex.is_string() || !parse_guid_ref(tex.as_string(), g)) {
                detail::error_at(diags, Errc::ParseError, in.display_path, tex, kStage,
                                 "'texture' must be \"guid:<uuid>\" or null");
                continue;
            }
            DepRef d;
            d.guid = g;
            d.expected = ChunkType::Texture;
            d.line = tex.line();
            d.col = tex.col();
            char where[64];
            std::snprintf(where, sizeof(where), "slots[%u].texture", i);
            d.where.assign(where);
            out.push_back(d);
        }
    }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue doc = in.settings;
        Reader r{in.display_path, diags};
        if (!detail::check_keys(doc, {"format", "version", "guid", "name", "bones", "slots", "animations"}, r.file, kStage,
                                diags))
            return false;
        const JsonValue fmt = doc.get("format");
        if (!fmt.is_string() || fmt.as_string() != "mce-skeleton2d")
            return r.fail(fmt.valid() ? fmt : doc, "\"format\" must be \"mce-skeleton2d\"");
        const JsonValue ver = doc.get("version");
        if (!ver.is_integer() || ver.as_number() != kFormatVersion) {
            String msg;
            appendf(msg, "unsupported skeleton version (this cook reads version %u)", kFormatVersion);
            detail::error_at(diags, Errc::VersionTooNew, r.file, ver.valid() ? ver : doc, kStage, msg);
            return false;
        }

        asset::Skeleton2DHeader h{};
        if (const JsonValue n = doc.get("name"); n.valid()) {
            if (!n.is_string() || n.as_string().size() >= sizeof(h.name))
                return r.fail(n, "'name' must be a string of at most 31 characters");
            std::memcpy(h.name, n.as_string().data(), n.as_string().size());
        }

        Array<char> pool;
        Array<Skeleton2DBone> bones;
        Array<StringView> bone_names;
        const JsonValue bone_list = doc.get("bones");
        if (!bone_list.is_array() || bone_list.size() == 0)
            return r.fail(bone_list.valid() ? bone_list : doc, "'bones' must be an array of at least one bone");
        if (bone_list.size() > asset::kSkeleton2DMaxBones) {
            String msg;
            appendf(msg, "a skeleton has at most %u bones (kit::Skeleton2D holds one pose per bone)",
                    asset::kSkeleton2DMaxBones);
            return r.fail(bone_list, msg.view());
        }
        for (u32 i = 0; i < bone_list.size(); ++i) {
            const JsonValue b = bone_list.at(i);
            if (!b.is_object())
                return r.fail(b, "a bone is {\"name\", \"parent\", \"x\", \"y\", \"rotation\", \"scale\", \"length\"}");
            if (!detail::check_keys(b, {"name", "parent", "x", "y", "rotation", "scale", "length"}, r.file, kStage,
                                    diags))
                return false;
            StringView name;
            if (!r.name(b, "bone", name)) return false;
            for (const StringView& other : bone_names)
                if (other == name) return r.fail(b.get("name"), "two bones share this name");
            Skeleton2DBone bone{};
            bone.parent = asset::kSkeleton2DNoParent;
            if (const JsonValue p = b.get("parent"); p.valid() && !p.is_null()) {
                if (!p.is_string()) return r.fail(p, "'parent' must be a bone name");
                i32 found = -1;
                for (u32 j = 0; j < bone_names.size(); ++j)
                    if (bone_names[j] == p.as_string()) found = static_cast<i32>(j);
                if (found < 0) {
                    bool later = false;
                    for (u32 j = i + 1; j < bone_list.size(); ++j) {
                        const JsonValue n = bone_list.at(j).is_object() ? bone_list.at(j).get("name") : JsonValue();
                        later |= n.is_string() && n.as_string() == p.as_string();
                    }
                    String msg;
                    if (later)
                        appendf(
                            msg,
                            "bone '%.*s' names parent '%.*s', which comes later; list parents before their children",
                            static_cast<int>(name.size()), name.data(), static_cast<int>(p.as_string().size()),
                            p.as_string().data());
                    else
                        appendf(msg, "bone '%.*s' names parent '%.*s', but no bone has that name",
                                static_cast<int>(name.size()), name.data(), static_cast<int>(p.as_string().size()),
                                p.as_string().data());
                    return r.fail(p, msg.view());
                }
                bone.parent = static_cast<u16>(found);
            }  // no parent: a root, posed from the entity's origin (a skeleton may have several)
            if (i == 0 && bone.parent != asset::kSkeleton2DNoParent)
                return r.fail(b, "the first bone is a root (it has no parent)");
            f32 rot = 0.0f;
            static constexpr f32 kOne[2] = {1.0f, 1.0f};
            f32 scale[2];
            if (!r.number(b, "x", 0.0, bone.x) || !r.number(b, "y", 0.0, bone.y) ||
                !r.number(b, "rotation", 0.0, rot) || !r.numbers(b, "scale", 2, kOne, scale, true) ||
                !r.number(b, "length", 0.0, bone.length))
                return false;
            if (bone.length < 0.0f) return r.fail(b.get("length"), "'length' must be 0 or more");
            bone.rotation = static_cast<f32>(rot * kDegToRad);
            bone.scale_x = scale[0];
            bone.scale_y = scale[1];
            bone.name = add_string(pool, name);
            bones.push_back(bone);
            bone_names.push_back(name);
        }

        Array<Skeleton2DSlot> slots;
        Array<StringView> slot_names;
        const JsonValue slot_list = doc.get("slots");
        if (slot_list.valid() && !slot_list.is_array()) return r.fail(slot_list, "'slots' must be an array");
        if (slot_list.valid() && slot_list.size() > asset::kSkeleton2DMaxSlots) {
            String msg;
            appendf(msg, "a skeleton has at most %u slots", asset::kSkeleton2DMaxSlots);
            return r.fail(slot_list, msg.view());
        }
        for (u32 i = 0; slot_list.valid() && i < slot_list.size(); ++i) {
            const JsonValue s = slot_list.at(i);
            if (!s.is_object())
                return r.fail(s,
                              "a slot is {\"name\", \"bone\", \"texture\", \"rect\", \"size\", \"pivot\", \"offset\", "
                              "\"rotation\", \"tint\"}");
            if (!detail::check_keys(s,
                                    {"name", "bone", "texture", "rect", "size", "pivot", "offset", "rotation", "tint"},
                                    r.file, kStage, diags))
                return false;
            StringView name;
            if (!r.name(s, "slot", name)) return false;
            for (const StringView& other : slot_names)
                if (other == name) return r.fail(s.get("name"), "two slots share this name");
            Skeleton2DSlot slot{};
            slot.kind = asset::Skeleton2DSlotKind::Quad;
            const JsonValue bn = s.get("bone");
            if (!bn.is_string()) return r.fail(bn.valid() ? bn : s, "a slot's 'bone' must be a bone name");
            i32 found = -1;
            for (u32 j = 0; j < bone_names.size(); ++j)
                if (bone_names[j] == bn.as_string()) found = static_cast<i32>(j);
            if (found < 0) {
                String msg;
                appendf(msg, "slot '%.*s' names bone '%.*s', but no bone has that name", static_cast<int>(name.size()),
                        name.data(), static_cast<int>(bn.as_string().size()), bn.as_string().data());
                return r.fail(bn, msg.view());
            }
            slot.bone = static_cast<u16>(found);
            if (const JsonValue t = s.get("texture"); t.is_string()) {
                Guid g;
                if (!parse_guid_ref(t.as_string(), g)) return r.fail(t, "'texture' must be \"guid:<uuid>\" or null");
                slot.texture[0] = g.hi;
                slot.texture[1] = g.lo;
            } else if (t.valid() && !t.is_null()) {
                return r.fail(t, "'texture' must be \"guid:<uuid>\" or null");
            }
            static constexpr f32 kRect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            static constexpr f32 kSize[2] = {1.0f, 1.0f};
            static constexpr f32 kPivot[2] = {0.5f, 0.5f};
            static constexpr f32 kZero[2] = {0.0f, 0.0f};
            static constexpr f32 kWhite[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            f32 rot = 0.0f;
            if (!r.numbers(s, "rect", 4, kRect, slot.rect) || !r.numbers(s, "size", 2, kSize, slot.size) ||
                !r.numbers(s, "pivot", 2, kPivot, slot.pivot) || !r.numbers(s, "offset", 2, kZero, slot.offset) ||
                !r.number(s, "rotation", 0.0, rot) || !r.numbers(s, "tint", 4, kWhite, slot.tint))
                return false;
            slot.rotation = static_cast<f32>(rot * kDegToRad);
            slot.name = add_string(pool, name);
            slots.push_back(slot);
            slot_names.push_back(name);
        }

        Animations anims;
        if (!read_animations(r, doc.get("animations"), bone_names, pool, anims)) return false;

        h.bone_count = static_cast<u32>(bones.size());
        h.slot_count = static_cast<u32>(slots.size());
        h.string_bytes = static_cast<u32>(pool.size());
        h.animation_count = static_cast<u32>(anims.animations.size());
        h.timeline_count = static_cast<u32>(anims.timelines.size());
        h.key_count = static_cast<u32>(anims.keys.size());
        h.event_count = static_cast<u32>(anims.events.size());
        Array<byte> payload;
        auto append = [&](const void* p, usize n) {
            const usize at = payload.size();
            payload.resize(at + n, byte{0});
            if (n) std::memcpy(payload.data() + at, p, n);
        };
        append(&h, sizeof(h));
        append(bones.data(), bones.size() * sizeof(Skeleton2DBone));
        append(slots.data(), slots.size() * sizeof(Skeleton2DSlot));
        append(anims.animations.data(), anims.animations.size() * sizeof(asset::Skeleton2DAnimation));
        append(anims.timelines.data(), anims.timelines.size() * sizeof(asset::Skeleton2DTimeline));
        append(anims.keys.data(), anims.keys.size() * sizeof(asset::Skeleton2DKey));
        append(anims.events.data(), anims.events.size() * sizeof(asset::Skeleton2DEvent));
        append(pool.data(), pool.size());

        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::Skeleton2D;
        // Minor 1 only when there are animations: a skeleton without them cooks byte for byte as before.
        d.version = asset::chunk_version(asset::SKELETON2D_VERSION,
                                         anims.animations.empty() ? 0 : asset::SKELETON2D_MINOR_ANIMATIONS);
        d.guid = in.guid;
        out.add(d, payload.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_skeleton2d_importer() { return mem::make_unique<Skeleton2DImporter>(mem::heap()); }

}  // namespace mce::cook
