#include "mce/asset/formats.h"

#include <algorithm>
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

Span<const byte> TextureView::pixels(u32 mip, u32 layer) const {
    MCE_ASSERT_MSG(mip < mips.size() && layer < header->layers, "TextureView::pixels out of range");
    const TextureMip& m = mips[mip];
    return payload.subspan(static_cast<usize>(m.offset + m.layer_size * layer), static_cast<usize>(m.layer_size));
}

Result<TextureView> TextureView::read(Span<const byte> payload) {
    constexpr const char* kStage = "asset.texture.read";
    const auto* h = header_of<TextureHeader>(payload);
    if (!h) return Error(Errc::Truncated, kStage);
    if (h->format != TextureFormat::RGBA8) return Error(Errc::Unsupported, kStage, {}, static_cast<u32>(h->format));
    if (h->width == 0 || h->height == 0 || h->layers == 0 || h->mip_count == 0 || h->mip_count > 32) return bad(kStage);
    TextureView v;
    v.header = h;
    v.payload = payload;
    if (!span_at(payload, sizeof(TextureHeader), h->mip_count, v.mips)) return bad(kStage);
    for (u32 i = 0; i < h->mip_count; ++i) {
        const TextureMip& m = v.mips[i];
        const u64 expect = static_cast<u64>(m.width) * m.height * 4;
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
        const u64 layer_size = static_cast<u64>(mips[i].width) * mips[i].height * 4;
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
    if (h->flags & ~u32(MeshFlags::Index32)) return Error(Errc::ReservedNonZero, kStage);
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

}  // namespace mce::asset
