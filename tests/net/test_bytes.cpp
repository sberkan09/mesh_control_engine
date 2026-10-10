#include <doctest/doctest.h>

#include "mce/net/bytes.h"

using namespace mce;
using namespace mce::net;

TEST_CASE("net bytes: scalars, varints and strings round-trip little-endian") {
    ByteWriter w;
    w.write_u8(0xAB);
    w.write_u16(0x1234);
    w.write_u32(0xDEADBEEF);
    w.write_u64(0x0102030405060708ull);
    w.write_i32(-7);
    w.write_f32(-1.5f);
    w.write_bool(true);
    w.write_varu32(0);
    w.write_varu32(127);
    w.write_varu32(128);
    w.write_varu32(0xFFFFFFFFu);
    w.write_string("hello");

    const Span<const u8> d = w.data();
    CHECK(d[1] == 0x34);  // u16 low byte first
    CHECK(d[2] == 0x12);

    ByteReader r(d);
    CHECK(r.read_u8() == 0xAB);
    CHECK(r.read_u16() == 0x1234);
    CHECK(r.read_u32() == 0xDEADBEEF);
    CHECK(r.read_u64() == 0x0102030405060708ull);
    CHECK(r.read_i32() == -7);
    CHECK(r.read_f32() == -1.5f);
    CHECK(r.read_bool());
    CHECK(r.read_varu32() == 0);
    CHECK(r.read_varu32() == 127);
    CHECK(r.read_varu32() == 128);
    CHECK(r.read_varu32() == 0xFFFFFFFFu);
    CHECK(r.read_string() == "hello");
    CHECK(r.ok());
    CHECK(r.at_end());
}

TEST_CASE("net bytes: varint sizes") {
    ByteWriter w;
    w.write_varu32(127);
    CHECK(w.size() == 1);
    w.clear();
    w.write_varu32(16384);
    CHECK(w.size() == 3);
    w.clear();
    w.write_varu32(0xFFFFFFFFu);
    CHECK(w.size() == 5);
}

TEST_CASE("net bytes: a short message fails once and reads zeros after") {
    const u8 bytes[3] = {1, 2, 3};
    ByteReader r(Span<const u8>(bytes, 3));
    CHECK(r.read_u16() == 0x0201);
    CHECK(r.ok());
    CHECK(r.read_u32() == 0);  // only one byte left
    CHECK_FALSE(r.ok());
    CHECK(r.read_u8() == 0);  // sticky
    CHECK_FALSE(r.ok());
}

TEST_CASE("net bytes: hostile lengths and varints are rejected") {
    SUBCASE("length prefix past the end") {
        ByteWriter w;
        w.write_varu32(1000);
        w.write_u8(1);
        ByteReader r(w.data());
        CHECK(r.read_bytes().empty());
        CHECK_FALSE(r.ok());
    }
    SUBCASE("varint longer than 32 bits") {
        const u8 bytes[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};
        ByteReader r(Span<const u8>(bytes, 6));
        CHECK(r.read_varu32() == 0);
        CHECK_FALSE(r.ok());
    }
    SUBCASE("unterminated varint") {
        const u8 bytes[2] = {0x80, 0x80};
        ByteReader r(Span<const u8>(bytes, 2));
        r.read_varu32();
        CHECK_FALSE(r.ok());
    }
}
