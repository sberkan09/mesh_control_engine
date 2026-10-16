// 2D skeleton tools (M-ANIM2D step 5): skel2d_create, skel2d_add_bone, skel2d_add_slot,
// skel2d_add_ik, skel2d_key and skel2d_preview. A rig is a .mcskel2d (user guide 7.13); play it
// with a Skeleton2D component (and a Skeleton2DAnimator for its clips).
//
// The edits are the editor's own (editor/skeleton2d_editor.h, what the Skeleton panel calls): the
// document is changed as JSON, then cooked in memory by the real importer before anything is
// written, so a bad edit fails with the importer's file:line message and the file stays as it was.
// skel2d_preview poses the skeleton with the runtime's own functions (the animator's blend and
// skeleton2d_solve_pose) and returns the bone transforms; the image is a CPU line drawing of the
// bones and slot quads, not a render of the textures (render_frame shows those in a scene).

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "agent_image.h"
#include "mce/cook/cook.h"
#include "mce/cook/fs.h"
#include "mce/editor/skeleton2d_editor.h"
#include "tool_args.h"
#include "tools.h"

namespace mce::mcp {

namespace {

namespace sk = editor::skel2d;

constexpr f64 kRadToDeg = 57.295779513082320876798;

bool is_mcskel2d(StringView rel) { return rel.size() > 9 && rel.substr(rel.size() - 9) == ".mcskel2d"; }

// Numbers in an object (or the call's arguments): absent keeps `out`; false with c.error set.
struct Fields {
    ToolCall& c;
    cook::JsonValue obj;
    const char* ctx = "";  // " (in keys[2])" and the like

    bool bad(const char* name, const char* expected) {
        c.error += "argument '";
        c.error += name;
        c.error += "' ";
        c.error += ctx;
        c.error += "must be ";
        c.error += expected;
        return false;
    }
    bool number(const char* name, f64& out) {
        const cook::JsonValue v = obj.get(name);
        if (!v.valid()) return true;
        if (!v.is_number() || !std::isfinite(v.as_number())) return bad(name, "a number");
        out = v.as_number();
        return true;
    }
    bool numbers(const char* name, u32 n, f64* out) {
        const cook::JsonValue v = obj.get(name);
        if (!v.valid()) return true;
        bool ok = v.is_array() && v.size() == n;
        for (u32 i = 0; ok && i < n; ++i) ok = v.at(i).is_number() && std::isfinite(v.at(i).as_number());
        if (!ok) {
            char what[48];
            std::snprintf(what, sizeof(what), "an array of %u numbers", n);
            return bad(name, what);
        }
        for (u32 i = 0; i < n; ++i) out[i] = v.at(i).as_number();
        return true;
    }
    bool string(const char* name, cook::String& out) {
        const cook::JsonValue v = obj.get(name);
        if (!v.valid()) return true;
        if (!v.is_string()) return bad(name, "a string");
        out = cook::String(v.as_string());
        return true;
    }
    bool boolean(const char* name, bool& has, bool& out) {
        const cook::JsonValue v = obj.get(name);
        if (!v.valid()) return true;
        if (!v.is_bool()) return bad(name, "true or false");
        has = true;
        out = v.as_bool();
        return true;
    }
};

// The skeleton named by 'skeleton' (a .mcskel2d path under assets/ or "guid:<uuid>"): its path,
// GUID and source text.
struct SkelFile {
    cook::String rel;
    asset::Guid guid;
    cook::String text;
};

bool arg_skeleton(ToolCall& c, SkelFile& out) {
    if (!need_project(c)) return false;
    const cook::JsonValue d = arg(c, "skeleton");
    if (!d.is_string()) return arg_missing(c, "skeleton", "a .mcskel2d path under assets/ or \"guid:<uuid>\"");
    const StringView ref = d.as_string();
    const editor::EditorProject& project = c.server.session().project();
    const editor::ProjectAsset* a = nullptr;
    if (ref.size() > 5 && ref.substr(0, 5) == "guid:") {
        const Result<asset::Guid> g = asset::parse_guid(ref.substr(5));
        if (g) a = project.find(g.value());
    } else {
        for (const editor::ProjectAsset& pa : project.assets())
            if (pa.path.view() == ref) a = &pa;
    }
    if (!a) return fail(c, "argument 'skeleton': no such asset (asset_list type Skeleton2D lists them)");
    if (a->type != asset::ChunkType::Skeleton2D) return fail(c, "argument 'skeleton' is not a 2D skeleton (.mcskel2d)");
    out.rel = a->path;
    out.guid = a->guid;
    if (!cook::fs::read_text(project.asset_path(a->path.view()).view(), out.text))
        return fail(c, "the skeleton could not be read");
    return true;
}

void write_summary(ToolCall& c, StringView rel, asset::Guid guid, StringView text) {
    editor::Skeleton2DInfo info;
    cook::String why;
    const bool ok = editor::Skeleton2DInfo::read(text, info, why);
    c.out.begin_object();
    c.out.key("skeleton").str(rel).key("guid").str(asset_guid_ref(guid).view());
    if (ok) {
        c.out.key("bones").num_u(info.bones.size()).key("slots").num_u(info.slots.size());
        c.out.key("animations").num_u(info.animations.size()).key("ik").num_u(info.ik.size());
    }
    c.out.end_object();
}

// Cooks and writes the edited text; the result names the skeleton and its counts.
bool save_edit(ToolCall& c, const SkelFile& f, StringView text) {
    if (!asset_validate_and_write(c, f.rel.view(), f.guid, text)) return false;
    write_summary(c, f.rel.view(), f.guid, text);
    return true;
}

bool edit_failed(ToolCall& c, const cook::String& why) {
    c.error += "the edit cannot be made: ";
    c.error += why.view();
    return false;
}

bool skel2d_create(ToolCall& c) {
    StringView rel;
    if (!need_project(c) || !arg_string(c, "path", rel, true)) return false;
    if (!asset_rel_valid(rel) || !is_mcskel2d(rel))
        return fail(c,
                    "argument 'path' must be a .mcskel2d path under assets/ like \"rigs/hero.mcskel2d\" (forward "
                    "slashes, no '..')");
    editor::EditorProject& project = c.server.session().project();
    if (cook::fs::exists(project.asset_path(rel).view()))
        return fail(
            c, "a file already exists there: skel2d_add_bone, skel2d_add_slot, skel2d_add_ik and skel2d_key change it");
    StringView name, root = "root";
    if (!arg_string(c, "name", name, false) || !arg_string(c, "root", root, false)) return false;
    if (name.empty()) {
        name = rel.substr(0, rel.size() - 9);
        const usize slash = name.rfind('/');
        if (slash != StringView::npos) name = name.substr(slash + 1);
    }
    if (root.empty() || root.size() > 31) return arg_missing(c, "root", "a bone name of 1 to 31 characters");
    if (name.size() > 31) return arg_missing(c, "name", "at most 31 characters");
    const asset::Guid guid = cook::generate_guid();
    const cook::String text = sk::new_text(guid, name, root);
    if (!asset_validate_and_write(c, rel, guid, text.view())) return false;
    write_summary(c, rel, guid, text.view());
    return true;
}

bool skel2d_add_bone(ToolCall& c) {
    SkelFile f;
    if (!arg_skeleton(c, f)) return false;
    Fields in{c, c.args};
    sk::BoneSpec b;
    if (!in.string("name", b.name) || !in.string("parent", b.parent) || !in.number("x", b.x) || !in.number("y", b.y) ||
        !in.number("rotation", b.rotation) || !in.number("length", b.length))
        return false;
    if (b.name.empty()) return arg_missing(c, "name", "the new bone's name (1 to 31 characters)");
    if (const cook::JsonValue s = arg(c, "scale"); s.valid()) {
        if (s.is_number()) {
            b.scale_x = b.scale_y = s.as_number();
        } else {
            f64 xy[2] = {1, 1};
            if (!in.numbers("scale", 2, xy)) return arg_missing(c, "scale", "a number or [sx, sy]");
            b.scale_x = xy[0];
            b.scale_y = xy[1];
        }
    }
    cook::String why;
    if (!sk::add_bone(f.text, b, why)) return edit_failed(c, why);
    return save_edit(c, f, f.text.view());
}

bool skel2d_add_slot(ToolCall& c) {
    SkelFile f;
    if (!arg_skeleton(c, f)) return false;
    Fields in{c, c.args};
    sk::SlotSpec s;
    if (!in.string("name", s.name) || !in.string("bone", s.bone) || !in.numbers("rect", 4, s.rect) ||
        !in.numbers("size", 2, s.size) || !in.numbers("pivot", 2, s.pivot) || !in.numbers("offset", 2, s.offset) ||
        !in.number("rotation", s.rotation) || !in.numbers("tint", 4, s.tint))
        return false;
    if (s.name.empty()) return arg_missing(c, "name", "the new slot's name (1 to 31 characters)");
    if (s.bone.empty()) return arg_missing(c, "bone", "the bone the slot rides");
    if (const cook::JsonValue t = arg(c, "texture"); t.valid() && !t.is_null()) {
        if (!t.is_string()) return arg_missing(c, "texture", "\"guid:<uuid>\" of a texture, or null for a solid quad");
        s.texture = cook::String(t.as_string());
    }
    if (const cook::JsonValue att = arg(c, "attachments"); att.valid()) {
        if (!att.is_array() || att.size() == 0)
            return arg_missing(c, "attachments", "an array of one or more attachment objects");
        cook::String json;
        cook::write_json(att, json, true);
        s.attachments_json = json;
        const cook::JsonValue shown = arg(c, "attachment");
        if (shown.valid() && shown.is_null())
            s.attachment_null = true;
        else if (shown.valid() && shown.is_string())
            s.attachment = cook::String(shown.as_string());
        else if (shown.valid())
            return arg_missing(c, "attachment", "the name of the attachment shown first, or null");
    }
    cook::String why;
    if (!sk::add_slot(f.text, s, why)) return edit_failed(c, why);
    return save_edit(c, f, f.text.view());
}

bool skel2d_add_ik(ToolCall& c) {
    SkelFile f;
    if (!arg_skeleton(c, f)) return false;
    Fields in{c, c.args};
    sk::IkSpec ik;
    f64 iterations = 0, order = 0;
    if (!in.string("name", ik.name) || !in.string("target", ik.target) || !in.number("mix", ik.mix) ||
        !in.string("type", ik.type) || !in.string("bend", ik.bend) || !in.number("iterations", iterations) ||
        !in.number("order", order))
        return false;
    if (ik.name.empty()) return arg_missing(c, "name", "the constraint's name (1 to 31 characters)");
    const cook::JsonValue bones = arg(c, "bones");
    if (!bones.is_array() || bones.size() == 0) return arg_missing(c, "bones", "an array of bone names, parent first");
    for (u32 i = 0; i < bones.size(); ++i) {
        if (!bones.at(i).is_string()) return arg_missing(c, "bones", "an array of bone names, parent first");
        ik.bones.push_back(cook::String(bones.at(i).as_string()));
    }
    if (iterations < 0 || iterations > 1000 || order < -1000 || order > 1000)
        return fail(c, "'iterations' and 'order' are out of range");
    ik.iterations = static_cast<u32>(iterations);
    ik.order = static_cast<i32>(order);
    cook::String why;
    if (!sk::add_ik(f.text, ik, why)) return edit_failed(c, why);
    return save_edit(c, f, f.text.view());
}

// The key fields of `obj` (a key object, or the call's arguments) onto `k`, for k.channel.
bool parse_key(ToolCall& c, cook::JsonValue obj, const char* ctx, sk::KeySpec& k) {
    Fields in{c, obj, ctx};
    if (!obj.get("t").valid()) return in.bad("t", "the key's time in seconds, 0 or more");
    if (!in.number("t", k.t) || k.t < 0) return in.bad("t", "the key's time in seconds, 0 or more");
    switch (k.channel) {
        case sk::Channel::Rotate:
        case sk::Channel::IkMix:
            if (!obj.get("value").valid())
                return in.bad("value", k.channel == sk::Channel::Rotate ? "degrees added to the bone's rest rotation"
                                                                        : "the mix, 0 to 1");
            if (!in.number("value", k.value)) return false;
            break;
        case sk::Channel::Translate:
            k.x = k.y = 0;
            if (!in.number("x", k.x) || !in.number("y", k.y)) return false;
            break;
        case sk::Channel::Scale:
            k.x = k.y = 1;
            if (!in.number("x", k.x) || !in.number("y", k.y)) return false;
            break;
        case sk::Channel::SlotColor:
            if (!obj.get("color").valid()) return in.bad("color", "linear RGBA [r, g, b, a]");
            if (!in.numbers("color", 4, k.color)) return false;
            break;
        case sk::Channel::SlotAttachment: {
            const cook::JsonValue a = obj.get("attachment");
            if (!a.valid() || !(a.is_string() || a.is_null()))
                return in.bad("attachment", "an attachment name, or null to hide the slot");
            k.attachment_null = a.is_null();
            if (a.is_string()) k.attachment = cook::String(a.as_string());
            break;
        }
    }
    k.curve = cook::String();
    k.bezier = false;
    if (const cook::JsonValue cv = obj.get("curve"); cv.valid()) {
        if (cv.is_string() && (cv.as_string() == "linear" || cv.as_string() == "stepped")) {
            k.curve = cook::String(cv.as_string());
        } else if (cv.is_array()) {
            if (!in.numbers("curve", 4, k.bezier_c)) return false;
            k.bezier = true;
        } else {
            return in.bad("curve", "\"linear\", \"stepped\" or a bezier [cx1, cy1, cx2, cy2]");
        }
    }
    return true;
}

bool skel2d_key(ToolCall& c) {
    SkelFile f;
    if (!arg_skeleton(c, f)) return false;
    Fields in{c, c.args};
    sk::KeySpec base;
    if (!in.string("animation", base.animation) || !in.string("target", base.target)) return false;
    if (base.animation.empty()) return arg_missing(c, "animation", "the animation's name (made when missing)");
    if (base.target.empty()) return arg_missing(c, "target", "the bone, slot or IK constraint the key belongs to");
    StringView channel;
    if (!arg_string(c, "channel", channel, true)) return false;
    if (channel == "rotate")
        base.channel = sk::Channel::Rotate;
    else if (channel == "translate")
        base.channel = sk::Channel::Translate;
    else if (channel == "scale")
        base.channel = sk::Channel::Scale;
    else if (channel == "color")
        base.channel = sk::Channel::SlotColor;
    else if (channel == "attachment")
        base.channel = sk::Channel::SlotAttachment;
    else if (channel == "ik_mix")
        base.channel = sk::Channel::IkMix;
    else
        return arg_missing(
            c, "channel", "rotate, translate, scale (a bone), color, attachment (a slot) or ik_mix (an IK constraint)");
    if (!in.number("duration", base.duration) || !in.boolean("loop", base.has_loop, base.loop)) return false;
    base.has_duration = arg(c, "duration").valid();

    Array<sk::KeySpec> keys;
    const cook::JsonValue list = arg(c, "keys");
    if (list.valid()) {
        if (!list.is_array() || list.size() == 0 || list.size() > 256)
            return arg_missing(c, "keys", "an array of one to 256 key objects");
        for (u32 i = 0; i < list.size(); ++i) {
            char ctx[32];
            std::snprintf(ctx, sizeof(ctx), "(in keys[%u]) ", i);
            if (!list.at(i).is_object()) return arg_missing(c, "keys", "an array of key objects");
            sk::KeySpec k = base;
            if (!parse_key(c, list.at(i), ctx, k)) return false;
            keys.push_back(k);
        }
    } else {
        sk::KeySpec k = base;
        if (!parse_key(c, c.args, "", k)) return false;
        keys.push_back(k);
    }
    cook::String why;
    for (usize i = 0; i < keys.size(); ++i) {
        keys[i].has_duration = base.has_duration && i + 1 == keys.size();  // set once, on the last pass
        keys[i].has_loop = base.has_loop && i + 1 == keys.size();
        if (!sk::key(f.text, keys[i], why)) return edit_failed(c, why);
    }
    return save_edit(c, f, f.text.view());
}

// ---------------------------------------------------------------- skel2d_preview

struct Canvas {
    cook::Image img;
    f64 scale = 1, ox = 0, oy = 0;  // world -> pixel: x * scale + ox, oy - y * scale
    i32 px(f64 x) const { return static_cast<i32>(std::lround(x * scale + ox)); }
    i32 py(f64 y) const { return static_cast<i32>(std::lround(oy - y * scale)); }
    void dot(i32 x, i32 y, u32 rgba, i32 r) {
        for (i32 yy = y - r; yy <= y + r; ++yy)
            for (i32 xx = x - r; xx <= x + r; ++xx) {
                if (xx < 0 || yy < 0 || xx >= static_cast<i32>(img.width) || yy >= static_cast<i32>(img.height))
                    continue;
                u8* p = img.rgba.data() + (usize(yy) * img.width + usize(xx)) * 4;
                p[0] = static_cast<u8>(rgba >> 24);
                p[1] = static_cast<u8>(rgba >> 16);
                p[2] = static_cast<u8>(rgba >> 8);
                p[3] = 255;
            }
    }
    // A line between two world points, `r` pixels either side.
    void line(math::vec2 a, math::vec2 b, u32 rgba, i32 r) {
        const i32 x0 = px(a.x), y0 = py(a.y), x1 = px(b.x), y1 = py(b.y);
        const i32 n = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
        for (i32 i = 0; i <= n; ++i) {
            const f64 t = n ? static_cast<f64>(i) / n : 0.0;
            dot(static_cast<i32>(std::lround(x0 + (x1 - x0) * t)), static_cast<i32>(std::lround(y0 + (y1 - y0) * t)),
                rgba, r);
        }
    }
};

u32 pack_color(const f32 rgba[4]) {
    auto ch = [](f32 v) { return static_cast<u32>(std::lround(std::min(std::max(v, 0.0f), 1.0f) * 255.0f)); };
    return (ch(rgba[0]) << 24) | (ch(rgba[1]) << 16) | (ch(rgba[2]) << 8) | 255u;
}

// The corners of slot `s`'s shown quad in the skeleton's space; false for a hidden slot or a mesh.
bool slot_corners(const kit::Skeleton2DRegistry::Def& def, u32 s, Span<const kit::Bone2DTransform> world,
                  math::vec2 out[4]) {
    const asset::Skeleton2DSlot& slot = def.slots[s];
    if (slot.bone >= world.size()) return false;
    const f32* size = slot.size;
    const f32* pivot = slot.pivot;
    const f32* offset = slot.offset;
    f32 rotation = slot.rotation;
    if (slot.attachment_count > 0) {
        if (slot.attachment >= slot.attachment_count) return false;
        const usize index = usize(slot.first_attachment) + slot.attachment;
        if (index >= def.attachments.size() || def.attachments[index].kind != asset::Skeleton2DAttachmentKind::Quad)
            return false;
        const asset::Skeleton2DAttachment& a = def.attachments[index];
        size = a.size;
        pivot = a.pivot;
        offset = a.offset;
        rotation = a.rotation;
    }
    const f32 cs = std::cos(rotation), sn = std::sin(rotation);
    static constexpr f32 kCorner[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (u32 i = 0; i < 4; ++i) {
        const f32 lx = (kCorner[i][0] - pivot[0]) * size[0], ly = (kCorner[i][1] - pivot[1]) * size[1];
        out[i] = world[slot.bone].apply({offset[0] + cs * lx - sn * ly, offset[1] + sn * lx + cs * ly});
    }
    return true;
}

cook::Image draw_pose(const kit::Skeleton2DRegistry::Def& def, Span<const kit::Bone2DTransform> world,
                      Span<const editor::Skeleton2DIKTarget> targets, StringView label, u32 width, u32 height) {
    Canvas cv;
    cv.img = make_image(width, height, 0x1c1c22ffu);
    // Frame: every bone origin and tip, every slot corner and target, with the origin included.
    f64 lo_x = 0, hi_x = 0, lo_y = 0, hi_y = 0;
    auto grow = [&](math::vec2 p) {
        lo_x = std::min<f64>(lo_x, p.x);
        hi_x = std::max<f64>(hi_x, p.x);
        lo_y = std::min<f64>(lo_y, p.y);
        hi_y = std::max<f64>(hi_y, p.y);
    };
    for (usize i = 0; i < def.bones.size(); ++i) {
        grow(world[i].apply({0, 0}));
        grow(world[i].apply({def.bones[i].length, 0}));
    }
    math::vec2 q[4];
    for (u32 s = 0; s < def.slots.size(); ++s)
        if (slot_corners(def, s, world, q))
            for (const math::vec2& p : q) grow(p);
    for (const editor::Skeleton2DIKTarget& t : targets) grow(t.position);
    const f64 span_x = std::max(hi_x - lo_x, 0.5), span_y = std::max(hi_y - lo_y, 0.5);
    cv.scale = std::min((width * 0.8) / span_x, (height * 0.8) / span_y);
    cv.ox = width * 0.5 - (lo_x + hi_x) * 0.5 * cv.scale;
    cv.oy = height * 0.5 + (lo_y + hi_y) * 0.5 * cv.scale;

    cv.line({-100.0f, 0.0f}, {100.0f, 0.0f}, 0x33333cffu, 0);  // the skeleton's axes
    cv.line({0.0f, -100.0f}, {0.0f, 100.0f}, 0x33333cffu, 0);
    for (u32 s = 0; s < def.slots.size(); ++s) {
        if (!slot_corners(def, s, world, q)) continue;
        const u32 col = pack_color(def.slots[s].tint);
        for (u32 i = 0; i < 4; ++i) cv.line(q[i], q[(i + 1) % 4], col == 0xffffffffu ? 0xb0b0b8ffu : col, 0);
    }
    for (usize i = 0; i < def.bones.size(); ++i) {
        const math::vec2 a = world[i].apply({0, 0});
        const f32 len = def.bones[i].length > 0.0f ? def.bones[i].length : 0.08f;
        cv.line(a, world[i].apply({len, 0}), 0x59d9ffffu, 1);
        cv.dot(cv.px(a.x), cv.py(a.y), 0xffb040ffu, 3);
    }
    for (const editor::Skeleton2DIKTarget& t : targets) {
        cv.line({t.position.x - 0.05f, t.position.y}, {t.position.x + 0.05f, t.position.y}, 0xff5050ffu, 1);
        cv.line({t.position.x, t.position.y - 0.05f}, {t.position.x, t.position.y + 0.05f}, 0xff5050ffu, 1);
    }
    draw_label(cv.img, 6, 6, label, 2, 0xe0e0e8ffu);
    return cv.img;
}

bool skel2d_preview(ToolCall& c) {
    SkelFile f;
    if (!arg_skeleton(c, f)) return false;
    editor::EditorProject& project = c.server.session().project();
    StringView animation;
    if (!arg_string(c, "animation", animation, false)) return false;
    Fields in{c, c.args};
    f64 time = 0;
    if (!in.number("time", time)) return false;
    if (time < 0 || time > 3600) return arg_missing(c, "time", "seconds into the animation, a number in [0, 3600]");
    bool render = true;
    if (!arg_bool(c, "render", render)) return false;
    u64 width = 480, height = 480;
    if (!arg_uint(c, "width", width, 64, 2048, false) || !arg_uint(c, "height", height, 64, 2048, false)) return false;

    Array<editor::Skeleton2DIKTarget> targets;
    if (const cook::JsonValue list = arg(c, "ik_targets"); list.valid()) {
        if (!list.is_array() || list.size() > 16)
            return arg_missing(c, "ik_targets", "an array of {constraint, x, y, mix?}");
        for (u32 i = 0; i < list.size(); ++i) {
            char ctx[32];
            std::snprintf(ctx, sizeof(ctx), "(in ik_targets[%u]) ", i);
            Fields t{c, list.at(i), ctx};
            editor::Skeleton2DIKTarget tg;
            f64 x = 0, y = 0, mix = 1;
            if (!list.at(i).is_object() || !list.at(i).get("constraint").is_string())
                return arg_missing(c, "ik_targets", "an array of {constraint, x, y, mix?}");
            if (!t.string("constraint", tg.constraint) || !t.number("x", x) || !t.number("y", y) ||
                !t.number("mix", mix))
                return false;
            tg.position = {static_cast<f32>(x), static_cast<f32>(y)};
            tg.mix = static_cast<f32>(mix);
            targets.push_back(static_cast<editor::Skeleton2DIKTarget&&>(tg));
        }
    }

    // Cooked in memory from the file as saved: the same definition the runtime would load.
    cook::AssetRegistry registry;
    cook::DiagList scan;
    registry.scan(project.root(), project.importers(), false, scan);
    kit::Skeleton2DRegistry defs;
    cook::DiagList diags;
    if (!editor::cook_skeleton2d(project, registry, f.guid, f.rel.view(), f.text.view(), defs, diags)) {
        c.error += "the skeleton does not cook:";
        append_diags(c.error, diags, 10);
        return false;
    }
    const kit::Skeleton2DRegistry::Def* def = defs.resolve(f.guid);
    if (!def) return fail(c, "the skeleton does not load");
    Array<kit::Bone2DTransform> world;
    cook::String why;
    if (!editor::skeleton2d_pose(*def, animation, static_cast<f32>(time), targets.span(), world, why))
        return fail(c, why.view());

    f32 duration = 0;
    bool looped = false;
    for (usize i = 0; i < def->animations.size(); ++i)
        if (!animation.empty() && def->string(def->animations[i].name) == animation) {
            duration = def->animations[i].duration;
            looped = (def->animations[i].flags & asset::Skeleton2DAnimationFlags::Loop) != 0;
        }

    c.out.begin_object();
    c.out.key("skeleton").str(f.rel.view());
    c.out.key("animation");
    if (animation.empty())
        c.out.null();
    else
        c.out.str(animation);
    c.out.key("time").num_f(time).key("duration").num_f(duration).key("loop").boolean(looped);
    c.out.key("bones").begin_array();
    for (usize i = 0; i < def->bones.size(); ++i) {
        const asset::Skeleton2DBone& b = def->bones[i];
        const kit::Bone2DTransform& m = world[i];
        const math::vec2 tip = m.apply({b.length, 0});
        c.out.begin_object();
        c.out.key("name").str(def->string(b.name));
        c.out.key("parent");
        if (b.parent == asset::kSkeleton2DNoParent)
            c.out.null();
        else
            c.out.str(def->string(def->bones[b.parent].name));
        c.out.key("x")
            .num_f(m.x)
            .key("y")
            .num_f(m.y)
            .key("rotation")
            .num_f(std::atan2(static_cast<f64>(m.b), static_cast<f64>(m.a)) * kRadToDeg);
        c.out.key("tip").begin_array().num_f(tip.x).num_f(tip.y).end_array();
        c.out.key("world").begin_object();
        c.out.key("a")
            .num_f(m.a)
            .key("b")
            .num_f(m.b)
            .key("c")
            .num_f(m.c)
            .key("d")
            .num_f(m.d)
            .key("x")
            .num_f(m.x)
            .key("y")
            .num_f(m.y);
        c.out.end_object();
        c.out.end_object();
    }
    c.out.end_array();
    c.out.end_object();

    if (render && c.content) {
        cook::String label(animation.empty() ? StringView("rest") : animation);
        if (!animation.empty()) cook::appendf(label, " t=%.2f", time);
        const cook::Image img = draw_pose(*def, world.span(), targets.span(), label.view(), static_cast<u32>(width),
                                          static_cast<u32>(height));
        ToolContent item;
        item.mime = cook::String("image/png");
        if (!png_base64(img, item.base64)) return fail(c, "png encode failed");
        c.content->push_back(static_cast<ToolContent&&>(item));
    }
    return true;
}

#define MCE_SK_DOC \
    R"j("skeleton":{"type":"string","description":"The skeleton: a .mcskel2d path under assets/ or \"guid:<uuid>\""})j"

const ToolDef kSkeleton2DTools[] = {
    {"skel2d_create",
     "Make a new 2D skeleton (.mcskel2d, user guide 7.13) with a fresh GUID and one root bone. Then skel2d_add_bone, "
     "skel2d_add_slot, skel2d_add_ik and skel2d_key build the rig and its animations, and skel2d_preview poses it. "
     "Play it with a "
     "Skeleton2D component (component_add with {\"skeleton\": \"guid:<uuid>\"}) and a Skeleton2DAnimator for the "
     "clips.",
     R"j({"type":"object","properties":{"path":{"type":"string","description":"New .mcskel2d path under assets/, e.g. rigs/hero.mcskel2d"},)j"
     R"j("name":{"type":"string","maxLength":31,"description":"The skeleton's name (default: the file name)"},)j"
     R"j("root":{"type":"string","maxLength":31,"description":"The root bone's name (default root)"}},"required":["path"]})j",
     skel2d_create},
    {"skel2d_add_bone",
     "Add a bone, last in the list: parents must already exist (add them first). x, y (metres) and rotation (degrees) "
     "are the rest "
     "pose in the parent's space, scale a factor or [sx, sy], length the bone's length along its +x (gizmos and IK). "
     "At most 64 bones. "
     "The importer checks the result; an error names the file position and the bone.",
     R"j({"type":"object","properties":{)j" MCE_SK_DOC
     R"j(,"name":{"type":"string","maxLength":31},"parent":{"type":"string","description":"The parent bone (omit for another root)"},)j"
     R"j("x":{"type":"number"},"y":{"type":"number"},"rotation":{"type":"number","description":"Degrees"},)j"
     R"j("scale":{"type":["number","array"],"items":{"type":"number"},"description":"A factor or [sx, sy]"},)j"
     R"j("length":{"type":"number","minimum":0}},"required":["skeleton","name"]})j",
     skel2d_add_bone},
    {"skel2d_add_slot",
     "Add a slot (a textured quad drawn on a bone; slots draw in the order added, the first behind). Without "
     "'attachments' the slot "
     "is one quad: texture (\"guid:<uuid>\", or omit for a solid quad in the tint), rect [u0, v0, u1, v1], size [w, h] "
     "metres, pivot "
     "[0..1, 0..1] (0,0 = bottom-left), offset [x, y] in the bone's space where the pivot sits, rotation (degrees), "
     "tint [r, g, b, a] "
     "linear. With 'attachments' (an array of {\"name\", \"type\": \"quad\" | \"mesh\", ...} in the .mcskel2d form, "
     "asset_schema type "
     "Skeleton2D) the slot takes only its tint and 'attachment', the one shown first (null hides it).",
     R"j({"type":"object","properties":{)j" MCE_SK_DOC
     R"j(,"name":{"type":"string","maxLength":31},"bone":{"type":"string","description":"The bone the slot rides"},)j"
     R"j("texture":{"type":["string","null"]},"rect":{"type":"array","items":{"type":"number"},"minItems":4,"maxItems":4},)j"
     R"j("size":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2},)j"
     R"j("pivot":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2},)j"
     R"j("offset":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2},"rotation":{"type":"number","description":"Degrees"},)j"
     R"j("tint":{"type":"array","items":{"type":"number"},"minItems":4,"maxItems":4},)j"
     R"j("attachments":{"type":"array","items":{"type":"object"},"description":"Quad and mesh attachments"},)j"
     R"j("attachment":{"type":["string","null"],"description":"The attachment shown first (with 'attachments')"}},"required":["skeleton","name","bone"]})j",
     skel2d_add_slot},
    {"skel2d_add_ik",
     "Add an IK constraint: 'bones' is the chain, parent first, each the child of the one before (two bones: a "
     "two-bone solve; more, "
     "or type \"ccd\": CCD). 'target' is a bone outside the chain and not under its first bone; without it only an "
     "IKTarget2D entity "
     "drives the constraint. mix 0..1 (default 1), bend \"positive\" / \"negative\" (two-bone: which way the elbow "
     "bends), iterations "
     "1..64 (CCD), order (constraints apply in order). At most 16.",
     R"j({"type":"object","properties":{)j" MCE_SK_DOC
     R"j(,"name":{"type":"string","maxLength":31},"bones":{"type":"array","items":{"type":"string"},"minItems":1,"description":"The chain, parent first"},)j"
     R"j("target":{"type":"string"},"mix":{"type":"number","minimum":0,"maximum":1},"type":{"type":"string","enum":["two_bone","ccd"]},)j"
     R"j("bend":{"type":"string","enum":["positive","negative"]},"iterations":{"type":"integer","minimum":1,"maximum":64},)j"
     R"j("order":{"type":"integer"}},"required":["skeleton","name","bones"]})j",
     skel2d_add_ik},
    {"skel2d_key",
     "Key an animation channel at time t (seconds), creating the animation when it is missing; a key at the same time "
     "is replaced. "
     "channel: rotate (a bone; 'value' degrees added to its rest rotation), translate (a bone; x, y metres added), "
     "scale (a bone; x, y "
     "factors, default 1), color (a slot; 'color' [r, g, b, a] linear), attachment (a slot; 'attachment' the name "
     "shown, or null "
     "to hide it) or ik_mix (an IK constraint; 'value' the mix 0..1). 'target' names the bone, slot or constraint. "
     "'curve' is \"linear\" "
     "(default), \"stepped\" or a bezier [cx1, cy1, cx2, cy2] with cx in 0..1. Give one key's fields, or 'keys', an "
     "array of them. "
     "'duration' (seconds; default the last key) and 'loop' (default true) set the animation's.",
     R"j({"type":"object","properties":{)j" MCE_SK_DOC
     R"j(,"animation":{"type":"string","maxLength":31},"channel":{"type":"string","enum":["rotate","translate","scale","color","attachment","ik_mix"]},)j"
     R"j("target":{"type":"string"},"t":{"type":"number","minimum":0},"value":{"type":"number"},"x":{"type":"number"},"y":{"type":"number"},)j"
     R"j("color":{"type":"array","items":{"type":"number"},"minItems":4,"maxItems":4},"attachment":{"type":["string","null"]},)j"
     R"j("curve":{"type":["string","array"],"items":{"type":"number"}},)j"
     R"j("keys":{"type":"array","items":{"type":"object"},"description":"Several keys: each {t, value | x, y | color | attachment, curve?}"},)j"
     R"j("duration":{"type":"number","minimum":0},"loop":{"type":"boolean"}},"required":["skeleton","animation","channel","target"]})j",
     skel2d_key},
    {"skel2d_preview",
     "Pose a skeleton and return every bone's world transform (the skeleton's space: a, b, c, d, x, y, plus the "
     "origin, rotation in "
     "degrees and the tip at 'length'). With no 'animation' it is the rest pose; with one, that clip at 'time' seconds "
     "(a looping clip "
     "wraps), the keyed IK mixes included. 'ik_targets' ({constraint, x, y, mix?} in the skeleton's space) aim "
     "constraints as an "
     "IKTarget2D entity would. The numbers come from the runtime's own pose functions. Also returns an image: a line "
     "drawing of "
     "the bones and slot quads (not the textures; render_frame shows those in a scene); render false skips it.",
     R"j({"type":"object","properties":{)j" MCE_SK_DOC
     R"j(,"animation":{"type":"string","description":"An animation to sample (default: the rest pose)"},"time":{"type":"number","minimum":0,"maximum":3600},)j"
     R"j("ik_targets":{"type":"array","maxItems":16,"items":{"type":"object","properties":{"constraint":{"type":"string"},"x":{"type":"number"},"y":{"type":"number"},"mix":{"type":"number"}},"required":["constraint","x","y"]}},)j"
     R"j("render":{"type":"boolean","default":true},"width":{"type":"integer","minimum":64,"maximum":2048,"default":480},)j"
     R"j("height":{"type":"integer","minimum":64,"maximum":2048,"default":480}},"required":["skeleton"]})j",
     skel2d_preview},
};

#undef MCE_SK_DOC

}  // namespace

Span<const ToolDef> skeleton2d_tool_table() {
    return Span<const ToolDef>(kSkeleton2DTools, sizeof(kSkeleton2DTools) / sizeof(kSkeleton2DTools[0]));
}

}  // namespace mce::mcp
