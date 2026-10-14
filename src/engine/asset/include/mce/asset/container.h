#pragma once
// The `.mce` container (assets.md §3, FR-AST-020): header, TOC, per-chunk schema versions, two
// CRCs, 16-byte aligned payloads. One reader for every mount shape (per-asset dev file, dev
// directory, shipping pack) and one writer used by the cook.
//
// File layout (little-endian):
//   [0, 64)                ContainerHeader
//   [64, 64 + 48 * N)      TocEntry[N]
//   [payload_offset, ...)  chunk payloads, each starting 16-byte aligned, zero padding between
//
// Deviations from the assets.md §3 sketch, both forced by arithmetic: the sketch's field order
// cannot satisfy its own static_asserts under natural alignment (the header comes to 80 bytes,
// the TOC entry to 48). The header keeps 64 bytes by moving `flags` into the padding hole and
// `header_crc` to the last 4 bytes (it covers [0, 60) exactly as specified); the TOC entry is
// 48 bytes with explicit reserved fields. payload_crc covers [64, total_size): the TOC and every
// payload, so a corrupt TOC entry that still passes the bounds checks is caught too.
//
// Validation order is part of the contract (assets.md §3): size → magic → version → reserved →
// bounds → alignment → header CRC → payload CRC. Every failure is a distinct Errc.

#include "mce/asset/guid.h"
#include "mce/containers/array.h"
#include "mce/result.h"

namespace mce::asset {

inline constexpr u32 CONTAINER_MAGIC = 0x3145434D;  // "MCE1" LE
inline constexpr u16 CONTAINER_VERSION_MAJOR = 1;
inline constexpr u16 CONTAINER_VERSION_MINOR = 0;
inline constexpr usize CHUNK_ALIGN = 16;

enum class ChunkType : u16 {  // closed set, appended only
    Mesh = 1,
    Material,
    Texture,
    Shader,
    Sfx,
    AudioPacket,
    Font,
    Atlas,
    Scene,
    Prefab,
    Timeline,
    InputMap,
    Manifest,
    ParticlePreset,  // v0.5
    StringTable,     // CHNK_STR: one locale's string table (M-MOBILE 11a; mce/loc/string_table.h)
    Tilemap,         // v0.5: CHNK_TILE, one tile layer (FR-R2D-115)
    Sequence,        // v0.5: CHNK_SEQ, one action-list sequence (FR-ENT-100)
    ModInfo,         // CHNK_MOD: a mod pack's identity (FR-RUN-090, ADR-0034); pack-level, GUID zero
    StoreCatalog,    // CHNK_STORE: a project's products and ad placements (M-MONETIZE; mce/monetize/catalog.h)
    RemoteConfig,    // CHNK_RCFG: a project's remote config defaults (M-LIVEOPS; mce/liveops/remote_config.h)
    GameServicesCatalog,  // CHNK_GSVC: achievements, leaderboards, cloud saves (M-LIVEOPS; mce/liveops/game_services.h)
    SpriteSheet,     // CHNK_SHEET: texture regions + named flipbook clips (M-DATA-REACH 5)
    AnimGraph,       // CHNK_ANIMG: a 3D animation state graph (M-DATA-REACH 7)
};

const char* chunk_type_name(ChunkType t);

namespace ContainerFlags {
enum : u32 {
    PayloadCrcValidatedAtWrite = 1u << 0,
};
}

namespace ChunkFlags {
enum : u16 {
    Streaming = 1u << 0,  // read sequentially, don't preload
};
}

struct ContainerHeader {  // 64 bytes exactly
    u32 magic;            // == CONTAINER_MAGIC
    u16 version_major;    // breaking changes bump major -> reject
    u16 version_minor;    // additive changes bump minor -> accept
    u32 chunk_count;
    u32 flags;            // ContainerFlags
    u64 toc_offset;       // byte offset to TocEntry[chunk_count]
    u64 payload_offset;   // first chunk payload; 16-byte aligned
    u64 total_size;       // for mmap bounds checking
    u32 payload_crc;      // CRC-32 of bytes [64, total_size)
    u8 reserved[16];      // must be zero; reader rejects nonzero
    u32 header_crc;       // CRC-32 (IEEE) of bytes [0, 60)
};
static_assert(sizeof(ContainerHeader) == 64);

// Per-type schema version: high byte = major (reader must match), low byte = minor (additive).
constexpr u16 chunk_version(u8 major, u8 minor) { return static_cast<u16>((major << 8) | minor); }
constexpr u8 chunk_version_major(u16 v) { return static_cast<u8>(v >> 8); }

struct TocEntry {  // 48 bytes
    ChunkType type;
    u16 chunk_version;  // chunk_version(major, minor)
    u16 flags;          // ChunkFlags
    u16 reserved0;      // zero
    u32 user_flags;     // type-specific (e.g. texture: format enum)
    u32 reserved1;      // zero
    u64 guid_hi;        // owning asset GUID (or 0 for anonymous chunks)
    u64 guid_lo;
    u64 offset;         // absolute, from container start
    u64 size;           // raw payload size

    Guid guid() const { return Guid{guid_hi, guid_lo}; }
};
static_assert(sizeof(TocEntry) == 48);

struct OpenOptions {
    bool verify_payload_crc = true;  // dev mounts verify; a trusted pack verifies once at boot
};

// Non-owning, validated view over container bytes (a mapped file or a buffer). The bytes must
// outlive the view.
class ContainerView {
public:
    ContainerView() = default;

    // `path` only labels errors.
    static Result<ContainerView> open(Span<const byte> bytes, StringView path = {}, OpenOptions opt = {});

    const ContainerHeader& header() const { return *header_; }
    u32 chunk_count() const { return header_ ? header_->chunk_count : 0; }
    const TocEntry& entry(u32 i) const;
    Span<const byte> payload(u32 i) const;
    Span<const byte> bytes() const { return bytes_; }

    // First chunk of `type` owned by `guid` (any GUID when guid is null); chunk_count() if absent.
    u32 find(ChunkType type, Guid guid = {}) const;

    // Checks the chunk's schema major version against what the caller can read.
    static Result<void> check_version(const TocEntry& e, u8 supported_major, StringView path = {});

private:
    Span<const byte> bytes_;
    const ContainerHeader* header_ = nullptr;
    const TocEntry* toc_ = nullptr;
};

// Builds a container in memory. Chunks are laid out in insertion order (the caller owns
// determinism — the cook adds them in a fixed order). No timestamps or environment data.
class ContainerWriter {
public:
    explicit ContainerWriter(mem::IAllocator& a = mem::heap()) : chunks_(a), data_(a) {}

    struct ChunkDesc {
        ChunkType type = ChunkType::Manifest;
        u16 version = chunk_version(1, 0);
        u16 flags = 0;
        u32 user_flags = 0;
        Guid guid;
    };
    void add(const ChunkDesc& desc, Span<const byte> payload);
    usize chunk_count() const { return chunks_.size(); }

    // Serializes into `out` (replacing its contents).
    void finish(Array<byte>& out) const;

private:
    struct Pending {
        ChunkDesc desc;
        usize data_offset;
        usize size;
    };
    Array<Pending> chunks_;
    Array<byte> data_;
};

// Byte-view helpers for chunk payload building/reading.
template <class T>
Span<const byte> as_bytes_of(const T& v) {
    return Span<const byte>(reinterpret_cast<const byte*>(&v), sizeof(T));
}

}  // namespace mce::asset
