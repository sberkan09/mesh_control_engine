#pragma once
// Result<T> / Error (core.md §2.3.1). Expected failures return Result; programmer errors assert.
// Never exceptions.
//
//   Result<int> parse(StringView s);
//   Result<void> load() { MCE_TRY(open_file()); MCE_TRY_ASSIGN(int v, parse("4")); return ok(); }

#include <new>
#include <type_traits>
#include <utility>

#include "mce/assert.h"
#include "mce/fixed_string.h"

namespace mce {

enum class Errc : u16 {
    // 0x0000–0x00FF core
    Ok = 0,
    Unknown = 1,
    OutOfMemory,
    AssertFailed,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    Unsupported,
    // 0x0100–0x01FF platform / filesystem
    FileNotFound = 0x0100,
    AccessDenied,
    MmapFailed,
    IoError,
    PlatformInit,
    // 0x0200 assets
    BadMagic = 0x0200,
    CrcMismatch,
    VersionTooNew,
    Truncated,
    MissingRef,
    OutOfBounds,          // TOC / chunk range outside the container or the mapped bytes
    ReservedNonZero,      // reserved header bytes set (forward-compat discipline)
    Misaligned,           // chunk payload not 16-byte aligned
    PayloadCrcMismatch,   // header CRC is CrcMismatch; payload CRC gets its own code
    ChunkVersionTooNew,
    DuplicateGuid,
    MissingSidecar,
    DependencyCycle,
    ParseError,
    ImportFailed,
    // 0x0300 rhi
    ShaderCompile = 0x0300,
    PipelineCreate,
    DeviceLost,
    // 0x0400 audio
    AudioDeviceOpen = 0x0400,
    AudioQueueFull,
    // 0x0500 physics, 0x0600 scene: allocated by their modules.
};

const char* errc_name(Errc code);

struct Error {
    Errc code = Errc::Unknown;
    const char* stage = "";   // "cook.mesh.import", "rhi.pipeline.create", ... (static string)
    FixedString<256> path;    // asset path / file (FR-DX-050: code + path + stage)
    u32 detail = 0;           // line/offset, GL error, OS error code

    Error() = default;
    Error(Errc c, const char* st, StringView p = {}, u32 d = 0) : code(c), stage(st), path(p), detail(d) {}
};

// Formats "<ErrcName> [stage] path (detail N)" into out.
void format_error(const Error& e, FixedString<512>& out);

template <class T>
class [[nodiscard]] Result {
    static_assert(!std::is_reference_v<T>, "Result<T&> is not supported");
    static_assert(!std::is_same_v<std::remove_cv_t<T>, Error>, "Result<Error> is ambiguous");

public:
    Result(const T& v) : has_(true) { new (&storage_.value) T(v); }  // NOLINT(google-explicit-constructor)
    Result(T&& v) : has_(true) { new (&storage_.value) T(std::move(v)); }  // NOLINT
    Result(const Error& e) : has_(false) { new (&storage_.error) Error(e); }  // NOLINT

    Result(const Result& o) : has_(o.has_) {
        if (has_) new (&storage_.value) T(o.storage_.value);
        else new (&storage_.error) Error(o.storage_.error);
    }
    Result(Result&& o) noexcept : has_(o.has_) {
        if (has_) new (&storage_.value) T(std::move(o.storage_.value));
        else new (&storage_.error) Error(o.storage_.error);
    }
    Result& operator=(const Result& o) {
        if (this != &o) {
            destroy();
            has_ = o.has_;
            if (has_) new (&storage_.value) T(o.storage_.value);
            else new (&storage_.error) Error(o.storage_.error);
        }
        return *this;
    }
    Result& operator=(Result&& o) noexcept {
        if (this != &o) {
            destroy();
            has_ = o.has_;
            if (has_) new (&storage_.value) T(std::move(o.storage_.value));
            else new (&storage_.error) Error(o.storage_.error);
        }
        return *this;
    }
    ~Result() { destroy(); }

    bool has_value() const { return has_; }
    explicit operator bool() const { return has_; }

    T& value() & {
        MCE_ASSERT_MSG(has_, "Result::value() on error");
        return storage_.value;
    }
    const T& value() const& {
        MCE_ASSERT_MSG(has_, "Result::value() on error");
        return storage_.value;
    }
    T&& value() && {
        MCE_ASSERT_MSG(has_, "Result::value() on error");
        return std::move(storage_.value);
    }
    T value_or(T def) const { return has_ ? storage_.value : def; }

    const Error& error() const {
        MCE_ASSERT_MSG(!has_, "Result::error() on value");
        return storage_.error;
    }

private:
    void destroy() {
        if (has_) storage_.value.~T();
        else storage_.error.~Error();
    }

    union Storage {
        Storage() {}
        ~Storage() {}
        T value;
        Error error;
    } storage_;
    bool has_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(const Error& e) : has_error_(true), error_(e) {}  // NOLINT(google-explicit-constructor)

    bool has_value() const { return !has_error_; }
    explicit operator bool() const { return !has_error_; }
    const Error& error() const {
        MCE_ASSERT_MSG(has_error_, "Result::error() on value");
        return error_;
    }

private:
    bool has_error_ = false;
    Error error_;
};

using ResultV = Result<void>;

inline Result<void> ok() { return {}; }

}  // namespace mce

// Propagate the error of a Result-returning expression.
#define MCE_TRY(expr)                                   \
    do {                                                \
        auto&& mce_try_result_ = (expr);                \
        if (!mce_try_result_) return mce_try_result_.error(); \
    } while (0)

#define MCE_TRY_ASSIGN_IMPL(tmp, decl, expr) \
    auto tmp = (expr);                       \
    if (!tmp) return tmp.error();            \
    decl = std::move(tmp).value()

// Evaluate expr; on error return it, otherwise assign/declare: MCE_TRY_ASSIGN(int v, parse(s));
#define MCE_TRY_ASSIGN(decl, expr) MCE_TRY_ASSIGN_IMPL(MCE_CONCAT(mce_try_tmp_, __LINE__), decl, expr)
