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
//      "rotation": 0, "tint": [1, 1, 1, 1]}]
// }
//
// Metres and degrees in the text, Y up; the chunk stores radians. A bone's x, y and rotation are
// its rest pose in its parent's space; "scale" is [sx, sy] or one number. A slot's offset and
// rotation place the quad's pivot in its bone's space. A null or absent texture draws a solid quad
// in the slot's tint. At most 64 bones (kit::Skeleton2D holds one pose per bone) and 256 slots.

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

class Skeleton2DImporter final : public IImporter {
public:
    StringView id() const override { return "skeleton2d"; }
    u32 version() const override { return 1; }
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
        if (!detail::check_keys(doc, {"format", "version", "guid", "name", "bones", "slots"}, r.file, kStage, diags))
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

        h.bone_count = static_cast<u32>(bones.size());
        h.slot_count = static_cast<u32>(slots.size());
        h.string_bytes = static_cast<u32>(pool.size());
        Array<byte> payload;
        auto append = [&](const void* p, usize n) {
            const usize at = payload.size();
            payload.resize(at + n, byte{0});
            if (n) std::memcpy(payload.data() + at, p, n);
        };
        append(&h, sizeof(h));
        append(bones.data(), bones.size() * sizeof(Skeleton2DBone));
        append(slots.data(), slots.size() * sizeof(Skeleton2DSlot));
        append(pool.data(), pool.size());

        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::Skeleton2D;
        d.version = asset::chunk_version(asset::SKELETON2D_VERSION, 0);
        d.guid = in.guid;
        out.add(d, payload.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_skeleton2d_importer() { return mem::make_unique<Skeleton2DImporter>(mem::heap()); }

}  // namespace mce::cook
