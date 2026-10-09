#include "mce/log.h"

#include <mutex>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace mce::log {
namespace {

spdlog::level::level_enum to_spd(Level l) { return static_cast<spdlog::level::level_enum>(static_cast<int>(l)); }
Level from_spd(spdlog::level::level_enum l) { return static_cast<Level>(static_cast<int>(l)); }

// Fixed-size circular stores; guarded by one mutex (logging is never on a hot path).
struct Rings {
    std::mutex mutex;
    ConsoleLine console[kConsoleRingSize];
    ConsoleLine crumbs[kBreadcrumbCount];
    u64 next_seq = 1;
    u64 crumb_count = 0;
};

Rings& rings() {
    static Rings r;
    return r;
}

class RingSink final : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        Rings& r = rings();
        std::lock_guard<std::mutex> lock(r.mutex);
        const u64 seq = r.next_seq++;
        ConsoleLine& line = r.console[seq % kConsoleRingSize];
        line.seq = seq;
        line.level = from_spd(msg.level);
        line.text.assign(StringView(msg.payload.data(), msg.payload.size()));
        if (msg.level >= spdlog::level::warn) {
            r.crumbs[r.crumb_count % kBreadcrumbCount] = line;
            ++r.crumb_count;
        }
    }
    void flush_() override {}
};

}  // namespace

void init(const LogDesc& desc) {
    std::vector<spdlog::sink_ptr> sinks;  // mce-allow: spdlog API takes std::vector
    if (desc.console) sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    if (desc.file_path) {
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(desc.file_path, 4 * MiB, 3));
    }
    sinks.push_back(std::make_shared<RingSink>());
    auto logger = std::make_shared<spdlog::logger>(desc.app_name, sinks.begin(), sinks.end());
    logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    logger->set_level(to_spd(desc.level));
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(std::move(logger));
}

void shutdown() {
    flush();
    spdlog::shutdown();
}

void set_level(Level level) { spdlog::default_logger_raw()->set_level(to_spd(level)); }

void flush() { spdlog::default_logger_raw()->flush(); }

usize read_console(u64 since, Span<ConsoleLine> out) {
    Rings& r = rings();
    std::lock_guard<std::mutex> lock(r.mutex);
    const u64 last = r.next_seq - 1;
    u64 first = since + 1;
    if (last >= kConsoleRingSize && first < last - kConsoleRingSize + 1) first = last - kConsoleRingSize + 1;
    usize n = 0;
    for (u64 s = first; s <= last && n < out.size(); ++s) out[n++] = r.console[s % kConsoleRingSize];
    return n;
}

u64 console_last_seq() {
    Rings& r = rings();
    std::lock_guard<std::mutex> lock(r.mutex);
    return r.next_seq - 1;
}

void visit_breadcrumbs(BreadcrumbFn fn, void* user) {
    Rings& r = rings();
    std::lock_guard<std::mutex> lock(r.mutex);
    const u64 count = r.crumb_count < kBreadcrumbCount ? r.crumb_count : kBreadcrumbCount;
    for (u64 i = r.crumb_count - count; i < r.crumb_count; ++i) fn(r.crumbs[i % kBreadcrumbCount], user);
}

bool try_visit_breadcrumbs(BreadcrumbFn fn, void* user) {
    Rings& r = rings();
    std::unique_lock<std::mutex> lock(r.mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    const u64 count = r.crumb_count < kBreadcrumbCount ? r.crumb_count : kBreadcrumbCount;
    for (u64 i = r.crumb_count - count; i < r.crumb_count; ++i) fn(r.crumbs[i % kBreadcrumbCount], user);
    return true;
}

}  // namespace mce::log
