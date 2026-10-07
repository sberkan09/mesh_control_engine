#include "mce/asset/file_io.h"

#include <cstdio>

namespace mce::asset {

Result<void> read_file(StringView path, Array<byte>& out) {
    constexpr const char* kStage = "asset.file.read";
    FixedString<1024> p(path);
    if (p.truncated()) return Error(Errc::InvalidArgument, kStage, path);
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return Error(Errc::FileNotFound, kStage, path);
    out.clear();
    bool ok_read = std::fseek(f, 0, SEEK_END) == 0;
    const long size = ok_read ? std::ftell(f) : -1;
    ok_read = ok_read && size >= 0 && std::fseek(f, 0, SEEK_SET) == 0;
    if (ok_read) {
        out.resize_uninitialized(static_cast<usize>(size));
        ok_read = size == 0 || std::fread(out.data(), 1, static_cast<usize>(size), f) == static_cast<usize>(size);
    }
    std::fclose(f);
    if (!ok_read) {
        out.clear();
        return Error(Errc::IoError, kStage, path);
    }
    return ok();
}

}  // namespace mce::asset
