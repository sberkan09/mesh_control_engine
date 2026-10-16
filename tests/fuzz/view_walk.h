// M-FUZZ 2: what a consumer does with a view the reader accepted. A reader may only be called safe when
// everything it let through can be followed: every string offset, every range into a sibling array, every
// index a draw or a player would take. `walk` does that for each chunk type and folds what it reads into a
// checksum, so the reads cannot be optimised away; under the ASan stage a stray offset is an out-of-bounds
// report, in the plain stage it is a wild value the checks below refuse.
#pragma once

#include <cstdint>

#include "mce/asset/formats.h"
#include "mce/asset/vorbis.h"

namespace mce::fuzz {

struct Sink {
    volatile std::uint64_t acc = 0;
    void eat(std::uint64_t v) { acc = acc * 1099511628211ull + v; }
    void eat(StringView s) {
        eat(s.size());
        if (!s.empty()) eat(static_cast<unsigned char>(s.front())), eat(static_cast<unsigned char>(s.back()));
    }
    void eat(Span<const byte> s) {
        eat(s.size());
        if (!s.empty()) eat(static_cast<std::uint64_t>(s.front())), eat(static_cast<std::uint64_t>(s.back()));
    }
};

using namespace mce::asset;

inline void walk(Sink& s, const TextureView& v) {
    for (u32 layer = 0; layer < v.header->layers; ++layer)
        for (u32 mip = 0; mip < v.mips.size(); ++mip) s.eat(v.pixels(mip, layer));
}

inline void walk(Sink& s, const SfxView& v) {
    for (f32 f : v.samples) s.eat(static_cast<std::uint64_t>(f > 0.f));
    s.eat(v.vorbis);
    if (!v.vorbis.empty()) {  // the decoder is the parser of the stream inside the chunk
        Array<f32> pcm;
        u32 channels = 0, rate = 0;
        if (decode_vorbis(v.vorbis, pcm, channels, rate)) s.eat(pcm.size());
    }
    for (const Guid& g : v.set_clips) s.eat(g.hi ^ g.lo);
}

inline void walk(Sink& s, const FontView& v) {
    for (const FontGlyph& g : v.glyphs) s.eat(v.find(g.codepoint) != nullptr);
    for (const FontKern& k : v.kerning) s.eat(static_cast<std::uint64_t>(v.kern(k.left, k.right) > 0.f));
    s.eat(v.atlas);
}

inline void walk(Sink& s, const TilemapView& v) {
    for (u32 t : v.tiles) s.eat(t);
}

inline void walk(Sink& s, const AnimGraphView& v) {
    for (const AnimGraphState& st : v.states) {
        s.eat(AnimGraphView::text(st.name, sizeof st.name));
        s.eat(v.find_state(AnimGraphView::text(st.name, sizeof st.name)));
        if (st.blend_count > 0) {
            if (st.blend_2d)
                for (u32 i = 0; i < st.blend_count; ++i) s.eat(static_cast<std::uint64_t>(v.blend2d[st.first_blend + i].x > 0.f));
            else
                for (u32 i = 0; i < st.blend_count; ++i) s.eat(static_cast<std::uint64_t>(v.blend_points[st.first_blend + i].at > 0.f));
        }
        for (u32 i = 0; i < st.event_count; ++i) s.eat(v.events[st.first_event + i].id);
    }
    for (const AnimGraphTransition& t : v.transitions) {
        if (t.from != kAnimAnyState) s.eat(v.states[t.from].speed > 0.f);
        s.eat(v.states[t.to].speed > 0.f);
        for (u32 i = 0; i < t.condition_count; ++i) s.eat(v.conditions[t.first_condition + i].param);
    }
    for (const AnimGraphLayer& l : v.layers)
        for (u32 i = 0; i < l.mask_count; ++i) s.eat(AnimGraphView::text(v.mask_bones[l.first_mask + i].bone, 32));
    s.eat(v.root_bone());
}

inline void walk(Sink& s, const TerrainView& v) {
    const f32 w = v.header->size[0], d = v.header->size[1];
    for (f32 fx : {0.f, 0.37f, 0.99f, 1.f, -3.f, 7.f})
        for (f32 fz : {0.f, 0.61f, 1.f, 9.f}) {
            s.eat(static_cast<std::uint64_t>(v.height_at(fx * w, fz * d) > 0.f));
            s.eat(static_cast<std::uint64_t>(v.surface_at(fx * w, fz * d) > 0.f));
        }
    const u32 r = v.resolution();
    for (u32 z = 0; z < r; z += (r / 8 ? r / 8 : 1))
        for (u32 x = 0; x < r; x += (r / 8 ? r / 8 : 1)) s.eat(v.sample(x, z) > 0.f), s.eat(v.hole(x, z));
    const u32 sr = v.splat_resolution();
    if (!v.layers.empty() && sr > 0)
        for (u32 l = 0; l < v.layers.size(); ++l) s.eat(v.weight(0, 0, l)), s.eat(v.weight(sr - 1, sr - 1, l));
}

inline void walk(Sink& s, const SequenceView& v) {
    s.eat(v.name());
    for (const SequenceStep& st : v.steps) s.eat(st.id);
}

inline void walk(Sink& s, const TimelineView& v) {
    s.eat(v.name());
    for (const TimelineTrack& t : v.tracks) {
        s.eat(v.string(t.path));
        for (const TimelineKey& k : v.keys_of(t)) s.eat(v.string(k.text)), s.eat(v.string(k.text2));
    }
    for (const TimelineKey& k : v.keys) s.eat(v.string(k.text)), s.eat(v.string(k.text2));
}

inline void walk(Sink& s, const BtView& v) {
    s.eat(v.name());
    for (const BtKey& k : v.keys) s.eat(v.string(k.name));
    for (const BtNode& n : v.nodes) {
        s.eat(v.string(n.name)), s.eat(v.string(n.text)), s.eat(v.string(n.arg));
        if (n.key < v.keys.size()) s.eat(v.keys[n.key].slot);
        s.eat(v.nodes.subspan(0, n.end).size());
    }
    for (const BtConsideration& c : v.considerations)
        if (c.key < v.keys.size()) s.eat(v.keys[c.key].id);
}

inline void walk(Sink& s, const Skeleton2DView& v) {
    s.eat(v.name());
    for (const auto& b : v.bones) {
        s.eat(v.string(b.name));
        if (b.parent != kSkeleton2DNoParent) s.eat(v.bones[b.parent].name);
    }
    for (const auto& sl : v.slots) {
        s.eat(v.string(sl.name));
        s.eat(v.bones[sl.bone].name);
        for (u32 i = 0; i < sl.attachment_count; ++i) s.eat(v.string(v.attachments[sl.first_attachment + i].name));
    }
    for (const auto& a : v.attachments) {
        s.eat(v.string(a.name));
        if (a.kind != Skeleton2DAttachmentKind::Mesh) continue;  // only a mesh owns vertices and indices
        for (u32 i = 0; i < a.vertex_count; ++i) {
            const Skeleton2DVertex& vx = v.vertices[a.first_vertex + i];
            for (u32 w = 0; w < vx.weight_count; ++w) s.eat(v.bones[v.weights[vx.first_weight + w].bone].name);
        }
        for (u32 i = 0; i < a.index_count; ++i) s.eat(v.indices[a.first_index + i]);
    }
    for (const auto& an : v.animations) {
        s.eat(v.string(an.name));
        for (u32 i = 0; i < an.event_count; ++i) s.eat(v.string(v.events[an.first_event + i].name));
        for (u32 i = 0; i < an.timeline_count; ++i) {
            const Skeleton2DTimeline& t = v.timelines[an.first_timeline + i];
            for (u32 k = 0; k < t.key_count; ++k) {
                // Color, Attachment and Deform timelines store slot keys; the others store plain keys.
                if (t.channel == Skeleton2DChannel::Color || t.channel == Skeleton2DChannel::Attachment ||
                    t.channel == Skeleton2DChannel::Deform)
                    s.eat(v.slot_keys[t.first_key + k].value);
                else
                    s.eat(v.keys[t.first_key + k].curve == Skeleton2DCurve::Stepped);
            }
        }
    }
    for (const auto& c : v.ik) {
        s.eat(v.string(c.name));
        s.eat(v.bones[c.bone].name);
        if (c.target != kSkeleton2DNoTarget) s.eat(v.bones[c.target].name);
    }
    s.eat(static_cast<std::uint64_t>(v.find_bone("root")));
    s.eat(static_cast<std::uint64_t>(v.find_animation("idle")));
    s.eat(static_cast<std::uint64_t>(v.find_slot("body")));
}

inline void walk(Sink& s, const ShaderView& v) {
    for (u32 i = 0; i < v.header->define_count; ++i) s.eat(v.define(i));
    Array<char> text;
    for (const ShaderVariant& var : v.variants) {
        for (u32 w : v.spirv(var)) s.eat(w);
        for (const ShaderResource& r : v.resources_of(var)) s.eat(r.binding);
        s.eat(v.find(var.mask) != nullptr);
    }
    v.compose(0, text);
    s.eat(text.size());
}

inline void walk(Sink& s, const MeshView& v) {
    for (u32 si = 0; si < v.submeshes.size(); ++si) {
        const MeshSubmesh& sm = v.submeshes[si];
        s.eat(v.node_name(sm.node));
        for (u32 i = 0; i < sm.index_count; ++i) {
            const u32 idx = v.index(sm.first_index + i);
            s.eat(v.vertices[sm.base_vertex + idx].position[0]);
            if (!v.skin_vertices.empty()) {
                const MeshSkinVertex& sv = v.skin_vertices[sm.base_vertex + idx];
                const i32 skin = v.submesh_skins[si];
                if (skin >= 0)
                    for (u32 j = 0; j < 4; ++j)
                        if (sv.weights[j]) s.eat(v.joints[v.skins[usize(skin)].first_joint + sv.joints[j]].node);
            }
        }
        for (const MeshLod& l : v.submesh_lods(si))
            for (u32 i = 0; i < l.index_count; ++i) s.eat(v.vertices[l.base_vertex + v.index(l.first_index + i)].position[0]);
        s.eat(static_cast<std::uint64_t>(v.material_ext(si).any()));
    }
    for (u32 n = 0; n < v.nodes.size(); ++n) {
        s.eat(v.node_name(n));
        if (v.nodes[n].parent >= 0) s.eat(v.nodes[usize(v.nodes[n].parent)].name_size);
    }
    for (u32 c = 0; c < v.clips.size(); ++c) {
        s.eat(v.clip_name(c));
        s.eat(v.find_clip(v.clip_name(c)));
        const MeshClip& cl = v.clips[c];
        for (u32 i = 0; i < cl.channel_count; ++i) {
            const MeshChannel& ch = v.channels[cl.first_channel + i];
            s.eat(v.node_name(ch.node));
            for (u32 k = 0; k < ch.key_count; ++k) s.eat(static_cast<std::uint64_t>(v.keys[ch.times_offset + k] > 0.f));
            const u32 width = (ch.path == AnimPath::Rotation ? 4u : 3u) * (ch.interp == AnimInterp::CubicSpline ? 3u : 1u);
            for (u32 k = 0; k < ch.key_count * width; ++k) s.eat(static_cast<std::uint64_t>(v.keys[ch.values_offset + k] > 0.f));
        }
    }
}

inline void walk(Sink& s, const SceneView& v) {
    for (u32 e = 0; e < v.header->entity_count; ++e) s.eat(v.entity_name(e));
    for (u32 c = 0; c < v.columns.size(); ++c) {
        for (u32 row : v.rows(c)) {
            s.eat(row);
            s.eat(row < v.header->entity_count);
        }
        s.eat(v.data(c));
        s.eat(v.find_column(v.columns[c].type_id) != nullptr);
    }
    for (const SceneFixup& f : v.asset_fixups) {
        const SceneColumn& col = v.columns[f.column];
        s.eat(v.data(f.column).subspan(usize(f.row) * col.type_size + f.byte_offset, sizeof(Guid)));
        s.eat(v.asset_refs[f.index].type_id);
    }
    for (const SceneFixup& f : v.entity_fixups) {
        const SceneColumn& col = v.columns[f.column];
        s.eat(v.data(f.column).subspan(usize(f.row) * col.type_size + f.byte_offset, sizeof(u32)));
    }
}

}  // namespace mce::fuzz
