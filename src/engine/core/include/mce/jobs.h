#pragma once
// Job system API (core.md §2.6). Counter/fork-join model. Until the runtime enablement flag lands
// (v0.5, ROADMAP M5a, gated by the determinism CI test) jobs execute inline on the caller, so
// call sites are written once and parallelism is a swap, not a migration.

#include <atomic>

#include "mce/types.h"

namespace mce::jobs {

using JobFn = void(void* ctx);

struct Counter {
    std::atomic<u32> pending{0};
};

void init(u32 workers);
void shutdown();
u32 worker_count();

// Runs fn(ctx) after `depends` reaches zero. Increments `counter` while the job is pending.
void run(JobFn* fn, void* ctx, Counter* counter, Counter* depends = nullptr);
// Blocks (helping with work) until the counter reaches zero.
void wait_for(Counter* counter);

}  // namespace mce::jobs
