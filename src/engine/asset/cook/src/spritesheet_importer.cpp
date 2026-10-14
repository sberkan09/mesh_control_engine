// .mcsheet -> CHNK_SHEET: regions of one texture and named flipbook clips over them (M-DATA-REACH 5;
// the kit's AnimatedSprite plays them, Sprite.region shows one). The texture GUID is a dependency
// (a missing or non-texture reference is a cook error naming file:line:col).
//
// {
//   "format": "mce-spritesheet", "version": 1, "guid": "<uuid>", "name": "hero",
//   "texture": "guid:<texture uuid>" | null,
//   "grid": {"columns": 8, "rows": 4, "count": 30},     // equal cells, left to right from the top row
//   "image_size": [256, 128],                            // pixels; needed by "regions"
//   "regions": [{"name": "hurt", "rect": [0, 96, 32, 32]}],  // pixel rects, y down from the top;
//                                                        // numbered after the grid's cells
//   "clips": [{"name": "run", "frames": [0, 1, 2, "hurt"] | {"from": 0, "to": 7},
//              "fps": 12, "mode": "Loop" | "PingPong" | "Once",
//              "events": [{"frame": 2, "name": "footstep"}]}]   // frame: index within the clip
// }

#include <cstring>

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/hash.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.spritesheet";
constexpr u32 kSheetFormatVersion = 1;
constexpr u32 kMaxRegions = 65536;
constexpr u32 kMaxFrames = 65535;

struct NamedRegion {
    String name;
    u32 index = 0;
};

bool whole(JsonValue v, f64 lo, f64 hi, u32& out) {
    if (!v.is_number()) return false;
    const f64 d = v.as_number();
    if (d < lo || d > hi || d != static_cast<f64>(static_cast<u32>(d))) return false;
    out = static_cast<u32>(d);
    return true;
}

void copy_name(char* dst, usize cap, StringView s) {
    std::memset(dst, 0, cap);
    std::memcpy(dst, s.data(), s.size() < cap ? s.size() : cap - 1);
}

class SpriteSheetImporter final : public IImporter {
public:
    StringView id() const override { return "spritesheet"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::SpriteSheet; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ".mcsheet"; }

    void scan_dependencies(const ImportInput& in, Array<DepRef>& out, DiagList& diags) const override {
        const JsonValue tex = in.settings.get("texture");
        if (!tex.valid() || tex.is_null()) return;
        Guid g;
        if (!tex.is_string() || !parse_guid_ref(tex.as_string(), g)) {
            detail::error_at(diags, Errc::ParseError, in.display_path, tex, kStage, "'texture' must be \"guid:<uuid>\" or null");
            return;
        }
        DepRef d;
        d.guid = g;
        d.expected = ChunkType::Texture;
        d.line = tex.line();
        d.col = tex.col();
        d.where.assign("texture");
        out.push_back(d);
    }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        const JsonValue doc = in.settings;
        const StringView file = in.display_path;
        auto fail = [&](JsonValue at, StringView msg) {
            detail::error_at(diags, Errc::ParseError, file, at.valid() ? at : doc, kStage, msg);
            return false;
        };
        if (!detail::check_keys(doc, {"format", "version", "guid", "name", "texture", "grid", "image_size", "regions", "clips"}, file,
                                kStage, diags))
            return false;
        const JsonValue fmt = doc.get("format");
        if (!fmt.is_string() || fmt.as_string() != "mce-spritesheet") return fail(fmt, "\"format\" must be \"mce-spritesheet\"");
        const JsonValue ver = doc.get("version");
        if (!ver.is_integer() || ver.as_number() != kSheetFormatVersion) {
            String msg;
            appendf(msg, "unsupported sprite sheet version (this cook reads version %u)", kSheetFormatVersion);
            detail::error_at(diags, Errc::VersionTooNew, file, ver.valid() ? ver : doc, kStage, msg);
            return false;
        }

        asset::SpriteSheetHeader h{};
        if (const JsonValue t = doc.get("texture"); t.is_string()) {
            Guid g;
            if (parse_guid_ref(t.as_string(), g)) {
                h.texture[0] = g.hi;
                h.texture[1] = g.lo;
            }
        }

        // Regions: the grid's cells, then the named pixel rects.
        Array<asset::SpriteSheetRegion> regions;
        Array<NamedRegion> names;
        if (const JsonValue grid = doc.get("grid"); grid.valid()) {
            if (!grid.is_object()) return fail(grid, "'grid' must be {\"columns\": n, \"rows\": n, \"count\": n}");
            if (!detail::check_keys(grid, {"columns", "rows", "count"}, file, kStage, diags)) return false;
            u32 cols = 0, rows = 0, count = 0;
            if (!whole(grid.get("columns"), 1, 1024, cols)) return fail(grid.get("columns"), "'grid.columns' must be a whole number from 1 to 1024");
            if (!whole(grid.get("rows"), 1, 1024, rows)) return fail(grid.get("rows"), "'grid.rows' must be a whole number from 1 to 1024");
            count = cols * rows;
            if (grid.get("count").valid() && !whole(grid.get("count"), 1, cols * rows, count))
                return fail(grid.get("count"), "'grid.count' must be a whole number from 1 to columns x rows");
            for (u32 i = 0; i < count; ++i) {
                const u32 c = i % cols, r = i / cols;
                regions.push_back({static_cast<f32>(c) / static_cast<f32>(cols), static_cast<f32>(r) / static_cast<f32>(rows),
                                   static_cast<f32>(c + 1) / static_cast<f32>(cols), static_cast<f32>(r + 1) / static_cast<f32>(rows)});
            }
        }
        if (const JsonValue list = doc.get("regions"); list.valid()) {
            const JsonValue size = doc.get("image_size");
            u32 iw = 0, ih = 0;
            if (!size.is_array() || size.size() != 2 || !whole(size.at(0), 1, 65536, iw) || !whole(size.at(1), 1, 65536, ih))
                return fail(size, "'regions' needs 'image_size': [width, height] in pixels");
            if (!list.is_array()) return fail(list, "'regions' must be an array of {\"name\", \"rect\": [x, y, w, h]}");
            for (u32 i = 0; i < list.size(); ++i) {
                const JsonValue r = list.at(i);
                if (!r.is_object() || !detail::check_keys(r, {"name", "rect"}, file, kStage, diags))
                    return fail(r, "a region is {\"name\": \"...\", \"rect\": [x, y, w, h]}");
                const JsonValue rect = r.get("rect");
                u32 x = 0, y = 0, w = 0, hh = 0;
                if (!rect.is_array() || rect.size() != 4 || !whole(rect.at(0), 0, iw - 1, x) || !whole(rect.at(1), 0, ih - 1, y) ||
                    !whole(rect.at(2), 1, iw - x, w) || !whole(rect.at(3), 1, ih - y, hh))
                    return fail(rect, "'rect' must be [x, y, width, height] in whole pixels inside image_size (y down)");
                regions.push_back({static_cast<f32>(x) / static_cast<f32>(iw), static_cast<f32>(y) / static_cast<f32>(ih),
                                   static_cast<f32>(x + w) / static_cast<f32>(iw), static_cast<f32>(y + hh) / static_cast<f32>(ih)});
                if (const JsonValue n = r.get("name"); n.valid()) {
                    if (!n.is_string() || n.as_string().empty()) return fail(n, "a region's 'name' must be a non-empty string");
                    for (const NamedRegion& e : names)
                        if (e.name.view() == n.as_string()) return fail(n, "two regions share this name");
                    names.push_back(NamedRegion{String(n.as_string()), static_cast<u32>(regions.size() - 1)});
                }
            }
        } else if (doc.get("image_size").valid() && !doc.get("image_size").is_array()) {
            return fail(doc.get("image_size"), "'image_size' must be [width, height] in pixels");
        }
        if (regions.size() > kMaxRegions) return fail(doc, "a sprite sheet holds at most 65536 regions");
        const u32 region_count = static_cast<u32>(regions.size());

        // Clips.
        Array<asset::SpriteSheetClip> clips;
        Array<u32> frames;
        Array<asset::SpriteSheetEvent> events;
        const JsonValue clip_list = doc.get("clips");
        if (clip_list.valid() && !clip_list.is_array()) return fail(clip_list, "'clips' must be an array");
        for (u32 ci = 0; clip_list.valid() && ci < clip_list.size(); ++ci) {
            const JsonValue c = clip_list.at(ci);
            if (!c.is_object() || !detail::check_keys(c, {"name", "frames", "fps", "mode", "events"}, file, kStage, diags))
                return fail(c, "a clip is {\"name\", \"frames\", \"fps\", \"mode\", \"events\"}");
            asset::SpriteSheetClip clip{};
            const JsonValue name = c.get("name");
            if (!name.is_string() || name.as_string().empty() || name.as_string().size() >= sizeof(clip.name))
                return fail(name, "a clip's 'name' must be a string of 1 to 31 characters");
            for (const asset::SpriteSheetClip& other : clips)
                if (asset::SpriteSheetView::name_of(other) == name.as_string()) return fail(name, "two clips share this name");
            copy_name(clip.name, sizeof(clip.name), name.as_string());
            clip.first_frame = static_cast<u32>(frames.size());
            const JsonValue fr = c.get("frames");
            auto add_frame = [&](JsonValue v) {
                u32 idx = 0;
                if (v.is_string()) {
                    for (const NamedRegion& e : names)
                        if (e.name.view() == v.as_string()) {
                            frames.push_back(e.index);
                            return true;
                        }
                    return fail(v, "no region has this name");
                }
                if (region_count == 0 || !whole(v, 0, region_count - 1, idx)) {
                    String msg;
                    appendf(msg, "a frame must be a region index from 0 to %u or a region name", region_count == 0 ? 0 : region_count - 1);
                    return fail(v, msg.view());
                }
                frames.push_back(idx);
                return true;
            };
            if (fr.is_array()) {
                for (u32 i = 0; i < fr.size(); ++i)
                    if (!add_frame(fr.at(i))) return false;
            } else if (fr.is_object()) {
                if (!detail::check_keys(fr, {"from", "to"}, file, kStage, diags)) return false;
                u32 from = 0, to = 0;
                if (region_count == 0 || !whole(fr.get("from"), 0, region_count - 1, from) || !whole(fr.get("to"), 0, region_count - 1, to))
                    return fail(fr, "'frames' {\"from\", \"to\"} must name region indices of this sheet");
                for (u32 i = from;; i = from <= to ? i + 1 : i - 1) {  // to < from plays backwards
                    frames.push_back(i);
                    if (i == to) break;
                }
            } else {
                return fail(fr, "'frames' must be an array of region indices or names, or {\"from\": a, \"to\": b}");
            }
            clip.frame_count = static_cast<u32>(frames.size()) - clip.first_frame;
            if (clip.frame_count == 0 || clip.frame_count > kMaxFrames) return fail(fr, "a clip has 1 to 65535 frames");
            f64 fps = 12.0;
            detail::read_number(c, "fps", 12.0, fps, file, kStage, diags);
            if (!(fps > 0.0) || fps > 240.0) return fail(c.get("fps"), "'fps' must be above 0 and at most 240");
            clip.fps = static_cast<f32>(fps);
            if (const JsonValue m = c.get("mode"); m.valid()) {
                const StringView ms = m.is_string() ? m.as_string() : StringView();
                if (ms == "Loop") clip.mode = 0;
                else if (ms == "PingPong") clip.mode = 1;
                else if (ms == "Once") clip.mode = 2;
                else return fail(m, "'mode' must be \"Loop\", \"PingPong\" or \"Once\"");
            }
            clip.first_event = static_cast<u32>(events.size());
            if (const JsonValue ev = c.get("events"); ev.valid()) {
                if (!ev.is_array()) return fail(ev, "'events' must be an array of {\"frame\", \"name\"}");
                for (u32 i = 0; i < ev.size(); ++i) {
                    const JsonValue e = ev.at(i);
                    if (!e.is_object() || !detail::check_keys(e, {"frame", "name"}, file, kStage, diags))
                        return fail(e, "an event is {\"frame\": n, \"name\": \"...\"}");
                    asset::SpriteSheetEvent out_ev{};
                    u32 f = 0;
                    if (!whole(e.get("frame"), 0, clip.frame_count - 1, f)) return fail(e.get("frame"), "'frame' must be a frame index within the clip");
                    const JsonValue en = e.get("name");
                    if (!en.is_string() || en.as_string().empty() || en.as_string().size() >= sizeof(out_ev.name))
                        return fail(en, "an event's 'name' must be a string of 1 to 23 characters");
                    out_ev.frame = static_cast<u16>(f);
                    out_ev.id = static_cast<u32>(fnv1a64(en.as_string()));
                    copy_name(out_ev.name, sizeof(out_ev.name), en.as_string());
                    events.push_back(out_ev);
                }
            }
            clip.event_count = static_cast<u32>(events.size()) - clip.first_event;
            clips.push_back(clip);
        }

        h.region_count = region_count;
        h.clip_count = static_cast<u32>(clips.size());
        h.frame_count = static_cast<u32>(frames.size());
        h.event_count = static_cast<u32>(events.size());
        Array<byte> payload;
        auto append = [&](const void* p, usize n) {
            const usize at = payload.size();
            payload.resize(at + n, byte{0});
            if (n) std::memcpy(payload.data() + at, p, n);
        };
        append(&h, sizeof(h));
        append(regions.data(), regions.size() * sizeof(asset::SpriteSheetRegion));
        append(clips.data(), clips.size() * sizeof(asset::SpriteSheetClip));
        append(frames.data(), frames.size() * sizeof(u32));
        append(events.data(), events.size() * sizeof(asset::SpriteSheetEvent));

        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::SpriteSheet;
        d.version = asset::chunk_version(asset::SPRITESHEET_VERSION, 0);
        d.guid = in.guid;
        out.add(d, payload.span());
        return true;
    }
};

}  // namespace

mem::UniquePtr<IImporter> make_spritesheet_importer() { return mem::make_unique<SpriteSheetImporter>(mem::heap()); }

}  // namespace mce::cook
