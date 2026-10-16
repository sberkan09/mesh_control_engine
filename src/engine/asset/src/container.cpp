#include "mce/asset/container.h"

#include <cstring>

namespace mce::asset {

const char* chunk_type_name(ChunkType t) {
    switch (t) {
        case ChunkType::Mesh: return "Mesh";
        case ChunkType::Material: return "Material";
        case ChunkType::Texture: return "Texture";
        case ChunkType::Shader: return "Shader";
        case ChunkType::Sfx: return "Sfx";
        case ChunkType::AudioPacket: return "AudioPacket";
        case ChunkType::Font: return "Font";
        case ChunkType::Atlas: return "Atlas";
        case ChunkType::Scene: return "Scene";
        case ChunkType::Prefab: return "Prefab";
        case ChunkType::Timeline: return "Timeline";
        case ChunkType::InputMap: return "InputMap";
        case ChunkType::Manifest: return "Manifest";
        case ChunkType::ParticlePreset: return "ParticlePreset";
        case ChunkType::StringTable: return "StringTable";
        case ChunkType::Tilemap: return "Tilemap";
        case ChunkType::Sequence: return "Sequence";
        case ChunkType::ModInfo: return "ModInfo";
        case ChunkType::StoreCatalog: return "StoreCatalog";
        case ChunkType::RemoteConfig: return "RemoteConfig";
        case ChunkType::GameServicesCatalog: return "GameServicesCatalog";
        case ChunkType::SpriteSheet: return "SpriteSheet";
        case ChunkType::AnimGraph: return "AnimGraph";
        case ChunkType::UiDocument: return "UiDocument";
        case ChunkType::UiTheme: return "UiTheme";
        case ChunkType::Terrain: return "Terrain";
        case ChunkType::NavMesh: return "NavMesh";
        case ChunkType::Video: return "Video";
        case ChunkType::Skeleton2D: return "Skeleton2D";
    }
    return "ChunkType(?)";
}

namespace {
constexpr const char* kStage = "asset.container.open";
constexpr usize kHeaderCrcBytes = 60;

// Range [off, off + size) inside [0, limit) without overflow.
bool in_bounds(u64 off, u64 size, u64 limit) { return off <= limit && size <= limit - off; }
}  // namespace

Result<ContainerView> ContainerView::open(Span<const byte> bytes, StringView path, OpenOptions opt) {
    if (bytes.size() < sizeof(ContainerHeader)) return Error(Errc::Truncated, kStage, path, static_cast<u32>(bytes.size()));
    // The header is read through a pointer into the caller's bytes: they must be 8-byte aligned
    // (mmap and heap buffers are). A misaligned base is a programmer error, not bad data.
    MCE_ASSERT_MSG(reinterpret_cast<std::uintptr_t>(bytes.data()) % alignof(u64) == 0,
                   "container bytes must be 8-byte aligned");
    const auto* h = reinterpret_cast<const ContainerHeader*>(bytes.data());

    if (h->magic != CONTAINER_MAGIC) return Error(Errc::BadMagic, kStage, path, h->magic);
    if (h->version_major != CONTAINER_VERSION_MAJOR) return Error(Errc::VersionTooNew, kStage, path, h->version_major);
    for (u8 r : h->reserved)
        if (r != 0) return Error(Errc::ReservedNonZero, kStage, path);

    if (h->total_size > bytes.size()) return Error(Errc::Truncated, kStage, path, static_cast<u32>(bytes.size()));
    const u64 total = h->total_size;
    if (total < sizeof(ContainerHeader)) return Error(Errc::OutOfBounds, kStage, path);
    if (h->toc_offset != sizeof(ContainerHeader) ||
        !in_bounds(h->toc_offset, static_cast<u64>(h->chunk_count) * sizeof(TocEntry), total))
        return Error(Errc::OutOfBounds, kStage, path, static_cast<u32>(h->toc_offset));
    const u64 toc_end = h->toc_offset + static_cast<u64>(h->chunk_count) * sizeof(TocEntry);
    if (h->payload_offset < toc_end || h->payload_offset > total)
        return Error(Errc::OutOfBounds, kStage, path, static_cast<u32>(h->payload_offset));
    if (h->payload_offset % CHUNK_ALIGN != 0) return Error(Errc::Misaligned, kStage, path, static_cast<u32>(h->payload_offset));

    const auto* toc = reinterpret_cast<const TocEntry*>(bytes.data() + h->toc_offset);
    for (u32 i = 0; i < h->chunk_count; ++i) {
        const TocEntry& e = toc[i];
        if (e.reserved0 != 0 || e.reserved1 != 0) return Error(Errc::ReservedNonZero, kStage, path, i);
        if (e.offset < h->payload_offset || !in_bounds(e.offset, e.size, total)) return Error(Errc::OutOfBounds, kStage, path, i);
        if (e.offset % CHUNK_ALIGN != 0) return Error(Errc::Misaligned, kStage, path, i);
    }

    if (crc32(bytes.data(), kHeaderCrcBytes) != h->header_crc) return Error(Errc::CrcMismatch, kStage, path);
    if (opt.verify_payload_crc) {
        const u32 crc = crc32(bytes.data() + sizeof(ContainerHeader), static_cast<usize>(total - sizeof(ContainerHeader)));
        if (crc != h->payload_crc) return Error(Errc::PayloadCrcMismatch, kStage, path);
    }

    ContainerView v;
    v.bytes_ = bytes.first(static_cast<usize>(total));
    v.header_ = h;
    v.toc_ = toc;
    return v;
}

const TocEntry& ContainerView::entry(u32 i) const {
    MCE_ASSERT_MSG(i < chunk_count(), "ContainerView::entry out of range");
    return toc_[i];
}

Span<const byte> ContainerView::payload(u32 i) const {
    const TocEntry& e = entry(i);
    return bytes_.subspan(static_cast<usize>(e.offset), static_cast<usize>(e.size));
}

u32 ContainerView::find(ChunkType type, Guid guid) const {
    for (u32 i = 0; i < chunk_count(); ++i) {
        if (toc_[i].type != type) continue;
        if (guid.is_null() || toc_[i].guid() == guid) return i;
    }
    return chunk_count();
}

Result<void> ContainerView::check_version(const TocEntry& e, u8 supported_major, StringView path) {
    const u8 major = chunk_version_major(e.chunk_version);
    if (major != supported_major) return Error(Errc::ChunkVersionTooNew, "asset.chunk.version", path, e.chunk_version);
    return ok();
}

void ContainerWriter::add(const ChunkDesc& desc, Span<const byte> payload) {
    const usize at = align_up(data_.size(), CHUNK_ALIGN);
    data_.resize(at + payload.size(), byte{0});
    if (!payload.empty()) std::memcpy(data_.data() + at, payload.data(), payload.size());
    chunks_.push_back(Pending{desc, at, payload.size()});
}

void ContainerWriter::finish(Array<byte>& out) const {
    const usize n = chunks_.size();
    const usize toc_offset = sizeof(ContainerHeader);
    const usize payload_offset = align_up(toc_offset + n * sizeof(TocEntry), CHUNK_ALIGN);
    const usize total = align_up(payload_offset + data_.size(), CHUNK_ALIGN);

    out.clear();
    out.resize(total, byte{0});

    ContainerHeader h{};
    h.magic = CONTAINER_MAGIC;
    h.version_major = CONTAINER_VERSION_MAJOR;
    h.version_minor = CONTAINER_VERSION_MINOR;
    h.chunk_count = static_cast<u32>(n);
    h.flags = ContainerFlags::PayloadCrcValidatedAtWrite;
    h.toc_offset = toc_offset;
    h.payload_offset = payload_offset;
    h.total_size = total;

    for (usize i = 0; i < n; ++i) {
        const Pending& p = chunks_[i];
        TocEntry e{};
        e.type = p.desc.type;
        e.chunk_version = p.desc.version;
        e.flags = p.desc.flags;
        e.user_flags = p.desc.user_flags;
        e.guid_hi = p.desc.guid.hi;
        e.guid_lo = p.desc.guid.lo;
        e.offset = payload_offset + p.data_offset;
        e.size = p.size;
        std::memcpy(out.data() + toc_offset + i * sizeof(TocEntry), &e, sizeof(e));
    }
    if (!data_.empty()) std::memcpy(out.data() + payload_offset, data_.data(), data_.size());

    h.payload_crc = crc32(out.data() + sizeof(ContainerHeader), total - sizeof(ContainerHeader));
    std::memcpy(out.data(), &h, sizeof(h));
    h.header_crc = crc32(out.data(), kHeaderCrcBytes);
    std::memcpy(out.data(), &h, sizeof(h));
}

}  // namespace mce::asset
