#include "mce/jobs.h"

#include "mce/assert.h"

namespace mce::jobs {
namespace {
u32 g_workers = 0;
}

void init(u32 workers) { g_workers = workers; }
void shutdown() { g_workers = 0; }
u32 worker_count() { return g_workers; }

void run(JobFn* fn, void* ctx, Counter* counter, Counter* depends) {
    // Serial inline implementation: dependencies are complete by construction.
    MCE_ASSERT_MSG(!depends || depends->pending.load() == 0, "inline jobs cannot wait on pending work");
    if (counter) counter->pending.fetch_add(1, std::memory_order_relaxed);
    fn(ctx);
    if (counter) counter->pending.fetch_sub(1, std::memory_order_release);
}

void wait_for(Counter* counter) {
    MCE_ASSERT_MSG(!counter || counter->pending.load(std::memory_order_acquire) == 0, "inline job still pending");
}

}  // namespace mce::jobs
