#include "mce/cook/fs.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace mce::cook::fs {

namespace stdfs = std::filesystem;

namespace {
stdfs::path to_path(StringView s) { return stdfs::path(s.begin(), s.end()); }

String from_path(const stdfs::path& p) {
    const auto u8 = p.generic_u8string();
    return String(StringView(reinterpret_cast<const char*>(u8.data()), u8.size()));
}
}  // namespace

bool exists(StringView path) {
    std::error_code ec;
    return stdfs::exists(to_path(path), ec);
}

String absolute(StringView path) {
    std::error_code ec;
    const stdfs::path p = stdfs::absolute(to_path(path), ec);
    return ec ? String(path) : from_path(p.lexically_normal());
}

bool is_directory(StringView path) {
    std::error_code ec;
    return stdfs::is_directory(to_path(path), ec);
}

Result<void> create_directories(StringView path) {
    std::error_code ec;
    stdfs::create_directories(to_path(path), ec);
    if (ec) return Error(Errc::IoError, "cook.fs.mkdir", path, static_cast<u32>(ec.value()));
    return ok();
}

Result<void> write_file_atomic(StringView path, Span<const byte> bytes) {
    const stdfs::path target = to_path(path);
    std::error_code ec;
    if (target.has_parent_path()) {
        stdfs::create_directories(target.parent_path(), ec);
        if (ec) return Error(Errc::IoError, "cook.fs.mkdir", path, static_cast<u32>(ec.value()));
    }
    String tmp(path);
    tmp += ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return Error(Errc::AccessDenied, "cook.fs.write", tmp.view());
    const bool wrote = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    const bool flushed = std::fflush(f) == 0;
    std::fclose(f);
    if (!wrote || !flushed) {
        stdfs::remove(to_path(tmp.view()), ec);
        return Error(Errc::IoError, "cook.fs.write", tmp.view());
    }
    stdfs::rename(to_path(tmp.view()), target, ec);  // MoveFileExW(REPLACE_EXISTING) on Windows
    if (ec) {
        std::error_code ignore;
        stdfs::remove(to_path(tmp.view()), ignore);
        return Error(Errc::IoError, "cook.fs.rename", path, static_cast<u32>(ec.value()));
    }
    return ok();
}

Result<void> rename(StringView from, StringView to) {
    std::error_code ec;
    const stdfs::path target = to_path(to);
    if (target.has_parent_path()) stdfs::create_directories(target.parent_path(), ec);
    stdfs::rename(to_path(from), target, ec);
    if (ec) return Error(Errc::IoError, "cook.fs.rename", from, static_cast<u32>(ec.value()));
    return ok();
}

bool remove(StringView path) {
    std::error_code ec;
    return stdfs::remove(to_path(path), ec) && !ec;
}

bool read_text(StringView path, String& out) {
    const FixedString<1024> p(path);
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[4096];
    usize n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(StringView(buf, n));
    std::fclose(f);
    return true;
}

Result<void> list_files(StringView root, Array<String>& out) {
    out.clear();
    std::error_code ec;
    const stdfs::path base = to_path(root);
    stdfs::recursive_directory_iterator it(base, stdfs::directory_options::none, ec);
    if (ec) return Error(Errc::FileNotFound, "cook.fs.list", root, static_cast<u32>(ec.value()));
    const stdfs::recursive_directory_iterator end;
    while (it != end) {
        const stdfs::directory_entry& e = *it;
        const auto name = e.path().filename().u8string();
        if (!name.empty() && name[0] == u8'.') {
            if (e.is_directory(ec)) it.disable_recursion_pending();
        } else if (e.is_regular_file(ec)) {
            out.push_back(from_path(e.path().lexically_relative(base)));
        }
        it.increment(ec);
        if (ec) return Error(Errc::IoError, "cook.fs.list", root, static_cast<u32>(ec.value()));
    }
    std::sort(out.begin(), out.end());
    return ok();
}

bool stat(StringView path, u64& size, i64& mtime) {
    std::error_code ec;
    const stdfs::path p = to_path(path);
    const auto sz = stdfs::file_size(p, ec);
    if (ec) return false;
    const auto t = stdfs::last_write_time(p, ec);
    if (ec) return false;
    size = static_cast<u64>(sz);
    mtime = static_cast<i64>(t.time_since_epoch().count());
    return true;
}

}  // namespace mce::cook::fs
