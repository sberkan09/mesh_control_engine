#include "mce/asset/formats.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mce::asset {

namespace {

bool in_bounds(u64 off, u64 size, u64 limit) { return off <= limit && size <= limit - off; }

// Bounds- and alignment-checked typed array at `offset` inside `payload`.
template <class T>
bool span_at(Span<const byte> payload, u64 offset, u64 count, Span<const T>& out) {
    if (count > (~u64(0)) / sizeof(T)) return false;
    if (!in_bounds(offset, count * sizeof(T), payload.size())) return false;
    if (offset % alignof(T) != 0) return false;
    out = Span<const T>(reinterpret_cast<const T*>(payload.data() + offset), static_cast<usize>(count));
    return true;
}

template <class T>
const T* header_of(Span<const byte> payload) {
    if (payload.size() < sizeof(T)) return nullptr;
    if (reinterpret_cast<std::uintptr_t>(payload.data()) % alignof(T) != 0) return nullptr;
    return reinterpret_cast<const T*>(payload.data());
}

// Appends 16-byte aligned blocks and patches offsets into earlier blocks.
class PayloadBuilder {
public:
    explicit PayloadBuilder(Array<byte>& out) : out_(out) { out_.clear(); }
    usize append(const void* data, usize size) {
        const usize at = align_up(out_.size(), CHUNK_ALIGN);
        out_.resize(at + size, byte{0});
        if (size) std::memcpy(out_.data() + at, data, size);
        return at;
    }
    template <class T>
    usize append_span(Span<const T> s) {
        return append(s.data(), s.size_bytes());
    }
    template <class T>
    void patch(usize at, const T& v) {
        std::memcpy(out_.data() + at, &v, sizeof(T));
    }
    void finish() { out_.resize(align_up(out_.size(), CHUNK_ALIGN), byte{0}); }

private:
    Array<byte>& out_;
};

Error bad(const char* stage, u32 detail = 0) { return Error(Errc::OutOfBounds, stage, {}, detail); }

}  // namespace

// ------------------------------------------------------------------ manifest

Result<ManifestView> ManifestView::read(Span<const byte> payload) {
    constexpr const char* kStage = "asset.manifest.read";
    const auto* h = header_of<ManifestHeader>(payload);
    if (!h) return Error(Errc::Truncated, kStage);
    Span<const ManifestEntry> entries;
    if (!span_at(payload, sizeof(ManifestHeader), h->entry_count, entries)) return bad(kStage);
    const u64 strings_at = sizeof(ManifestHeader) + static_cast<u64>(h->entry_count) * sizeof(ManifestEntry);
    if (!in_bounds(strings_at, h->strings_size, payload.size())) return bad(kStage);
    for (u32 i = 0; i < h->entry_count; ++i) {
        const ManifestEntry& e = entries[i];
        if (!in_bounds(e.path_offset, e.path_size, h->strings_size)) return bad(kStage, i);
        if (i > 0 && !(entries[i - 1].guid() < e.guid())) return Error(Errc::DuplicateGuid, kStage, {}, i);
    }
    ManifestView v;
    v.entries_ = entries.data();
    v.strings_ = reinterpret_cast<const char*>(payload.data() + strings_at);
    v.count_ = h->entry_count;
    return v;
}

const ManifestEntry& ManifestView::entry(u32 i) const {
    MCE_ASSERT_MSG(i < count_, "ManifestView::entry out of range");
    return entries_[i];
}

StringView ManifestView::path(u32 i) const {
    const ManifestEntry& e = entry(i);
    return StringView(strings_ + e.path_offset, e.path_size);
}

u32 ManifestView::find(Guid g) const {
    const ManifestEntry* end = entries_ + count_;
    const ManifestEntry* it = std::lower_bound(entries_, end, g, [](const ManifestEntry& e, Guid key) { return e.guid() < key; });
    return (it != end && it->guid() == g) ? static_cast<u32>(it - entries_) : count_;
}

void write_manifest(Span<ManifestItem> items, Array<byte>& out) {
    std::sort(items.begin(), items.end(), [](const ManifestItem& a, const ManifestItem& b) { return a.guid < b.guid; });
    Array<ManifestEntry> entries;
    Array<char> strings;
    entries.reserve(items.size());
    for (usize i = 0; i < items.size(); ++i) {
        const ManifestItem& it = items[i];
        MCE_ASSERT_MSG(i == 0 || items[i - 1].guid != it.guid, "write_manifest: duplicate GUID");
        ManifestEntry e{};
        e.guid_hi = it.guid.hi;
        e.guid_lo = it.guid.lo;
        e.content_hash = it.content_hash;
        e.type = it.type;
        e.path_offset = static_cast<u32>(strings.size());
        e.path_size = static_cast<u32>(it.path.size());
        for (char c : it.path) strings.push_back(c);
        entries.push_back(e);
    }
    ManifestHeader h{};
    h.entry_count = static_cast<u32>(entries.size());
    h.strings_size = static_cast<u32>(strings.size());
    out.clear();
    const usize total = sizeof(h) + entries.size() * sizeof(ManifestEntry) + strings.size();
    out.resize(align_up(total, CHUNK_ALIGN), byte{0});
    std::memcpy(out.data(), &h, sizeof(h));
    if (!entries.empty()) std::memcpy(out.data() + sizeof(h), entries.data(), entries.size() * sizeof(ManifestEntry));
    if (!strings.empty())
        std::memcpy(out.data() + sizeof(h) + entries.size() * sizeof(ManifestEntry), strings.data(), strings.size());
}

// ------------------------------------------------------------------ texture

u64 texture_level_bytes(TextureFormat format, u32 width, u32 height) {
    switch (format) {
        case TextureFormat::RGBA8: return static_cast<u64>(width) * height * 4;
        case TextureFormat::ASTC_4x4: return static_cast<u64>((width + 3) / 4) * ((height + 3) / 4) * 16;
    }
    return 0;
}

Span<const byte> TextureView::pixels(u32 mip, u32 layer) const {
    MCE_ASSERT_MSG(mip < mips.size() && layer < header->layers, "TextureView::pixels out of range");
    const TextureMip& m = mips[mip];
    return payload.subspan(static_cast<usize>(m.offset + m.layer_size * layer), static_cast<usize>(m.layer_size));
}

Result<TextureView> TextureView::read(Span<const byte> payload) {
    constexpr const char* kStage = "asset.texture.read";
    const auto* h = header_of<TextureHeader>(payload);
    if (!h) return Error(Errc::Truncated, kStage);
    if (h->format != TextureFormat::RGBA8 && h->format != TextureFormat::ASTC_4x4)
        return Error(Errc::Unsupported, kStage, {}, static_cast<u32>(h->format));
    if (h->width == 0 || h->height == 0 || h->layers == 0 || h->mip_count == 0 || h->mip_count > 32) return bad(kStage);
    TextureView v;
    v.header = h;
    v.payload = payload;
    if (!span_at(payload, sizeof(TextureHeader), h->mip_count, v.mips)) return bad(kStage);
    for (u32 i = 0; i < h->mip_count; ++i) {
        const TextureMip& m = v.mips[i];
        const u64 expect = texture_level_bytes(h->format, m.width, m.height);
        if (m.layer_size != expect || m.width == 0 || m.height == 0) return bad(kStage, i);
        if (expect != 0 && h->layers > (~u64(0)) / expect) return bad(kStage, i);
        if (!in_bounds(m.offset, m.layer_size * h->layers, payload.size())) return bad(kStage, i);
    }
    return v;
}

void write_texture(TextureFormat format, u32 layers, u32 flags, Span<const TextureMipData> mips, Array<byte>& out) {
    MCE_ASSERT(!mips.empty() && layers > 0);
    PayloadBuilder b(out);
    TextureHeader h{};
    h.format = format;
    h.width = mips[0].width;
    h.height = mips[0].height;
    h.layers = layers;
    h.mip_count = static_cast<u32>(mips.size());
    h.flags = flags;
    b.append(&h, sizeof(h));
    Array<TextureMip> table;
    table.resize(mips.size());
    const usize table_at = b.append_span(Span<const TextureMip>(table.data(), table.size()));
    for (usize i = 0; i < mips.size(); ++i) {
        const u64 layer_size = texture_level_bytes(format, mips[i].width, mips[i].height);
        MCE_ASSERT_MSG(mips[i].pixels.size() == layer_size * layers, "write_texture: pixel size mismatch");
        table[i] = TextureMip{mips[i].width, mips[i].height, b.append(mips[i].pixels.data(), mips[i].pixels.size()), layer_size};
    }
    for (usize i = 0; i < mips.size(); ++i) b.patch(table_at + i * sizeof(TextureMip), table[i]);
    b.finish();
}

// ------------------------------------------------------------------ sfx

Result<SfxView> SfxView::read(Span<const byte> payload) {
    constexpr const char* kStage = "asset.sfx.read";
    const auto* h = header_of<SfxHeader>(payload);
    if (!h) return Error(Errc::Truncated, kStage);
    if (h->channels == 0 || h->channels > 8 || h->sample_rate == 0) return bad(kStage);
    if (h->frame_count > (~u64(0)) / h->channels) return bad(kStage);
    if ((h->flags & SfxFlags::Looping) && !(h->loop_start < h->loop_end && h->loop_end <= h->frame_count)) return bad(kStage);
    SfxView v;
    v.header = h;
    if (!span_at(payload, h->data_offset, h->frame_count * h->channels, v.samples)) return bad(kStage);
    return v;
}

void write_sfx(const SfxHeader& header, Span<const f32> samples, Array<byte>& out) {
    MCE_ASSERT(samples.size() == header.frame_count * header.channels);
    PayloadBuilder b(out);
    SfxHeader h = header;
    const usize at = b.append(&h, sizeof(h));
    h.data_offset = b.append_span(samples);
    b.patch(at, h);
    b.finish();
}

// ------------------------------------------------------------------ video

Result<VideoView> VideoView::read(Span<const byte> payload) {
    constexpr const char* kStage = "asset.video.read";
    const auto* h = header_of<VideoHeader>(payload);
    if (!h) return Error(Errc::Truncated, kStage);
    if (h->width == 0 || h->height == 0 || h->width > 4096 || h->height > 4096) return bad(kStage);
    if (!(h->frame_rate > 0.0f) || !(h->duration >= 0.0f) || h->mpeg_size == 0) return bad(kStage);
    if (h->audio_channels != 0 && h->audio_channels != 2) return bad(kStage);
    VideoView v;
    v.header = h;
    if (!span_at(payload, h->mpeg_offset, h->mpeg_size, v.mpeg)) return bad(kStage);
    if (!span_at(payload, h->audio_offset, u64(h->audio_frames) * h->audio_channels, v.audio)) return bad(kStage);
    return v;
}

void write_video(const VideoHeader& header, Span<const byte> mpeg, Span<const f32> audio, Array<byte>& out) {
    MCE_ASSERT(audio.size() == u64(header.audio_frames) * header.audio_channels);
    PayloadBuilder b(out);
    VideoHeader h = header;
    const usize at = b.append(&h, sizeof(h));
    h.mpeg_offset = b.append(mpeg.data(), mpeg.size());
    h.mpeg_size = mpeg.size();
    h.audio_offset = b.append_span(audio);
    b.patch(at, h);
    b.finish();
}

// ------------------------------------------------------------------ font

const FontGlyph* FontView::find(u32 codepoint) const {
    const FontGlyph* end = glyphs.data() + glyphs.size();
    const FontGlyph* it = std::lower_bound(glyphs.data(), end, codepoint, [](const FontGlyph& g, u32 cp) { return g.codepoint < cp; });
    return (it != end && it->codepoint == codepoint) ? it : nullptr;
}

f32 FontView::kern(u32 left, u32 right) const {
    const FontKern* end = kerning.data() + kerning.size();
    const FontKern* it = std::lower_bound(kerning.data(), end, FontKern{left, right, 0.0f}, [](const FontKern& a, const FontKern& b) {
        return a.left != b.left ? a.left < b.left : a.right < b.right;
    });
    return (it != end && it->left == left && it->right == right) ? it->advance : 0.0f;
}

Result<FontView> FontView::read(Span<const byte> payload) {
    constexpr const char* kStage = "asset.font.read";
    const auto* h = header_of<FontHeader>(payload);
    if (!h) return Error(Errc::Truncated, kStage);
    FontView v;
    v.header = h;
    if (!span_at(payload, h->glyph_offset, h->glyph_count, v.glyphs)) return bad(kStage);
    if (!span_at(payload, h->kern_offset, h->kern_count, v.kerning)) return bad(kStage);
    const u64 atlas_size = static_cast<u64>(h->atlas_width) * h->atlas_height;
    if (!in_bounds(h->atlas_offset, atlas_size, payload.size())) return bad(kStage);
    v.atlas = payload.subspan(static_cast<usize>(h->atlas_offset), static_cast<usize>(atlas_size));
    for (const FontGlyph& g : v.glyphs)
        if (g.x1 < g.x0 || g.y1 < g.y0 || g.x1 > h->atlas_width || g.y1 > h->atlas_height) return bad(kStage, g.codepoint);
    return v;
}

void write_font(const FontHeader& header, Span<const FontGlyph> glyphs, Span<const FontKern> kerning, Span<const byte> atlas,
                Array<byte>& out) {
    MCE_ASSERT(atlas.size() == static_cast<usize>(header.atlas_width) * header.atlas_height);
    PayloadBuilder b(out);
    FontHeader h = header;
    h.glyph_count = static_cast<u32>(glyphs.size());
    h.kern_count = static_cast<u32>(kerning.size());
    const usize at = b.append(&h, sizeof(h));
    h.glyph_offset = b.append_span(glyphs);
    h.kern_offset = b.append_span(kerning);
    h.atlas_offset = b.append_span(atlas);
    b.patch(at, h);
    b.finish();
}

// ------------------------------------------------------------------ material

namespace {
f32 half_to_f32(u16 h) {
    const u32 sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu;
    f32 v;
    if (exp == 0) v = std::ldexp(static_cast<f32>(man), -24);
    else if (exp == 31) v = man ? 0.0f : 65504.0f;  // no NaN or infinity in a texture scale
    else v = std::ldexp(static_cast<f32>(man | 0x400u), static_cast<int>(exp) - 25);
    return sign ? -v : v;
}

u16 f32_to_half(f32 f) {
    const f32 a = std::fabs(f);
    const u16 sign = f < 0.0f ? 0x8000u : 0u;
    if (!(a >= 6.1035156e-05f)) return sign;  // zero, subnormal range or NaN: 0 (read back as "no scale")
    if (a >= 65504.0f) return static_cast<u16>(sign | 0x7BFFu);
    int e = 0;
    const f32 m = std::frexp(a, &e);  // a = m * 2^e, m in [0.5, 1)
    u32 man = static_cast<u32>(std::lround((m * 2.0f - 1.0f) * 1024.0f));
    if (man == 1024u) {
        man = 0;
        ++e;
    }
    return static_cast<u16>(sign | static_cast<u32>(e + 14) << 10 | man);
}
}  // namespace

void MaterialChunk::uv_scale(f32 out[2]) const {
    out[0] = out[1] = 1.0f;
    if (uv_scale_half == 0) return;
    out[0] = half_to_f32(static_cast<u16>(uv_scale_half & 0xFFFFu));
    out[1] = half_to_f32(static_cast<u16>(uv_scale_half >> 16));
}

u32 MaterialChunk::pack_uv_scale(f32 u, f32 v) {
    if (u == 1.0f && v == 1.0f) return 0;
    return static_cast<u32>(f32_to_half(u)) | static_cast<u32>(f32_to_half(v)) << 16;
}

MaterialExt MaterialExt::of_material(Span<const byte> payload) {
    MaterialExt e{};
    if (payload.size() >= sizeof(MaterialChunk) + sizeof(MaterialExt))
        std::memcpy(&e, payload.data() + sizeof(MaterialChunk), sizeof(MaterialExt));
    return e;
}

Result<const MaterialChunk*> MaterialChunk::read(Span<const byte> payload) {
    const auto* m = header_of<MaterialChunk>(payload);
    if (!m) return Error(Errc::Truncated, "asset.material.read");
    return m;
}

// ------------------------------------------------------------------ tilemap

Result<TilemapView> TilemapView::read(Span<const byte> payload) {
    TilemapView v;
    v.header = header_of<TilemapHeader>(payload);
    if (!v.header) return Error(Errc::Truncated, "asset.tilemap.read");
    const u64 count = u64(v.header->width) * v.header->height;
    if (v.header->width == 0 || v.header->height == 0 || v.header->columns == 0 || v.header->rows == 0 ||
        count > (payload.size() - sizeof(TilemapHeader)) / sizeof(u32))
        return Error(Errc::Truncated, "asset.tilemap.read");
    v.tiles = Span<const u32>(reinterpret_cast<const u32*>(payload.data() + sizeof(TilemapHeader)), static_cast<usize>(count));
    return v;
}

// ------------------------------------------------------------------ sprite sheet

namespace {
StringView padded(const char* s, usize cap) {
    usize n = 0;
    while (n < cap && s[n] != '\0') ++n;
    return StringView(s, n);
}
}  // namespace

Result<SpriteSheetView> SpriteSheetView::read(Span<const byte> payload) {
    SpriteSheetView v;
    v.header = header_of<SpriteSheetHeader>(payload);
    if (!v.header) return Error(Errc::Truncated, "asset.spritesheet.read");
    const SpriteSheetHeader& h = *v.header;
    const u64 bytes = sizeof(SpriteSheetHeader) + u64(h.region_count) * sizeof(SpriteSheetRegion) +
                      u64(h.clip_count) * sizeof(SpriteSheetClip) + u64(h.frame_count) * sizeof(u32) +
                      u64(h.event_count) * sizeof(SpriteSheetEvent);
    if (h.region_count > (1u << 20) || h.clip_count > (1u << 16) || h.frame_count > (1u << 20) || h.event_count > (1u << 20) ||
        bytes > payload.size())
        return Error(Errc::Truncated, "asset.spritesheet.read");
    const byte* p = payload.data() + sizeof(SpriteSheetHeader);
    v.regions = Span<const SpriteSheetRegion>(reinterpret_cast<const SpriteSheetRegion*>(p), h.region_count);
    p += usize(h.region_count) * sizeof(SpriteSheetRegion);
    v.clips = Span<const SpriteSheetClip>(reinterpret_cast<const SpriteSheetClip*>(p), h.clip_count);
    p += usize(h.clip_count) * sizeof(SpriteSheetClip);
    v.frames = Span<const u32>(reinterpret_cast<const u32*>(p), h.frame_count);
    p += usize(h.frame_count) * sizeof(u32);
    v.events = Span<const SpriteSheetEvent>(reinterpret_cast<const SpriteSheetEvent*>(p), h.event_count);
    for (const u32 f : v.frames)
        if (f >= h.region_count) return Error(Errc::InvalidArgument, "asset.spritesheet.frame");
    for (const SpriteSheetClip& c : v.clips) {
        if (u64(c.first_frame) + c.frame_count > h.frame_count || u64(c.first_event) + c.event_count > h.event_count || c.mode > 2)
            return Error(Errc::InvalidArgument, "asset.spritesheet.clip");
        for (u32 i = 0; i < c.event_count; ++i)
            if (v.events[c.first_event + i].frame >= c.frame_count) return Error(Errc::InvalidArgument, "asset.spritesheet.event");
    }
    return v;
}

const SpriteSheetClip* SpriteSheetView::find_clip(StringView name) const {
    if (clips.empty()) return nullptr;
    if (name.empty()) return &clips[0];
    for (const SpriteSheetClip& c : clips)
        if (name_of(c) == name) return &c;
    return nullptr;
}

StringView SpriteSheetView::name_of(const SpriteSheetClip& c) { return padded(c.name, sizeof(c.name)); }
StringView SpriteSheetView::name_of(const SpriteSheetEvent& e) { return padded(e.name, sizeof(e.name)); }

// ------------------------------------------------------------------ animation graph

Result<AnimGraphView> AnimGraphView::read(Span<const byte> payload) {
    AnimGraphView v;
    v.header = header_of<AnimGraphHeader>(payload);
    if (!v.header) return Error(Errc::Truncated, "asset.animgraph.read");
    const AnimGraphHeader& h = *v.header;
    constexpr u32 kMax = 1u << 16;
    if (h.param_count > kAnimGraphMaxParams || h.state_count == 0 || h.state_count > kMax || h.transition_count > kMax ||
        h.condition_count > kMax || h.blend_point_count > kMax || h.event_count > kMax || h.start_state >= h.state_count)
        return Error(Errc::InvalidArgument, "asset.animgraph.read");
    const u64 bytes = sizeof(AnimGraphHeader) + u64(h.state_count) * sizeof(AnimGraphState) +
                      u64(h.transition_count) * sizeof(AnimGraphTransition) + u64(h.condition_count) * sizeof(AnimGraphCondition) +
                      u64(h.blend_point_count) * sizeof(AnimGraphBlendPoint) + u64(h.event_count) * sizeof(AnimGraphEvent);
    if (bytes > payload.size()) return Error(Errc::Truncated, "asset.animgraph.read");
    const byte* p = payload.data() + sizeof(AnimGraphHeader);
    v.states = Span<const AnimGraphState>(reinterpret_cast<const AnimGraphState*>(p), h.state_count);
    p += usize(h.state_count) * sizeof(AnimGraphState);
    v.transitions = Span<const AnimGraphTransition>(reinterpret_cast<const AnimGraphTransition*>(p), h.transition_count);
    p += usize(h.transition_count) * sizeof(AnimGraphTransition);
    v.conditions = Span<const AnimGraphCondition>(reinterpret_cast<const AnimGraphCondition*>(p), h.condition_count);
    p += usize(h.condition_count) * sizeof(AnimGraphCondition);
    v.blend_points = Span<const AnimGraphBlendPoint>(reinterpret_cast<const AnimGraphBlendPoint*>(p), h.blend_point_count);
    p += usize(h.blend_point_count) * sizeof(AnimGraphBlendPoint);
    v.events = Span<const AnimGraphEvent>(reinterpret_cast<const AnimGraphEvent*>(p), h.event_count);
    if (h.flags & ~kAnimGraphExtended) return Error(Errc::InvalidArgument, "asset.animgraph.read");
    if (h.flags & kAnimGraphExtended) {
        p += usize(h.event_count) * sizeof(AnimGraphEvent);
        if (bytes + sizeof(AnimGraphExtension) > payload.size()) return Error(Errc::Truncated, "asset.animgraph.read");
        v.extension = reinterpret_cast<const AnimGraphExtension*>(p);
        const AnimGraphExtension& x = *v.extension;
        if (x.blend2d_count > kMax || x.layer_count > kAnimGraphMaxLayers || x.mask_count > kMax || x.ik_count > kAnimGraphMaxIK)
            return Error(Errc::InvalidArgument, "asset.animgraph.read");
        const u64 more = sizeof(AnimGraphExtension) + u64(x.blend2d_count) * sizeof(AnimGraphBlendPoint2D) +
                         u64(x.layer_count) * sizeof(AnimGraphLayer) + u64(x.mask_count) * sizeof(AnimGraphMaskBone) +
                         u64(x.ik_count) * sizeof(AnimGraphIK);
        if (bytes + more > payload.size()) return Error(Errc::Truncated, "asset.animgraph.read");
        p += sizeof(AnimGraphExtension);
        v.blend2d = Span<const AnimGraphBlendPoint2D>(reinterpret_cast<const AnimGraphBlendPoint2D*>(p), x.blend2d_count);
        p += usize(x.blend2d_count) * sizeof(AnimGraphBlendPoint2D);
        v.layers = Span<const AnimGraphLayer>(reinterpret_cast<const AnimGraphLayer*>(p), x.layer_count);
        p += usize(x.layer_count) * sizeof(AnimGraphLayer);
        v.mask_bones = Span<const AnimGraphMaskBone>(reinterpret_cast<const AnimGraphMaskBone*>(p), x.mask_count);
        p += usize(x.mask_count) * sizeof(AnimGraphMaskBone);
        v.ik = Span<const AnimGraphIK>(reinterpret_cast<const AnimGraphIK*>(p), x.ik_count);
        for (const AnimGraphLayer& l : v.layers)
            if (u64(l.first_mask) + l.mask_count > x.mask_count || (l.weight_param != kAnimNoParam && l.weight_param >= h.param_count))
                return Error(Errc::InvalidArgument, "asset.animgraph.layer");
        for (const AnimGraphIK& k : v.ik)
            if (static_cast<u8>(k.kind) > static_cast<u8>(AnimIKKind::Foot) ||
                (k.weight_param != kAnimNoParam && k.weight_param >= h.param_count))
                return Error(Errc::InvalidArgument, "asset.animgraph.ik");
    }
    for (const AnimGraphState& s : v.states) {
        const u64 points = s.blend_2d ? v.blend2d.size() : h.blend_point_count;
        if (u64(s.first_blend) + s.blend_count > points || u64(s.first_event) + s.event_count > h.event_count ||
            (s.blend_count > 0 && s.blend_param >= h.param_count) || s.blend_2d > 1 ||
            (s.blend_2d && (s.blend_count == 0 || s.blend_count > kAnimGraphMaxBlend2D || s.blend_param_y >= h.param_count)))
            return Error(Errc::InvalidArgument, "asset.animgraph.state");
    }
    for (const AnimGraphTransition& t : v.transitions)
        if ((t.from != kAnimAnyState && t.from >= h.state_count) || t.to >= h.state_count ||
            u64(t.first_condition) + t.condition_count > h.condition_count)
            return Error(Errc::InvalidArgument, "asset.animgraph.transition");
    for (const AnimGraphCondition& c : v.conditions)
        if (c.param >= h.param_count || static_cast<u8>(c.op) > static_cast<u8>(AnimCompare::NotEqual))
            return Error(Errc::InvalidArgument, "asset.animgraph.condition");
    return v;
}

u32 AnimGraphView::find_state(StringView name) const {
    for (u32 i = 0; i < states.size(); ++i)
        if (text(states[i].name, sizeof(states[i].name)) == name) return i;
    return static_cast<u32>(states.size());
}

StringView AnimGraphView::text(const char* s, usize cap) { return padded(s, cap); }

// ------------------------------------------------------------------ particle preset

Result<const ParticlePresetChunk*> ParticlePresetChunk::read(Span<const byte> payload) {
    const ParticlePresetChunk* p = header_of<ParticlePresetChunk>(payload);
    if (!p) return Error(Errc::Truncated, "asset.particle.read");
    return p;
}

// ------------------------------------------------------------------ sequence

StringView SequenceView::name() const {
    usize n = 0;
    while (n < sizeof(header->name) && header->name[n] != '\0') ++n;
    return StringView(header->name, n);
}

Result<SequenceView> SequenceView::read(Span<const byte> payload) {
    SequenceView v;
    v.header = header_of<SequenceHeader>(payload);
    if (!v.header) return Error(Errc::Truncated, "asset.sequence.read");
    if (v.header->step_count == 0 || v.header->step_count > (payload.size() - sizeof(SequenceHeader)) / sizeof(SequenceStep))
        return Error(Errc::Truncated, "asset.sequence.read");
    v.steps = Span<const SequenceStep>(reinterpret_cast<const SequenceStep*>(payload.data() + sizeof(SequenceHeader)),
                                       v.header->step_count);
    if (v.steps[v.steps.size() - 1].kind != SequenceStepKind::End) return Error(Errc::InvalidArgument, "asset.sequence.no_end");
    return v;
}

// ------------------------------------------------------------------ timeline

StringView TimelineView::name() const {
    usize n = 0;
    while (n < sizeof(header->name) && header->name[n] != '\0') ++n;
    return StringView(header->name, n);
}

StringView TimelineView::string(u32 offset) const {
    if (offset >= strings.size()) return {};
    usize n = 0;
    while (offset + n < strings.size() && strings[offset + n] != '\0') ++n;
    return StringView(strings.data() + offset, n);
}

Result<TimelineView> TimelineView::read(Span<const byte> payload) {
    TimelineView v;
    v.header = header_of<TimelineHeader>(payload);
    if (!v.header) return Error(Errc::Truncated, "asset.timeline.read");
    const TimelineHeader& h = *v.header;
    constexpr u32 kMax = 1u << 20;
    if (!(h.duration > 0.0f) || h.track_count > kMax || h.key_count > kMax || h.string_bytes > kMax * 16)
        return Error(Errc::InvalidArgument, "asset.timeline.read");
    const u64 bytes = sizeof(TimelineHeader) + u64(h.track_count) * sizeof(TimelineTrack) + u64(h.key_count) * sizeof(TimelineKey) +
                      h.string_bytes;
    if (bytes > payload.size()) return Error(Errc::Truncated, "asset.timeline.read");
    const byte* p = payload.data() + sizeof(TimelineHeader);
    v.tracks = Span<const TimelineTrack>(reinterpret_cast<const TimelineTrack*>(p), h.track_count);
    p += usize(h.track_count) * sizeof(TimelineTrack);
    v.keys = Span<const TimelineKey>(reinterpret_cast<const TimelineKey*>(p), h.key_count);
    p += usize(h.key_count) * sizeof(TimelineKey);
    v.strings = Span<const char>(reinterpret_cast<const char*>(p), h.string_bytes);
    if (h.string_bytes > 0 && v.strings[h.string_bytes - 1] != '\0') return Error(Errc::InvalidArgument, "asset.timeline.strings");
    for (const TimelineTrack& t : v.tracks) {
        if (t.kind > TimelineTrackKind::Video || u64(t.first_key) + t.key_count > h.key_count ||
            (t.target != kTimelineSelf && t.target >= kTimelineBindings))
            return Error(Errc::InvalidArgument, "asset.timeline.track");
        const bool pathed = t.kind == TimelineTrackKind::Property || t.kind == TimelineTrackKind::Activation;
        if (pathed && t.path >= h.string_bytes) return Error(Errc::InvalidArgument, "asset.timeline.track");
        if (t.kind == TimelineTrackKind::Property && (t.value_count == 0 || t.value_count > 4))
            return Error(Errc::InvalidArgument, "asset.timeline.track");
        f32 prev = 0.0f;
        for (const TimelineKey& k : v.keys_of(t)) {
            if (!(k.t >= prev) || !(k.duration >= 0.0f) || k.ease > TimelineEase::EaseOut ||
                (k.target != kTimelineSelf && k.target >= kTimelineBindings) ||
                (k.text != kTimelineNoString && k.text >= h.string_bytes) || (k.text2 != kTimelineNoString && k.text2 >= h.string_bytes))
                return Error(Errc::InvalidArgument, "asset.timeline.key");
            prev = k.t;
        }
    }
    return v;
}

// ------------------------------------------------------------------ shader

StringView ShaderView::define(u32 i) const {
    const char* d = header->defines[i];
    usize n = 0;
    while (n < sizeof(header->defines[i]) && d[n] != '\0') ++n;
    return StringView(d, n);
}

const ShaderVariant* ShaderView::find(u32 mask) const {
    for (const ShaderVariant& v : variants)
        if (v.mask == mask) return &v;
    return nullptr;
}

Span<const u32> ShaderView::spirv(const ShaderVariant& v) const {
    const byte* base = reinterpret_cast<const byte*>(header);
    return Span<const u32>(reinterpret_cast<const u32*>(base + v.spirv_offset), v.spirv_words);
}

Span<const ShaderResource> ShaderView::resources_of(const ShaderVariant& v) const {
    return resources.subspan(v.first_resource, v.resource_count);
}

void ShaderView::compose(u32 mask, Array<char>& out) const {
    out.clear();
    usize at = 0;  // after the #version line (or the start when there is none)
    const usize v = source.find("#version");
    if (v != StringView::npos) {
        const usize eol = source.find('\n', v);
        at = eol == StringView::npos ? source.size() : eol + 1;
    }
    auto put = [&](StringView s) {
        for (char c : s) out.push_back(c);
    };
    put(source.substr(0, at));
    if (at > 0 && source[at - 1] != '\n') out.push_back('\n');
    for (u32 i = 0; i < header->define_count; ++i) {
        put("#define ");
        put(define(i));
        put((mask >> i) & 1u ? " 1\n" : " 0\n");
    }
    put(source.substr(at));
}

Result<ShaderView> ShaderView::read(Span<const byte> payload) {
    constexpr const char* kStage = "asset.shader.read";
    ShaderView s;
    s.header = header_of<ShaderHeader>(payload);
    if (!s.header) return Error(Errc::Truncated, kStage);
    const ShaderHeader& h = *s.header;
    if (h.define_count > kMaxShaderDefines || h.variant_count == 0 || h.variant_count > (1u << h.define_count))
        return Error(Errc::InvalidArgument, kStage);
    const usize variants_at = sizeof(ShaderHeader);
    const usize resources_at = variants_at + usize(h.variant_count) * sizeof(ShaderVariant);
    const usize source_at = resources_at + usize(h.resource_count) * sizeof(ShaderResource);
    if (source_at + h.source_size > payload.size()) return Error(Errc::Truncated, kStage);
    s.variants = Span<const ShaderVariant>(reinterpret_cast<const ShaderVariant*>(payload.data() + variants_at), h.variant_count);
    s.resources =
        Span<const ShaderResource>(reinterpret_cast<const ShaderResource*>(payload.data() + resources_at), h.resource_count);
    s.source = StringView(reinterpret_cast<const char*>(payload.data() + source_at), h.source_size);
    for (const ShaderVariant& v : s.variants) {
        if (v.spirv_offset % 4 != 0 || v.spirv_offset < source_at + h.source_size ||
            v.spirv_offset + usize(v.spirv_words) * 4 > payload.size() || v.first_resource + v.resource_count > h.resource_count ||
            v.mask >= (1u << h.define_count))
            return Error(Errc::Truncated, kStage);
    }
    return s;
}

// ------------------------------------------------------------------ mesh

u32 MeshView::index(u32 i) const {
    if (index32()) return reinterpret_cast<const u32*>(indices.data())[i];
    return reinterpret_cast<const u16*>(indices.data())[i];
}

StringView MeshView::node_name(u32 node) const {
    const MeshNode& n = nodes[node];
    return StringView(strings.data() + n.name_offset, n.name_size);
}

StringView MeshView::clip_name(u32 clip) const {
    const MeshClip& c = clips[clip];
    return StringView(strings.data() + c.name_offset, c.name_size);
}

u32 MeshView::find_clip(StringView name) const {
    for (u32 i = 0; i < clips.size(); ++i)
        if (clip_name(i) == name) return i;
    return static_cast<u32>(clips.size());
}

Result<MeshView> MeshView::read(Span<const byte> payload) {
    constexpr const char* kStage = "asset.mesh.read";
    const auto* h = header_of<MeshHeader>(payload);
    if (!h) return Error(Errc::Truncated, kStage);
    if (h->flags & ~u32(MeshFlags::Index32 | MeshFlags::Skinned | MeshFlags::Lods | MeshFlags::Ragdoll | MeshFlags::Materials))
        return Error(Errc::ReservedNonZero, kStage);
    if ((h->flags & MeshFlags::Ragdoll) && !(h->flags & MeshFlags::Skinned)) return Error(Errc::ReservedNonZero, kStage);
    if (!(h->flags & MeshFlags::Skinned) && h->skin_offset != 0) return Error(Errc::ReservedNonZero, kStage);
    MeshView v;
    v.header = h;
    const u64 isize = (h->flags & MeshFlags::Index32) ? 4 : 2;
    if (!span_at(payload, h->vertices_offset, h->vertex_count, v.vertices)) return bad(kStage, 1);
    if (!in_bounds(h->indices_offset, u64(h->index_count) * isize, payload.size()) || h->indices_offset % isize != 0)
        return bad(kStage, 2);
    v.indices = payload.subspan(static_cast<usize>(h->indices_offset), static_cast<usize>(u64(h->index_count) * isize));
    if (!span_at(payload, h->submeshes_offset, h->submesh_count, v.submeshes)) return bad(kStage, 3);
    if (!span_at(payload, h->nodes_offset, h->node_count, v.nodes)) return bad(kStage, 4);
    if (!span_at(payload, h->clips_offset, h->clip_count, v.clips)) return bad(kStage, 5);
    if (!span_at(payload, h->channels_offset, h->channel_count, v.channels)) return bad(kStage, 6);
    if (!span_at(payload, h->keys_offset, h->key_count, v.keys)) return bad(kStage, 7);
    if (!span_at(payload, h->strings_offset, h->strings_size, v.strings)) return bad(kStage, 8);

    for (u32 i = 0; i < h->node_count; ++i) {
        const MeshNode& n = v.nodes[i];
        if (n.parent < -1 || n.parent >= static_cast<i32>(i)) return bad(kStage, 0x100 + i);
        if (!in_bounds(n.name_offset, n.name_size, h->strings_size)) return bad(kStage, 0x100 + i);
    }
    if (h->flags & MeshFlags::Lods) {
        u32 table = 0;
        for (const MeshSubmesh& sm : v.submeshes) {
            if (submesh_lod_count(sm) >= kMaxMeshLods || submesh_lod_first(sm) > (1u << 24)) return bad(kStage, 14);
            table = std::max(table, submesh_lod_first(sm) + submesh_lod_count(sm));
        }
        const u64 at = align_up(static_cast<usize>(h->strings_offset + h->strings_size), CHUNK_ALIGN);
        if (!span_at(payload, at, table, v.lods)) return bad(kStage, 14);
        for (u32 s = 0; s < h->submesh_count; ++s) {
            const MeshSubmesh& sm = v.submeshes[s];
            if (sm.reserved[2] != 0) return Error(Errc::ReservedNonZero, kStage);
            for (const MeshLod& l : v.submesh_lods(s)) {
                if (!in_bounds(l.first_index, l.index_count, h->index_count) || l.index_count % 3 != 0 ||
                    !in_bounds(l.base_vertex, l.vertex_count, h->vertex_count))
                    return bad(kStage, 0x70000 + s);
                for (u32 i = 0; i < l.index_count; ++i)
                    if (v.index(l.first_index + i) >= l.vertex_count) return bad(kStage, 0x70000 + s);
            }
        }
    } else {
        for (const MeshSubmesh& sm : v.submeshes)
            if (sm.reserved[0] != 0 || sm.reserved[1] != 0) return Error(Errc::ReservedNonZero, kStage);
    }
    if ((h->flags & MeshFlags::Materials) && h->submesh_count > 0) {  // consecutive, one per submesh
        const u64 at = v.submeshes[0].reserved2[0];
        if (at % alignof(MaterialExt) != 0 || !span_at(payload, at, h->submesh_count, v.materials)) return bad(kStage, 15);
        for (u32 s = 0; s < h->submesh_count; ++s)
            if (v.submeshes[s].reserved2[0] != at + u64(s) * sizeof(MaterialExt)) return bad(kStage, 15);
    }
    for (u32 s = 0; s < h->submesh_count; ++s) {
        const MeshSubmesh& sm = v.submeshes[s];
        if (sm.node >= h->node_count) return bad(kStage, 0x10000 + s);
        if (!in_bounds(sm.first_index, sm.index_count, h->index_count)) return bad(kStage, 0x10000 + s);
        if (!in_bounds(sm.base_vertex, sm.vertex_count, h->vertex_count)) return bad(kStage, 0x10000 + s);
        for (u32 i = 0; i < sm.index_count; ++i)
            if (v.index(sm.first_index + i) >= sm.vertex_count) return bad(kStage, 0x10000 + s);
    }
    for (u32 c = 0; c < h->clip_count; ++c) {
        const MeshClip& clip = v.clips[c];
        if (!in_bounds(clip.first_channel, clip.channel_count, h->channel_count)) return bad(kStage, 0x20000 + c);
        if (!in_bounds(clip.name_offset, clip.name_size, h->strings_size)) return bad(kStage, 0x20000 + c);
    }
    for (u32 c = 0; c < h->channel_count; ++c) {
        const MeshChannel& ch = v.channels[c];
        const u32 width = ch.path == AnimPath::Rotation ? 4 : 3;
        if (ch.node >= h->node_count || static_cast<u32>(ch.path) > 2 || static_cast<u32>(ch.interp) > 2 || ch.key_count == 0)
            return bad(kStage, 0x30000 + c);
        const u64 per_key = ch.interp == AnimInterp::CubicSpline ? u64(width) * 3 : u64(width);
        if (!in_bounds(ch.times_offset, ch.key_count, h->key_count)) return bad(kStage, 0x30000 + c);
        if (!in_bounds(ch.values_offset, u64(ch.key_count) * per_key, h->key_count)) return bad(kStage, 0x30000 + c);
    }
    if (h->flags & MeshFlags::Skinned) {
        const auto* sh = header_of<MeshSkinHeader>(payload.subspan(std::min<usize>(h->skin_offset, payload.size())));
        if (!sh || h->skin_offset % CHUNK_ALIGN != 0) return bad(kStage, 9);
        if (sh->vertex_count != h->vertex_count || sh->submesh_count != h->submesh_count || sh->skin_count == 0)
            return bad(kStage, 9);
        if (!span_at(payload, sh->skins_offset, sh->skin_count, v.skins)) return bad(kStage, 10);
        if (!span_at(payload, sh->joints_offset, sh->joint_count, v.joints)) return bad(kStage, 11);
        if (!span_at(payload, sh->vertices_offset, sh->vertex_count, v.skin_vertices)) return bad(kStage, 12);
        if (!span_at(payload, sh->submesh_skins_offset, sh->submesh_count, v.submesh_skins)) return bad(kStage, 13);
        for (u32 k = 0; k < sh->skin_count; ++k) {
            const MeshSkin& skin = v.skins[k];
            if (skin.joint_count == 0 || skin.joint_count > kMaxSkinJoints || !in_bounds(skin.first_joint, skin.joint_count, sh->joint_count))
                return bad(kStage, 0x40000 + k);
        }
        const bool ragdoll = (h->flags & MeshFlags::Ragdoll) != 0;
        for (u32 j = 0; j < sh->joint_count; ++j) {
            const MeshJoint& mj = v.joints[j];
            if (mj.node >= h->node_count) return bad(kStage, 0x50000 + j);
            const bool overrides = mj.ragdoll_flags != 0 || mj.ragdoll_radius != 0.0f || mj.ragdoll_swing != 0.0f ||
                                   mj.ragdoll_twist != 0.0f || mj.ragdoll_mass != 0.0f;
            if (!ragdoll && overrides) return Error(Errc::ReservedNonZero, kStage);
            if (ragdoll && (mj.ragdoll_flags & ~0x3Fu)) return Error(Errc::ReservedNonZero, kStage);
        }
        for (u32 s = 0; s < h->submesh_count; ++s) {
            const i32 k = v.submesh_skins[s];
            if (k < 0) continue;
            if (static_cast<u32>(k) >= sh->skin_count) return bad(kStage, 0x60000 + s);
            const MeshSubmesh& sm = v.submeshes[s];
            const u32 joints = v.skins[static_cast<u32>(k)].joint_count;
            for (u32 i = 0; i < sm.vertex_count; ++i) {
                const MeshSkinVertex& sv = v.skin_vertices[sm.base_vertex + i];
                for (u32 c = 0; c < 4; ++c)  // all four: the shader reads every slot, weighted or not
                    if (sv.joints[c] >= joints) return bad(kStage, 0x60000 + s);
            }
        }
    }
    return v;
}

void write_mesh(const MeshData& d, Array<byte>& out) {
    PayloadBuilder b(out);
    MeshHeader h{};
    h.vertex_count = static_cast<u32>(d.vertices.size());
    const usize isize = (d.flags & MeshFlags::Index32) ? 4 : 2;
    h.index_count = static_cast<u32>(d.indices.size() / isize);
    h.submesh_count = static_cast<u32>(d.submeshes.size());
    h.node_count = static_cast<u32>(d.nodes.size());
    h.clip_count = static_cast<u32>(d.clips.size());
    h.channel_count = static_cast<u32>(d.channels.size());
    h.key_count = static_cast<u32>(d.keys.size());
    h.flags = d.flags;
    for (int i = 0; i < 3; ++i) {
        h.bounds_min[i] = d.bounds_min[i];
        h.bounds_max[i] = d.bounds_max[i];
    }
    h.strings_size = static_cast<u32>(d.strings.size());
    const usize at = b.append(&h, sizeof(h));
    h.vertices_offset = b.append_span(d.vertices);
    h.indices_offset = b.append_span(d.indices);
    h.submeshes_offset = b.append_span(d.submeshes);
    h.nodes_offset = b.append_span(d.nodes);
    h.clips_offset = b.append_span(d.clips);
    h.channels_offset = b.append_span(d.channels);
    h.keys_offset = b.append_span(d.keys);
    h.strings_offset = b.append_span(d.strings);
    if (!d.lods.empty()) {  // right after the strings: the reader finds it there
        h.flags |= MeshFlags::Lods;
        const usize lods_at = b.append_span(d.lods);
        MCE_ASSERT_MSG(lods_at == align_up(static_cast<usize>(h.strings_offset + h.strings_size), CHUNK_ALIGN),
                       "write_mesh: the LOD table must follow the strings");
    }
    if (!d.skins.empty()) {
        MCE_ASSERT_MSG(d.skin_vertices.size() == d.vertices.size() && d.submesh_skins.size() == d.submeshes.size(),
                       "write_mesh: the skin section needs one entry per vertex and per submesh");
        h.flags |= MeshFlags::Skinned;
        MeshSkinHeader sh{};
        sh.skin_count = static_cast<u32>(d.skins.size());
        sh.joint_count = static_cast<u32>(d.joints.size());
        sh.vertex_count = h.vertex_count;
        sh.submesh_count = h.submesh_count;
        const usize skin_at = b.append(&sh, sizeof(sh));
        h.skin_offset = static_cast<u32>(skin_at);
        sh.skins_offset = b.append_span(d.skins);
        sh.joints_offset = b.append_span(d.joints);
        sh.vertices_offset = b.append_span(d.skin_vertices);
        sh.submesh_skins_offset = b.append_span(d.submesh_skins);
        b.patch(skin_at, sh);
    }
    if (!d.materials.empty()) {
        MCE_ASSERT_MSG(d.materials.size() == d.submeshes.size(), "write_mesh: one material extension per submesh");
        h.flags |= MeshFlags::Materials;
        const usize ext_at = b.append_span(d.materials);
        for (usize s = 0; s < d.submeshes.size(); ++s) {
            MeshSubmesh sm = d.submeshes[s];
            sm.reserved2[0] = static_cast<u32>(ext_at + s * sizeof(MaterialExt));
            b.patch(static_cast<usize>(h.submeshes_offset) + s * sizeof(MeshSubmesh), sm);
        }
    }
    b.patch(at, h);
    b.finish();
}

// ------------------------------------------------------------------ terrain

f32 TerrainView::height_at(f32 x, f32 z) const {
    const u32 n = header->resolution;
    const f32 fx = (x / header->size[0] + 0.5f) * static_cast<f32>(n - 1);
    const f32 fz = (z / header->size[1] + 0.5f) * static_cast<f32>(n - 1);
    const f32 cx = std::clamp(fx, 0.0f, static_cast<f32>(n - 1)), cz = std::clamp(fz, 0.0f, static_cast<f32>(n - 1));
    const u32 x0 = std::min(static_cast<u32>(cx), n - 2), z0 = std::min(static_cast<u32>(cz), n - 2);
    const f32 tx = cx - static_cast<f32>(x0), tz = cz - static_cast<f32>(z0);
    const f32 a = sample(x0, z0) + (sample(x0 + 1, z0) - sample(x0, z0)) * tx;
    const f32 b = sample(x0, z0 + 1) + (sample(x0 + 1, z0 + 1) - sample(x0, z0 + 1)) * tx;
    return a + (b - a) * tz;
}

f32 TerrainView::surface_at(f32 x, f32 z) const {
    const u32 n = header->resolution;
    const f32 fx = std::clamp((x / header->size[0] + 0.5f) * static_cast<f32>(n - 1), 0.0f, static_cast<f32>(n - 1));
    const f32 fz = std::clamp((z / header->size[1] + 0.5f) * static_cast<f32>(n - 1), 0.0f, static_cast<f32>(n - 1));
    const u32 x0 = std::min(static_cast<u32>(fx), n - 2), z0 = std::min(static_cast<u32>(fz), n - 2);
    const f32 u = fx - static_cast<f32>(x0), w = fz - static_cast<f32>(z0);
    const f32 h00 = sample(x0, z0), h10 = sample(x0 + 1, z0), h01 = sample(x0, z0 + 1), h11 = sample(x0 + 1, z0 + 1);
    return u >= w ? h00 + u * (h10 - h00) + w * (h11 - h10) : h00 + w * (h01 - h00) + u * (h11 - h01);
}

Result<TerrainView> TerrainView::read(Span<const byte> payload) {
    TerrainView v;
    v.header = header_of<TerrainHeader>(payload);
    if (!v.header) return bad("asset.terrain.header");
    const TerrainHeader& h = *v.header;
    const u32 n = h.resolution;
    if (n < kTerrainMinResolution || n > kTerrainMaxResolution || ((n - 1) & (n - 2)) != 0) return bad("asset.terrain.resolution", n);
    if (h.chunk < 8 || h.chunk > 256 || (h.chunk & (h.chunk - 1)) != 0 || h.chunk > n - 1) return bad("asset.terrain.chunk", h.chunk);
    const bool sizes_ok = h.size[0] > 0.0f && h.size[1] > 0.0f && h.size[0] < 1e7f && h.size[1] < 1e7f;
    const bool heights_ok = h.height_min > -1e6f && h.height_max < 1e6f && h.height_max > h.height_min;
    if (!sizes_ok || !heights_ok) return bad("asset.terrain.extent");
    if (!span_at(payload, h.heights_offset, u64(n) * n, v.heights)) return bad("asset.terrain.heights");
    if (h.layer_count > kTerrainMaxLayers) return bad("asset.terrain.layers", h.layer_count);
    if (h.layer_count > 0) {
        const u32 s = h.splat_resolution;
        if (s < kTerrainMinResolution || s > n || ((s - 1) & (s - 2)) != 0) return bad("asset.terrain.splat_resolution", s);
        if (!span_at(payload, h.layers_offset, h.layer_count, v.layers)) return bad("asset.terrain.layers");
        if (!span_at(payload, h.splat_offset, u64(s) * s * 8, v.splat)) return bad("asset.terrain.splat");
        for (const TerrainLayer& l : v.layers)
            if (!(l.tile > 0.0f && l.tile < 1e6f) || !(l.height_strength >= 0.0f && l.height_strength <= 100.0f))
                return bad("asset.terrain.layer");
    } else if (h.splat_resolution != 0) {
        return bad("asset.terrain.splat_resolution", h.splat_resolution);
    }
    if ((h.flags & TerrainFlags::kHoles) && !span_at(payload, h.holes_offset, (u64(n) * n + 7) / 8, v.holes)) return bad("asset.terrain.holes");
    return v;
}

void write_terrain(const TerrainHeader& header, Span<const u16> heights, Array<byte>& out, Span<const TerrainLayer> layers,
                   Span<const u8> splat, Span<const u8> holes) {
    PayloadBuilder b(out);
    TerrainHeader h = header;
    h.heights_offset = h.layers_offset = h.splat_offset = h.holes_offset = 0;
    const usize at = b.append(&h, sizeof(h));
    h.heights_offset = b.append_span(heights);
    if (!layers.empty()) h.layers_offset = b.append_span(layers);
    if (!splat.empty()) h.splat_offset = b.append_span(splat);
    if (!holes.empty()) h.holes_offset = b.append_span(holes);
    b.patch(at, h);
    b.finish();
}

// ------------------------------------------------------------------ scene

Span<const u32> SceneView::rows(u32 column) const {
    const SceneColumn& c = columns[column];
    return Span<const u32>(reinterpret_cast<const u32*>(payload.data() + c.rows_offset), c.row_count);
}

Span<const byte> SceneView::data(u32 column) const {
    const SceneColumn& c = columns[column];
    return payload.subspan(static_cast<usize>(c.data_offset), static_cast<usize>(c.row_count) * c.type_size);
}

const SceneColumn* SceneView::find_column(u64 type_id) const {
    for (const SceneColumn& c : columns)
        if (c.type_id == type_id) return &c;
    return nullptr;
}

StringView SceneView::entity_name(u32 entity) const {
    MCE_ASSERT_MSG(entity < header->entity_count, "SceneView::entity_name out of range");
    const u32* table = reinterpret_cast<const u32*>(payload.data() + header->names_offset);
    const char* blob = reinterpret_cast<const char*>(table + 2 * header->entity_count);
    return StringView(blob + table[2 * entity], table[2 * entity + 1]);
}

Result<SceneView> SceneView::read(Span<const byte> payload) {
    constexpr const char* kStage = "asset.scene.read";
    const auto* h = header_of<SceneHeader>(payload);
    if (!h) return Error(Errc::Truncated, kStage);
    SceneView v;
    v.header = h;
    v.payload = payload;
    if (!span_at(payload, h->columns_offset, h->column_count, v.columns)) return bad(kStage);
    if (!span_at(payload, h->asset_refs_offset, h->asset_ref_count, v.asset_refs)) return bad(kStage);
    if (!span_at(payload, h->asset_fixups_offset, h->asset_fixup_count, v.asset_fixups)) return bad(kStage);
    if (!span_at(payload, h->entity_fixups_offset, h->entity_fixup_count, v.entity_fixups)) return bad(kStage);
    for (u32 i = 0; i < h->column_count; ++i) {
        const SceneColumn& c = v.columns[i];
        Span<const u32> rows;
        if (!span_at(payload, c.rows_offset, c.row_count, rows)) return bad(kStage, i);
        if (c.type_size == 0 || c.type_align == 0 || !is_pow2(c.type_align) || c.data_offset % c.type_align != 0) return bad(kStage, i);
        if (!in_bounds(c.data_offset, static_cast<u64>(c.row_count) * c.type_size, payload.size())) return bad(kStage, i);
        for (u32 r = 0; r < c.row_count; ++r)
            if (rows[r] >= h->entity_count || (r > 0 && rows[r] <= rows[r - 1])) return bad(kStage, i);
    }
    auto check_fixups = [&](Span<const SceneFixup> fixups, u32 width, u32 index_limit) {
        for (const SceneFixup& f : fixups) {
            if (f.column >= h->column_count) return false;
            const SceneColumn& c = v.columns[f.column];
            if (f.row >= c.row_count || f.byte_offset > c.type_size || width > c.type_size - f.byte_offset) return false;
            if (index_limit != 0 && f.index >= index_limit) return false;
        }
        return true;
    };
    if (!check_fixups(v.asset_fixups, 16, h->asset_ref_count == 0 ? 1 : h->asset_ref_count) ||
        (h->asset_ref_count == 0 && h->asset_fixup_count != 0))
        return bad(kStage);
    if (!check_fixups(v.entity_fixups, 4, 0)) return bad(kStage);
    Span<const u32> name_table;
    if (!span_at(payload, h->names_offset, static_cast<u64>(h->entity_count) * 2, name_table)) return bad(kStage);
    const u64 table_bytes = static_cast<u64>(h->entity_count) * 8;
    if (h->names_size < table_bytes || !in_bounds(h->names_offset, h->names_size, payload.size())) return bad(kStage);
    const u64 blob_size = h->names_size - table_bytes;
    for (u32 e = 0; e < h->entity_count; ++e)
        if (!in_bounds(name_table[2 * e], name_table[2 * e + 1], blob_size)) return bad(kStage, e);
    return v;
}

// ------------------------------------------------------------------ mod info

Result<ModInfoView> ModInfoView::read(Span<const byte> payload) {
    constexpr const char* kStage = "asset.mod_info.read";
    const auto* h = header_of<ModInfoHeader>(payload);
    if (!h) return Error(Errc::Truncated, kStage);
    for (u32 r : h->reserved)
        if (r != 0) return Error(Errc::ReservedNonZero, kStage);
    if (!in_bounds(sizeof(ModInfoHeader), h->strings_size, payload.size())) return bad(kStage);
    const char* blob = reinterpret_cast<const char*>(payload.data() + sizeof(ModInfoHeader));
    const ModInfoString* fields[5] = {&h->id, &h->name, &h->version, &h->game, &h->description};
    StringView out[5];
    for (u32 i = 0; i < 5; ++i) {
        if (!in_bounds(fields[i]->offset, fields[i]->size, h->strings_size)) return bad(kStage, i);
        out[i] = StringView(blob + fields[i]->offset, fields[i]->size);
    }
    ModInfoView v;
    v.data = ModInfoData{out[0], out[1], out[2], out[3], out[4]};
    return v;
}

void write_mod_info(const ModInfoData& info, Array<byte>& out) {
    ModInfoHeader h{};
    Array<char> strings;
    auto put = [&](StringView s, ModInfoString& at) {
        at.offset = static_cast<u32>(strings.size());
        at.size = static_cast<u32>(s.size());
        for (char c : s) strings.push_back(c);
    };
    put(info.id, h.id);
    put(info.name, h.name);
    put(info.version, h.version);
    put(info.game, h.game);
    put(info.description, h.description);
    h.strings_size = static_cast<u32>(strings.size());
    out.clear();
    out.resize(align_up(sizeof(h) + strings.size(), CHUNK_ALIGN), byte{0});
    std::memcpy(out.data(), &h, sizeof(h));
    if (!strings.empty()) std::memcpy(out.data() + sizeof(h), strings.data(), strings.size());
}

}  // namespace mce::asset
