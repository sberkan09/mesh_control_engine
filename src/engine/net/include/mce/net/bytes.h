#pragma once
// Message codec for mce_net (ADR-0035): explicit little-endian scalars, LEB128 varints and
// length-prefixed byte strings. The reader never reads past its span: a short or malformed
// message sets ok() to false and every later read returns zero, so a hostile packet costs one
// check at the end instead of one per field.

#include <cstring>

#include "mce/containers/array.h"
#include "mce/types.h"

namespace mce::net {

class ByteWriter {
public:
    ByteWriter() = default;
    explicit ByteWriter(mem::IAllocator& a) : buf_(a) {}

    void write_u8(u8 v) { buf_.push_back(v); }
    void write_u16(u16 v) { put_le(v, 2); }
    void write_u32(u32 v) { put_le(v, 4); }
    void write_u64(u64 v) { put_le(v, 8); }
    void write_i32(i32 v) { put_le(static_cast<u32>(v), 4); }
    void write_f32(f32 v) {
        u32 bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        put_le(bits, 4);
    }
    void write_bool(bool v) { write_u8(v ? 1 : 0); }
    // LEB128: 1 byte below 128, at most 5 bytes for a u32.
    void write_varu32(u32 v) {
        while (v >= 0x80) {
            write_u8(static_cast<u8>(v | 0x80));
            v >>= 7;
        }
        write_u8(static_cast<u8>(v));
    }
    void write_bytes(Span<const u8> bytes) {
        write_varu32(static_cast<u32>(bytes.size()));
        append(bytes);
    }
    void write_string(StringView s) { write_bytes(Span<const u8>(reinterpret_cast<const u8*>(s.data()), s.size())); }
    // Raw bytes, no length prefix.
    void append(Span<const u8> bytes) {
        const usize at = buf_.size();
        buf_.resize(at + bytes.size());
        if (!bytes.empty()) std::memcpy(buf_.data() + at, bytes.data(), bytes.size());
    }

    Span<const u8> data() const { return buf_.span(); }
    usize size() const { return buf_.size(); }
    void clear() { buf_.clear(); }

private:
    void put_le(u64 v, int n) {
        for (int i = 0; i < n; ++i) buf_.push_back(static_cast<u8>(v >> (8 * i)));
    }
    Array<u8> buf_;
};

class ByteReader {
public:
    explicit ByteReader(Span<const u8> data) : data_(data) {}

    u8 read_u8() { return static_cast<u8>(get_le(1)); }
    u16 read_u16() { return static_cast<u16>(get_le(2)); }
    u32 read_u32() { return static_cast<u32>(get_le(4)); }
    u64 read_u64() { return get_le(8); }
    i32 read_i32() { return static_cast<i32>(static_cast<u32>(get_le(4))); }
    f32 read_f32() {
        const u32 bits = static_cast<u32>(get_le(4));
        f32 v = 0;
        std::memcpy(&v, &bits, sizeof v);
        return v;
    }
    bool read_bool() { return read_u8() != 0; }
    u32 read_varu32() {
        u32 v = 0;
        for (u32 shift = 0; shift < 35; shift += 7) {
            const u8 b = read_u8();
            if (!ok_) return 0;
            if (shift == 28 && (b & 0x70) != 0) return fail();  // more than 32 bits
            v |= u32(b & 0x7F) << shift;
            if ((b & 0x80) == 0) return v;
        }
        return fail();
    }
    // A length-prefixed span into the message (valid while the message is).
    Span<const u8> read_bytes() {
        const u32 n = read_varu32();
        if (!ok_ || n > remaining()) {
            fail();
            return {};
        }
        Span<const u8> out = data_.subspan(pos_, n);
        pos_ += n;
        return out;
    }
    StringView read_string() {
        const Span<const u8> b = read_bytes();
        return StringView(reinterpret_cast<const char*>(b.data()), b.size());
    }

    bool ok() const { return ok_; }
    bool at_end() const { return pos_ == data_.size(); }
    usize remaining() const { return data_.size() - pos_; }
    usize position() const { return pos_; }

private:
    u32 fail() {
        ok_ = false;
        pos_ = data_.size();
        return 0;
    }
    u64 get_le(usize n) {
        if (!ok_ || n > remaining()) return fail();
        u64 v = 0;
        for (usize i = 0; i < n; ++i) v |= u64(data_[pos_ + i]) << (8 * i);
        pos_ += n;
        return v;
    }

    Span<const u8> data_;
    usize pos_ = 0;
    bool ok_ = true;
};

}  // namespace mce::net
