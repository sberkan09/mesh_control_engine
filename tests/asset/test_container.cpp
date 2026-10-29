// .mce container: round trip, the negative suite (every failure a distinct Errc, never a crash)
// and a deterministic byte-mutation fuzz pass (assets.md §10, FR-AST-020).

#include <doctest/doctest.h>

#include <cstring>

#include "mce/asset/container.h"
#include "mce/asset/guid.h"
#include "mce/asset/xxhash.h"

using namespace mce;
using namespace mce::asset;

namespace {

Span<const byte> bytes_of(const char* s) { return Span<const byte>(reinterpret_cast<const byte*>(s), std::strlen(s)); }

void build_sample(Array<byte>& out) {
    ContainerWriter w;
    ContainerWriter::ChunkDesc a;
    a.type = ChunkType::Texture;
    a.version = chunk_version(1, 2);
    a.user_flags = 7;
    a.guid = Guid{0x1111, 0x2222};
    w.add(a, bytes_of("hello texture"));
    ContainerWriter::ChunkDesc b;
    b.type = ChunkType::Sfx;
    b.flags = ChunkFlags::Streaming;
    b.guid = Guid{0x3333, 0x4444};
    w.add(b, bytes_of("x"));
    ContainerWriter::ChunkDesc c;
    c.type = ChunkType::Manifest;
    w.add(c, {});
    w.finish(out);
}

ContainerHeader& header_of(Array<byte>& b) { return *reinterpret_cast<ContainerHeader*>(b.data()); }
TocEntry& toc_of(Array<byte>& b, u32 i) { return *reinterpret_cast<TocEntry*>(b.data() + sizeof(ContainerHeader) + i * sizeof(TocEntry)); }

void refresh_header_crc(Array<byte>& b) { header_of(b).header_crc = crc32(b.data(), 60); }
void refresh_crcs(Array<byte>& b) {
    header_of(b).payload_crc = crc32(b.data() + 64, b.size() - 64);
    refresh_header_crc(b);
}

Errc open_code(const Array<byte>& b) {
    auto r = ContainerView::open(b.span(), "test.mce");
    return r ? Errc::Ok : r.error().code;
}

}  // namespace

TEST_CASE("container: write then read round-trips chunks, alignment and metadata") {
    Array<byte> file;
    build_sample(file);
    CHECK(file.size() % CHUNK_ALIGN == 0);
    auto r = ContainerView::open(file.span(), "sample.mce");
    REQUIRE(r);
    const ContainerView& v = r.value();
    REQUIRE(v.chunk_count() == 3);
    CHECK(v.header().version_major == CONTAINER_VERSION_MAJOR);
    CHECK((v.header().flags & ContainerFlags::PayloadCrcValidatedAtWrite) != 0);
    CHECK(v.entry(0).type == ChunkType::Texture);
    CHECK(v.entry(0).chunk_version == chunk_version(1, 2));
    CHECK(v.entry(0).user_flags == 7);
    CHECK(v.entry(0).guid() == Guid{0x1111, 0x2222});
    CHECK(v.entry(1).flags == ChunkFlags::Streaming);
    for (u32 i = 0; i < 3; ++i) CHECK(v.entry(i).offset % CHUNK_ALIGN == 0);
    const auto p0 = v.payload(0);
    CHECK(std::string_view(reinterpret_cast<const char*>(p0.data()), p0.size()) == "hello texture");
    CHECK(v.payload(1).size() == 1);
    CHECK(v.payload(2).empty());
    CHECK(v.find(ChunkType::Sfx) == 1);
    CHECK(v.find(ChunkType::Texture, Guid{0x1111, 0x2222}) == 0);
    CHECK(v.find(ChunkType::Texture, Guid{0x9, 0x9}) == v.chunk_count());
    CHECK(v.find(ChunkType::Font) == v.chunk_count());
}

TEST_CASE("container: writer output is deterministic") {
    Array<byte> a, b;
    build_sample(a);
    build_sample(b);
    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
    CHECK(xxh64(a.data(), a.size()) == xxh64(b.data(), b.size()));
}

TEST_CASE("container: empty container is valid") {
    ContainerWriter w;
    Array<byte> file;
    w.finish(file);
    auto r = ContainerView::open(file.span());
    REQUIRE(r);
    CHECK(r.value().chunk_count() == 0);
}

TEST_CASE("container: negative suite — each failure is a distinct, diagnosable error") {
    Array<byte> good;
    build_sample(good);
    REQUIRE(open_code(good) == Errc::Ok);

    SUBCASE("truncated below the header") {
        Array<byte> b = good;
        b.resize(40);
        CHECK(open_code(b) == Errc::Truncated);
    }
    SUBCASE("truncated file (total_size beyond the bytes)") {
        Array<byte> b = good;
        b.resize(b.size() - 16);
        CHECK(open_code(b) == Errc::Truncated);
    }
    SUBCASE("bad magic") {
        Array<byte> b = good;
        header_of(b).magic = 0x12345678;
        CHECK(open_code(b) == Errc::BadMagic);
    }
    SUBCASE("version ahead") {
        Array<byte> b = good;
        header_of(b).version_major = CONTAINER_VERSION_MAJOR + 1;
        refresh_header_crc(b);
        CHECK(open_code(b) == Errc::VersionTooNew);
        auto r = ContainerView::open(b.span(), "ahead.mce");
        REQUIRE(!r);
        CHECK(r.error().path == "ahead.mce");
        CHECK(r.error().detail == CONTAINER_VERSION_MAJOR + 1);
    }
    SUBCASE("newer minor version is accepted") {
        Array<byte> b = good;
        header_of(b).version_minor = 9;
        refresh_header_crc(b);
        CHECK(open_code(b) == Errc::Ok);
    }
    SUBCASE("nonzero reserved header bytes") {
        Array<byte> b = good;
        header_of(b).reserved[3] = 1;
        refresh_header_crc(b);
        CHECK(open_code(b) == Errc::ReservedNonZero);
    }
    SUBCASE("nonzero reserved TOC fields") {
        Array<byte> b = good;
        toc_of(b, 1).reserved1 = 5;
        refresh_crcs(b);
        CHECK(open_code(b) == Errc::ReservedNonZero);
    }
    SUBCASE("TOC beyond total_size") {
        Array<byte> b = good;
        header_of(b).chunk_count = 100000;
        refresh_header_crc(b);
        CHECK(open_code(b) == Errc::OutOfBounds);
    }
    SUBCASE("chunk offset out of bounds") {
        Array<byte> b = good;
        toc_of(b, 0).offset = b.size() + 16;
        refresh_crcs(b);
        CHECK(open_code(b) == Errc::OutOfBounds);
    }
    SUBCASE("chunk size overflowing past the end") {
        Array<byte> b = good;
        toc_of(b, 0).size = ~u64(0) - 4;
        refresh_crcs(b);
        CHECK(open_code(b) == Errc::OutOfBounds);
    }
    SUBCASE("chunk pointing into the TOC") {
        Array<byte> b = good;
        toc_of(b, 0).offset = 64;
        refresh_crcs(b);
        CHECK(open_code(b) == Errc::OutOfBounds);
    }
    SUBCASE("misaligned chunk payload") {
        Array<byte> b = good;
        toc_of(b, 0).offset += 4;
        toc_of(b, 0).size = 1;
        refresh_crcs(b);
        CHECK(open_code(b) == Errc::Misaligned);
    }
    SUBCASE("header CRC mismatch") {
        Array<byte> b = good;
        header_of(b).flags ^= 0x80;
        CHECK(open_code(b) == Errc::CrcMismatch);
    }
    SUBCASE("payload CRC mismatch") {
        Array<byte> b = good;
        b[static_cast<usize>(toc_of(b, 0).offset)] ^= byte{0x01};
        CHECK(open_code(b) == Errc::PayloadCrcMismatch);
        auto skip = ContainerView::open(b.span(), "trusted.mce", OpenOptions{false});
        CHECK(skip.has_value());  // a trusted pack skips the payload CRC
    }
}

TEST_CASE("container: chunk schema version check") {
    TocEntry e{};
    e.chunk_version = chunk_version(1, 7);
    CHECK(ContainerView::check_version(e, 1).has_value());  // newer minor: accepted
    e.chunk_version = chunk_version(2, 0);
    auto r = ContainerView::check_version(e, 1, "x.mce");
    REQUIRE(!r);
    CHECK(r.error().code == Errc::ChunkVersionTooNew);
}

TEST_CASE("container: fuzz — 1000 deterministic byte mutations never crash") {
    Array<byte> good;
    build_sample(good);
    u64 state = 0x9E3779B97F4A7C15ull;
    auto next = [&] {
        state = mix64(state + 0x9E3779B97F4A7C15ull);
        return state;
    };
    u32 rejected = 0;
    for (int iter = 0; iter < 1000; ++iter) {
        Array<byte> b = good;
        const u32 flips = 1 + static_cast<u32>(next() % 4);
        for (u32 f = 0; f < flips; ++f) b[static_cast<usize>(next() % b.size())] = static_cast<byte>(next() & 0xFF);
        if (next() % 8 == 0) b.resize(static_cast<usize>(next() % b.size()));
        // Half the mutants get valid CRCs so the structural checks behind them are exercised too.
        if (iter % 2 == 0 && b.size() >= 64) refresh_crcs(b);
        auto r = ContainerView::open(b.span());
        if (!r) {
            ++rejected;
            continue;
        }
        // Whatever opens must be internally consistent: every payload inside the bytes.
        const ContainerView& v = r.value();
        for (u32 i = 0; i < v.chunk_count(); ++i) {
            const auto p = v.payload(i);
            CHECK(p.data() >= b.data());
            CHECK(p.data() + p.size() <= b.data() + b.size());
        }
    }
    CHECK(rejected > 400);
}

TEST_CASE("container: a writer with thousands of chunks keeps every payload and its alignment (M-BENCH 3 growth path)") {
    ContainerWriter w;
    constexpr u32 kChunks = 3000;
    for (u32 i = 0; i < kChunks; ++i) {
        ContainerWriter::ChunkDesc d;
        d.type = ChunkType::Texture;
        d.guid = Guid{0x1000u + i, 0x2000u + i};
        const u32 size = 1 + (i * 37u) % 300;  // sizes that straddle the alignment
        Array<byte> payload;
        payload.resize(size, static_cast<byte>(i));
        w.add(d, payload.span());
    }
    Array<byte> file;
    w.finish(file);
    auto r = ContainerView::open(file.span(), "many.mce");
    REQUIRE(r);
    const ContainerView& v = r.value();
    REQUIRE(v.chunk_count() == kChunks);
    for (u32 i = 0; i < kChunks; i += 97) {
        CAPTURE(i);
        const u32 at = v.find(ChunkType::Texture, Guid{0x1000u + i, 0x2000u + i});
        REQUIRE(at == i);
        CHECK(v.entry(at).offset % CHUNK_ALIGN == 0);
        const Span<const byte> p = v.payload(at);
        CHECK(p.size() == 1 + (i * 37u) % 300);
        CHECK(p[0] == static_cast<byte>(i));
        CHECK(p[p.size() - 1] == static_cast<byte>(i));
    }
}
