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
// M-ANIM2D 3 adds attachments, meshes and slot keys (chunk minor 2; a skeleton without them cooks
// as before):
//   "slots": [{"name": "arm", "bone": "arm", "tint": [1, 1, 1, 1], "attachment": "arm_mesh" | null,
//              "attachments": [
//                {"name": "arm", "type": "quad", "texture": ..., "rect", "size", "pivot", "offset", "rotation"},
//                {"name": "arm_mesh", "type": "mesh", "texture": ...,
//                 "vertices": [{"uv": [0, 0], "x": 0, "y": 0},                      // in the slot bone's space
//                              {"uv": [1, 0], "weights": [{"bone": "arm", "x": 0.4, "y": 0, "weight": 0.5}, ...]}],
//                 "triangles": [0, 1, 2, ...]}]}]
//   "animations": [{..., "slots": {"arm": {"color": [{"t": 0, "color": [1, 1, 1, 1], "curve": ...}],
//                                          "attachment": [{"t": 0, "name": "arm_mesh" | null}]}},
//                        "deform": {"arm": {"arm_mesh": [{"t": 0, "offsets": [dx0, dy0, dx1, dy1, ...], "curve": ...}]}}}]
// A slot without "attachments" has one quad attachment named after the slot, made of its own
// fields (the step-1 meaning); a slot with "attachments" takes no quad fields itself. "attachment"
// names the one shown at first (default: the first listed; null hides the slot). Weights are
// normalized to sum to 1. Color keys are absolute linear RGBA; attachment keys are stepped; deform
// offsets (2 per vertex, default 0) are metres in the space of the bones the vertex follows.
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
    Array<asset::Skeleton2DSlotKey> slot_keys;
    Array<f32> values;
    bool slot_timelines = false;  // any color, attachment or deform keys (chunk minor 2)
};

// The attachments of the cooked slots, as the animations look them up by name.
struct SlotNames {
    struct Slot {
        StringView name;
        u32 first_name = 0;  // into names / global / vertices
        u32 name_count = 0;
    };
    Array<Slot> slots;
    Array<StringView> names;  // per slot: its attachments' names (a slot's own quad: the slot's name)
    Array<u32> global;        // the attachment's index in the chunk (~0u: the slot's own quad)
    Array<u32> vertices;      // a mesh's vertex count, 0 for quads
    i32 find(StringView slot) const {
        for (u32 i = 0; i < slots.size(); ++i)
            if (slots[i].name == slot) return static_cast<i32>(i);
        return -1;
    }
    // The local index of attachment `name` in slot `s`, or -1.
    i32 find_attachment(u32 s, StringView name) const {
        for (u32 i = 0; i < slots[s].name_count; ++i)
            if (names[slots[s].first_name + i] == name) return static_cast<i32>(i);
        return -1;
    }
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
template <class Key>
bool read_curve(Reader& r, JsonValue key, StringView anim, StringView bone, Key& k) {
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

bool read_slot_keys(Reader& r, JsonValue a, StringView name, const SlotNames& slots, Animations& out,
                    asset::Skeleton2DAnimation& anim, f32& last);

bool read_animations(Reader& r, JsonValue list, const Array<StringView>& bone_names, const SlotNames& slots,
                     Array<char>& pool, Animations& out) {
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
        if (!detail::check_keys(a, {"name", "duration", "loop", "bones", "events", "slots", "deform"}, r.file, kStage,
                                r.diags))
            return false;
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

        if (!read_slot_keys(r, a, name, slots, out, anim, last)) return false;

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

bool slot_fail(Reader& r, JsonValue at, StringView anim, StringView slot, StringView attachment, const char* msg) {
    String text;
    appendf(text, "animation '%.*s', slot '%.*s'", static_cast<int>(anim.size()), anim.data(), static_cast<int>(slot.size()),
            slot.data());
    if (!attachment.empty()) appendf(text, ", attachment '%.*s'", static_cast<int>(attachment.size()), attachment.data());
    appendf(text, ": %s", msg);
    return r.fail(at, text.view());
}

// One slot timeline's key list: times in increasing order; `fill` reads each key's value.
template <class Fill>
bool read_slot_timeline(Reader& r, JsonValue keys, StringView anim, StringView slot, StringView att, const char* what,
                        asset::Skeleton2DChannel channel, u32 slot_index, u32 attachment, Animations& out,
                        asset::Skeleton2DAnimation& a, f32& last, std::initializer_list<StringView> allowed, Fill&& fill) {
    if (!keys.is_array() || keys.size() == 0) {
        String msg;
        appendf(msg, "'%s' must be an array of at least one key", what);
        return slot_fail(r, keys, anim, slot, att, msg.c_str());
    }
    if (out.slot_keys.size() + keys.size() > asset::kSkeleton2DMaxSlotKeys)
        return slot_fail(r, keys, anim, slot, att, "the skeleton has too many slot keys");
    asset::Skeleton2DTimeline tl{};
    tl.bone = static_cast<u16>(slot_index);
    tl.channel = channel;
    tl.first_key = static_cast<u32>(out.slot_keys.size());
    tl.key_count = keys.size();
    tl.attachment = attachment;
    f32 prev = 0.0f;
    for (u32 ki = 0; ki < keys.size(); ++ki) {
        const JsonValue key = keys.at(ki);
        if (!key.is_object()) return slot_fail(r, key, anim, slot, att, "a key is an object with \"t\"");
        if (!detail::check_keys(key, allowed, r.file, kStage, r.diags)) return false;
        asset::Skeleton2DSlotKey k{};
        if (!key.get("t").valid()) return slot_fail(r, key, anim, slot, att, "a key needs a time 't'");
        if (!r.number(key, "t", 0.0, k.t)) return false;
        if (k.t < 0.0f) return slot_fail(r, key.get("t"), anim, slot, att, "a key's 't' must be 0 or more");
        if (ki > 0 && !(k.t > prev)) {
            String msg;
            appendf(msg, "%s keys must be in increasing time order (t %g after %g)", what, static_cast<f64>(k.t),
                    static_cast<f64>(prev));
            return slot_fail(r, key.get("t"), anim, slot, att, msg.c_str());
        }
        prev = k.t;
        if (!fill(key, k)) return false;
        out.slot_keys.push_back(k);
    }
    if (prev > last) last = prev;
    if (out.timelines.size() >= asset::kSkeleton2DMaxTimelines)
        return slot_fail(r, keys, anim, slot, att, "the skeleton has too many timelines");
    out.timelines.push_back(tl);
    ++a.timeline_count;
    out.slot_timelines = true;
    return true;
}

bool read_slot_keys(Reader& r, JsonValue a, StringView name, const SlotNames& slots, Animations& out,
                    asset::Skeleton2DAnimation& anim, f32& last) {
    const JsonValue list = a.get("slots");
    if (list.valid() && !list.is_object())
        return anim_fail(r, list, name, {}, "'slots' must be an object of slot name: {\"color\", \"attachment\"}");
    for (u32 si = 0; list.valid() && si < list.size(); ++si) {
        const StringView slot = list.key_at(si);
        const JsonValue channels = list.at(si);
        const i32 found = slots.find(slot);
        if (found < 0) return slot_fail(r, channels, name, slot, {}, "no slot has that name");
        for (u32 j = 0; j < si; ++j)
            if (list.key_at(j) == slot) return slot_fail(r, channels, name, slot, {}, "the slot is listed twice");
        if (!channels.is_object()) return slot_fail(r, channels, name, slot, {}, "a slot's keys are {\"color\", \"attachment\"}");
        if (!detail::check_keys(channels, {"color", "attachment"}, r.file, kStage, r.diags)) return false;
        const u32 s = static_cast<u32>(found);
        if (const JsonValue keys = channels.get("color"); keys.valid()) {
            auto fill = [&](JsonValue key, asset::Skeleton2DSlotKey& k) {
                static constexpr f32 kWhite[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                f32 c[4];
                if (!r.numbers(key, "color", 4, kWhite, c) || !read_curve(r, key, name, {}, k)) return false;
                if (out.values.size() + 4 > asset::kSkeleton2DMaxValues)
                    return slot_fail(r, key, name, slot, {}, "the skeleton has too many key values");
                k.value = static_cast<u32>(out.values.size());
                for (f32 x : c) out.values.push_back(x);
                return true;
            };
            if (!read_slot_timeline(r, keys, name, slot, {}, "color", asset::Skeleton2DChannel::Color, s, 0, out, anim, last,
                                    {"t", "color", "curve"}, fill))
                return false;
        }
        if (const JsonValue keys = channels.get("attachment"); keys.valid()) {
            auto fill = [&](JsonValue key, asset::Skeleton2DSlotKey& k) {
                const JsonValue n = key.get("name");
                k.curve = asset::Skeleton2DCurve::Stepped;
                if (n.is_null()) {
                    k.value = asset::kSkeleton2DNoAttachmentKey;
                    return true;
                }
                const i32 at = n.is_string() ? slots.find_attachment(s, n.as_string()) : -1;
                if (at < 0)
                    return slot_fail(r, n.valid() ? n : key, name, slot, {},
                                     "an attachment key's 'name' must name one of the slot's attachments, or be null");
                k.value = static_cast<u32>(at);
                return true;
            };
            if (!read_slot_timeline(r, keys, name, slot, {}, "attachment", asset::Skeleton2DChannel::Attachment, s, 0, out,
                                    anim, last, {"t", "name"}, fill))
                return false;
        }
    }

    const JsonValue deform = a.get("deform");
    if (deform.valid() && !deform.is_object())
        return anim_fail(r, deform, name, {}, "'deform' must be an object of slot name: {mesh attachment name: keys}");
    for (u32 si = 0; deform.valid() && si < deform.size(); ++si) {
        const StringView slot = deform.key_at(si);
        const JsonValue meshes = deform.at(si);
        const i32 found = slots.find(slot);
        if (found < 0) return slot_fail(r, meshes, name, slot, {}, "no slot has that name");
        for (u32 j = 0; j < si; ++j)
            if (deform.key_at(j) == slot) return slot_fail(r, meshes, name, slot, {}, "the slot is listed twice");
        if (!meshes.is_object()) return slot_fail(r, meshes, name, slot, {}, "a slot's deform keys are {mesh attachment name: keys}");
        const u32 s = static_cast<u32>(found);
        for (u32 mi = 0; mi < meshes.size(); ++mi) {
            const StringView att = meshes.key_at(mi);
            const i32 local = slots.find_attachment(s, att);
            const u32 entry = local < 0 ? 0u : slots.slots[s].first_name + static_cast<u32>(local);
            if (local < 0 || slots.vertices[entry] == 0)
                return slot_fail(r, meshes.at(mi), name, slot, att, "deform keys need a mesh attachment of the slot");
            for (u32 j = 0; j < mi; ++j)
                if (meshes.key_at(j) == att) return slot_fail(r, meshes.at(mi), name, slot, att, "the attachment is listed twice");
            const u32 n = 2 * slots.vertices[entry];
            auto fill = [&](JsonValue key, asset::Skeleton2DSlotKey& k) {
                if (!read_curve(r, key, name, {}, k)) return false;
                if (out.values.size() + n > asset::kSkeleton2DMaxValues)
                    return slot_fail(r, key, name, slot, att, "the skeleton has too many key values");
                const JsonValue o = key.get("offsets");
                bool ok = !o.valid() || (o.is_array() && o.size() == n);
                for (u32 i = 0; ok && o.valid() && i < n; ++i) ok = o.at(i).is_number() && std::isfinite(o.at(i).as_number());
                if (!ok) {
                    String msg;
                    appendf(msg, "'offsets' must be %u numbers (x, y for each of the mesh's %u vertices)", n, n / 2);
                    return slot_fail(r, o, name, slot, att, msg.c_str());
                }
                k.value = static_cast<u32>(out.values.size());
                for (u32 i = 0; i < n; ++i) out.values.push_back(o.valid() ? static_cast<f32>(o.at(i).as_number()) : 0.0f);
                return true;
            };
            if (!read_slot_timeline(r, meshes.at(mi), name, slot, att, "deform", asset::Skeleton2DChannel::Deform, s,
                                    slots.global[entry], out, anim, last, {"t", "offsets", "curve"}, fill))
                return false;
        }
    }
    return true;
}

// Meshes and the attachment list the slots cook into (chunk minor 2).
struct Attachments {
    Array<asset::Skeleton2DAttachment> attachments;
    Array<asset::Skeleton2DVertex> vertices;
    Array<asset::Skeleton2DWeight> weights;
    Array<u32> indices;
};

bool att_fail(Reader& r, JsonValue at, StringView slot, StringView att, const char* msg) {
    String text;
    appendf(text, "slot '%.*s', attachment '%.*s': %s", static_cast<int>(slot.size()), slot.data(),
            static_cast<int>(att.size()), att.data(), msg);
    return r.fail(at, text.view());
}

// The texture of a slot or an attachment: "guid:<uuid>", null or absent.
bool read_texture(Reader& r, JsonValue obj, u64 out[2]) {
    const JsonValue t = obj.get("texture");
    if (t.is_string()) {
        Guid g;
        if (!parse_guid_ref(t.as_string(), g)) return r.fail(t, "'texture' must be \"guid:<uuid>\" or null");
        out[0] = g.hi;
        out[1] = g.lo;
    } else if (t.valid() && !t.is_null()) {
        return r.fail(t, "'texture' must be \"guid:<uuid>\" or null");
    }
    return true;
}

// The quad fields of a slot or a quad attachment.
bool read_quad(Reader& r, JsonValue o, f32 rect[4], f32 size[2], f32 pivot[2], f32 offset[2], f32& rotation) {
    static constexpr f32 kRect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    static constexpr f32 kSize[2] = {1.0f, 1.0f};
    static constexpr f32 kPivot[2] = {0.5f, 0.5f};
    static constexpr f32 kZero[2] = {0.0f, 0.0f};
    f32 rot = 0.0f;
    if (!r.numbers(o, "rect", 4, kRect, rect) || !r.numbers(o, "size", 2, kSize, size) ||
        !r.numbers(o, "pivot", 2, kPivot, pivot) || !r.numbers(o, "offset", 2, kZero, offset) || !r.number(o, "rotation", 0.0, rot))
        return false;
    rotation = static_cast<f32>(rot * kDegToRad);
    return true;
}

bool read_mesh(Reader& r, JsonValue o, StringView slot, StringView att, const Array<StringView>& bone_names, Attachments& out,
               asset::Skeleton2DAttachment& a) {
    const JsonValue verts = o.get("vertices");
    if (!verts.is_array() || verts.size() < 3 || verts.size() > asset::kSkeleton2DMaxMeshVertices) {
        String msg;
        appendf(msg, "a mesh's 'vertices' must be an array of 3 to %u vertices", asset::kSkeleton2DMaxMeshVertices);
        return att_fail(r, verts.valid() ? verts : o, slot, att, msg.c_str());
    }
    if (out.vertices.size() + verts.size() > asset::kSkeleton2DMaxVertices)
        return att_fail(r, verts, slot, att, "the skeleton has too many mesh vertices");
    a.first_vertex = static_cast<u32>(out.vertices.size());
    a.vertex_count = verts.size();
    for (u32 vi = 0; vi < verts.size(); ++vi) {
        const JsonValue v = verts.at(vi);
        String where;
        appendf(where, "vertex %u: ", vi);
        auto vfail = [&](JsonValue at, const char* msg) {
            String m = where;
            appendf(m, "%s", msg);
            return att_fail(r, at, slot, att, m.c_str());
        };
        if (!v.is_object()) return vfail(v, "a vertex is {\"uv\", \"x\", \"y\"} or {\"uv\", \"weights\"}");
        if (!detail::check_keys(v, {"uv", "x", "y", "weights"}, r.file, kStage, r.diags)) return false;
        asset::Skeleton2DVertex vx{};
        static constexpr f32 kZero[2] = {0.0f, 0.0f};
        f32 uv[2];
        if (!v.get("uv").valid()) return vfail(v, "a vertex needs its texture coordinates 'uv'");
        if (!r.numbers(v, "uv", 2, kZero, uv)) return false;
        vx.u = uv[0];
        vx.v = uv[1];
        const JsonValue ws = v.get("weights");
        if (ws.valid()) {
            if (v.get("x").valid() || v.get("y").valid())
                return vfail(v, "a weighted vertex has its positions in 'weights', not 'x' and 'y'");
            if (!ws.is_array() || ws.size() == 0 || ws.size() > asset::kSkeleton2DMaxVertexWeights)
                return vfail(ws, "'weights' must be an array of 1 to 4 {\"bone\", \"x\", \"y\", \"weight\"}");
            if (out.weights.size() + ws.size() > asset::kSkeleton2DMaxWeights)
                return vfail(ws, "the skeleton has too many vertex weights");
            vx.first_weight = static_cast<u32>(out.weights.size());
            vx.weight_count = ws.size();
            f64 sum = 0.0;
            for (u32 wi = 0; wi < ws.size(); ++wi) {
                const JsonValue w = ws.at(wi);
                if (!w.is_object()) return vfail(w, "a weight is {\"bone\", \"x\", \"y\", \"weight\"}");
                if (!detail::check_keys(w, {"bone", "x", "y", "weight"}, r.file, kStage, r.diags)) return false;
                asset::Skeleton2DWeight bw{};
                const JsonValue bn = w.get("bone");
                i32 found = -1;
                for (u32 j = 0; bn.is_string() && j < bone_names.size(); ++j)
                    if (bone_names[j] == bn.as_string()) found = static_cast<i32>(j);
                if (found < 0) return vfail(bn.valid() ? bn : w, "a weight's 'bone' must name a bone");
                bw.bone = static_cast<u16>(found);
                if (!w.get("weight").valid()) return vfail(w, "a weight needs its 'weight'");
                if (!r.number(w, "x", 0.0, bw.x) || !r.number(w, "y", 0.0, bw.y) || !r.number(w, "weight", 0.0, bw.weight))
                    return false;
                if (bw.weight < 0.0f) return vfail(w.get("weight"), "a weight must be 0 or more");
                sum += static_cast<f64>(bw.weight);
                out.weights.push_back(bw);
            }
            if (!(sum > 0.0)) return vfail(ws, "a vertex's weights must not all be 0");
            // Normalized so they sum to 1 (the reader checks within 1e-3).
            for (u32 wi = 0; wi < ws.size(); ++wi) {
                f32& w = out.weights[vx.first_weight + wi].weight;
                w = static_cast<f32>(static_cast<f64>(w) / sum);
            }
        } else if (!r.number(v, "x", 0.0, vx.x) || !r.number(v, "y", 0.0, vx.y)) {
            return false;
        }
        out.vertices.push_back(vx);
    }
    const JsonValue tris = o.get("triangles");
    if (!tris.is_array() || tris.size() < 3 || tris.size() % 3 != 0 || tris.size() > 3 * asset::kSkeleton2DMaxMeshTriangles) {
        String msg;
        appendf(msg, "a mesh's 'triangles' must be vertex indices, three per triangle, 1 to %u triangles",
                asset::kSkeleton2DMaxMeshTriangles);
        return att_fail(r, tris.valid() ? tris : o, slot, att, msg.c_str());
    }
    if (out.indices.size() + tris.size() > asset::kSkeleton2DMaxIndices)
        return att_fail(r, tris, slot, att, "the skeleton has too many triangle indices");
    a.first_index = static_cast<u32>(out.indices.size());
    a.index_count = tris.size();
    for (u32 i = 0; i < tris.size(); ++i) {
        const JsonValue t = tris.at(i);
        if (!t.is_integer() || t.as_number() < 0.0 || t.as_number() >= static_cast<f64>(a.vertex_count)) {
            String msg;
            appendf(msg, "triangle index %u must be a vertex index below %u", i, a.vertex_count);
            return att_fail(r, t, slot, att, msg.c_str());
        }
        out.indices.push_back(static_cast<u32>(t.as_number()));
    }
    return true;
}

// A slot's "attachments" and "attachment". Without "attachments" the slot's own quad is its one
// attachment, named after it.
bool read_slot_attachments(Reader& r, JsonValue s, StringView name, u32 slot_index, const Array<StringView>& bone_names,
                           Array<char>& pool, asset::Skeleton2DSlot& slot, Attachments& out, SlotNames& names,
                           bool& attached) {
    SlotNames::Slot info;
    info.name = name;
    info.first_name = static_cast<u32>(names.names.size());
    const JsonValue list = s.get("attachments");
    const JsonValue shown = s.get("attachment");
    if (list.valid() || shown.valid()) attached = true;
    if (!list.valid()) {
        names.names.push_back(name);
        names.global.push_back(~0u);
        names.vertices.push_back(0);
        info.name_count = 1;
        names.slots.push_back(info);
        if (shown.is_null()) slot.attachment = asset::kSkeleton2DNoAttachment;
        else if (shown.valid() && !(shown.is_string() && shown.as_string() == name))
            return r.fail(shown, "a slot without 'attachments' shows its own quad: 'attachment' must be its name or null");
        return true;
    }
    for (const char* k : {"texture", "rect", "size", "pivot", "offset", "rotation"})
        if (s.get(k).valid()) {
            String msg;
            appendf(msg, "slot '%.*s' lists 'attachments': put '%s' in an attachment", static_cast<int>(name.size()), name.data(), k);
            return r.fail(s.get(k), msg.view());
        }
    if (!list.is_array() || list.size() == 0 || list.size() > asset::kSkeleton2DMaxSlotAttachments) {
        String msg;
        appendf(msg, "'attachments' must be an array of 1 to %u attachments", asset::kSkeleton2DMaxSlotAttachments);
        return r.fail(list, msg.view());
    }
    if (out.attachments.size() + list.size() > asset::kSkeleton2DMaxAttachments) {
        String msg;
        appendf(msg, "a skeleton has at most %u attachments", asset::kSkeleton2DMaxAttachments);
        return r.fail(list, msg.view());
    }
    slot.first_attachment = static_cast<u32>(out.attachments.size());
    slot.attachment_count = static_cast<u16>(list.size());
    for (u32 i = 0; i < list.size(); ++i) {
        const JsonValue o = list.at(i);
        if (!o.is_object()) return r.fail(o, "an attachment is {\"name\", \"type\": \"quad\" | \"mesh\", ...}");
        StringView an;
        if (!r.name(o, "attachment", an)) return false;
        for (u32 j = 0; j < i; ++j)
            if (names.names[info.first_name + j] == an) return att_fail(r, o.get("name"), name, an, "two attachments of the slot share this name");
        asset::Skeleton2DAttachment a{};
        a.slot = static_cast<u16>(slot_index);
        const JsonValue type = o.get("type");
        bool mesh = false;
        if (type.is_string() && type.as_string() == "mesh") mesh = true;
        else if (type.valid() && !(type.is_string() && type.as_string() == "quad"))
            return att_fail(r, type, name, an, "'type' must be \"quad\" or \"mesh\"");
        if (mesh ? !detail::check_keys(o, {"name", "type", "texture", "vertices", "triangles"}, r.file, kStage, r.diags)
                 : !detail::check_keys(o, {"name", "type", "texture", "rect", "size", "pivot", "offset", "rotation"}, r.file,
                                       kStage, r.diags))
            return false;
        if (!read_texture(r, o, a.texture)) return false;
        if (!read_quad(r, o, a.rect, a.size, a.pivot, a.offset, a.rotation)) return false;
        if (mesh) {
            a.kind = asset::Skeleton2DAttachmentKind::Mesh;
            if (!read_mesh(r, o, name, an, bone_names, out, a)) return false;
        }
        a.name = add_string(pool, an);
        names.names.push_back(an);
        names.global.push_back(static_cast<u32>(out.attachments.size()));
        names.vertices.push_back(mesh ? a.vertex_count : 0u);
        out.attachments.push_back(a);
    }
    info.name_count = list.size();
    names.slots.push_back(info);
    slot.attachment = 0;
    if (shown.is_null()) {
        slot.attachment = asset::kSkeleton2DNoAttachment;
    } else if (shown.valid()) {
        const i32 at = shown.is_string() ? names.find_attachment(static_cast<u32>(names.slots.size() - 1), shown.as_string()) : -1;
        if (at < 0) return r.fail(shown, "'attachment' must name one of the slot's attachments, or be null");
        slot.attachment = static_cast<u8>(at);
    }
    return true;
}

class Skeleton2DImporter final : public IImporter {
public:
    StringView id() const override { return "skeleton2d"; }
    u32 version() const override { return 3; }  // 2: animations (M-ANIM2D 2); 3: attachments, meshes, slot keys (M-ANIM2D 3)
    ChunkType type() const override { return ChunkType::Skeleton2D; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ".mcskel2d"; }

    void scan_dependencies(const ImportInput& in, Array<DepRef>& out, DiagList& diags) const override {
        const JsonValue slots = in.settings.get("slots");
        if (!slots.is_array()) return;
        auto dep = [&](JsonValue tex, const char* where) {
            if (!tex.valid() || tex.is_null()) return;
            Guid g;
            if (!tex.is_string() || !parse_guid_ref(tex.as_string(), g)) {
                detail::error_at(diags, Errc::ParseError, in.display_path, tex, kStage,
                                 "'texture' must be \"guid:<uuid>\" or null");
                return;
            }
            DepRef d;
            d.guid = g;
            d.expected = ChunkType::Texture;
            d.line = tex.line();
            d.col = tex.col();
            d.where.assign(where);
            out.push_back(d);
        };
        for (u32 i = 0; i < slots.size(); ++i) {
            if (!slots.at(i).is_object()) continue;
            char where[64];
            std::snprintf(where, sizeof(where), "slots[%u].texture", i);
            dep(slots.at(i).get("texture"), where);
            const JsonValue list = slots.at(i).get("attachments");
            for (u32 j = 0; list.is_array() && j < list.size(); ++j) {
                if (!list.at(j).is_object()) continue;
                std::snprintf(where, sizeof(where), "slots[%u].attachments[%u].texture", i, j);
                dep(list.at(j).get("texture"), where);
            }
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
        SlotNames slot_names;
        Attachments atts;
        bool attached = false;  // attachments, meshes or slot keys: chunk minor 2
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
                              "\"rotation\", \"tint\", \"attachments\", \"attachment\"}");
            if (!detail::check_keys(s,
                                    {"name", "bone", "texture", "rect", "size", "pivot", "offset", "rotation", "tint",
                                     "attachments", "attachment"},
                                    r.file, kStage, diags))
                return false;
            StringView name;
            if (!r.name(s, "slot", name)) return false;
            if (slot_names.find(name) >= 0) return r.fail(s.get("name"), "two slots share this name");
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
            if (!read_slot_attachments(r, s, name, i, bone_names, pool, slot, atts, slot_names, attached)) return false;
            if (!read_texture(r, s, slot.texture)) return false;
            static constexpr f32 kWhite[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            if (!read_quad(r, s, slot.rect, slot.size, slot.pivot, slot.offset, slot.rotation) ||
                !r.numbers(s, "tint", 4, kWhite, slot.tint))
                return false;
            slot.name = add_string(pool, name);
            slots.push_back(slot);
        }

        Animations anims;
        if (!read_animations(r, doc.get("animations"), bone_names, slot_names, pool, anims)) return false;
        attached = attached || anims.slot_timelines;

        h.bone_count = static_cast<u32>(bones.size());
        h.slot_count = static_cast<u32>(slots.size());
        h.string_bytes = static_cast<u32>(pool.size());
        h.animation_count = static_cast<u32>(anims.animations.size());
        h.timeline_count = static_cast<u32>(anims.timelines.size());
        h.key_count = static_cast<u32>(anims.keys.size());
        h.event_count = static_cast<u32>(anims.events.size());
        asset::Skeleton2DAttachmentHeader ah{};
        if (attached) {
            h.flags = asset::Skeleton2DHeaderFlags::Attachments;
            ah.attachment_count = static_cast<u32>(atts.attachments.size());
            ah.vertex_count = static_cast<u32>(atts.vertices.size());
            ah.weight_count = static_cast<u32>(atts.weights.size());
            ah.index_count = static_cast<u32>(atts.indices.size());
            ah.slot_key_count = static_cast<u32>(anims.slot_keys.size());
            ah.value_count = static_cast<u32>(anims.values.size());
        }
        Array<byte> payload;
        auto append = [&](const void* p, usize n) {
            const usize at = payload.size();
            payload.resize(at + n, byte{0});
            if (n) std::memcpy(payload.data() + at, p, n);
        };
        append(&h, sizeof(h));
        if (attached) append(&ah, sizeof(ah));
        append(bones.data(), bones.size() * sizeof(Skeleton2DBone));
        append(slots.data(), slots.size() * sizeof(Skeleton2DSlot));
        append(anims.animations.data(), anims.animations.size() * sizeof(asset::Skeleton2DAnimation));
        append(anims.timelines.data(), anims.timelines.size() * sizeof(asset::Skeleton2DTimeline));
        append(anims.keys.data(), anims.keys.size() * sizeof(asset::Skeleton2DKey));
        append(anims.events.data(), anims.events.size() * sizeof(asset::Skeleton2DEvent));
        if (attached) {
            append(atts.attachments.data(), atts.attachments.size() * sizeof(asset::Skeleton2DAttachment));
            append(atts.vertices.data(), atts.vertices.size() * sizeof(asset::Skeleton2DVertex));
            append(atts.weights.data(), atts.weights.size() * sizeof(asset::Skeleton2DWeight));
            append(atts.indices.data(), atts.indices.size() * sizeof(u32));
            append(anims.slot_keys.data(), anims.slot_keys.size() * sizeof(asset::Skeleton2DSlotKey));
            append(anims.values.data(), anims.values.size() * sizeof(f32));
        }
        append(pool.data(), pool.size());

        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::Skeleton2D;
        // Minor 1 only when there are animations, minor 2 only with attachments, meshes or slot keys:
        // a skeleton without them cooks byte for byte as before.
        d.version = asset::chunk_version(asset::SKELETON2D_VERSION,
                                         attached                  ? asset::SKELETON2D_MINOR_ATTACHMENTS
                                         : anims.animations.empty() ? 0
                                                                    : asset::SKELETON2D_MINOR_ANIMATIONS);
        d.guid = in.guid;
        out.add(d, payload.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_skeleton2d_importer() { return mem::make_unique<Skeleton2DImporter>(mem::heap()); }

}  // namespace mce::cook
