#include "mce/ecs/world.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "mce/app.h"
#include "mce/hash.h"

namespace mce {

namespace {
// A parallel batch member's run context. Only worker jobs set these, so the main thread (and any
// other World on any thread) keeps using World::run_.
thread_local const World* t_world = nullptr;
thread_local ecs::detail::RunCtx* t_ctx = nullptr;
}  // namespace

namespace ecs::detail {

// Fixed worker threads for the auto-parallel scheduling experiment. run() hands out n jobs to the
// workers and the calling thread and returns once every worker has checked in for this run (so no
// straggler can still be reading the previous run's job). Workers spin briefly between batches
// (a phase is several batches in a row), then sleep.
class SystemPool {
public:
    using Job = void (*)(void* ctx, u32 index);

    explicit SystemPool(u32 workers) : threads_(mem::heap()) {
        for (u32 i = 0; i < workers; ++i) threads_.push_back(std::thread([this] { worker(); }));
    }
    ~SystemPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
            generation_.fetch_add(1, std::memory_order_release);
        }
        wake_.notify_all();
        for (std::thread& t : threads_) t.join();
    }
    u32 workers() const { return static_cast<u32>(threads_.size()); }

    void run(u32 n, Job job, void* ctx) {
        job_ = job;
        ctx_ = ctx;
        count_ = n;
        next_.store(0, std::memory_order_relaxed);
        arrived_.store(0, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            generation_.fetch_add(1, std::memory_order_release);
        }
        wake_.notify_all();
        drain();
        while (arrived_.load(std::memory_order_acquire) != threads_.size()) std::this_thread::yield();
    }

private:
    void drain() {
        for (u32 i = next_.fetch_add(1, std::memory_order_acq_rel); i < count_; i = next_.fetch_add(1, std::memory_order_acq_rel)) {
            job_(ctx_, i);
        }
    }
    void worker() {
        u64 seen = 0;
        for (;;) {
            u64 g = generation_.load(std::memory_order_acquire);
            for (u32 spin = 0; g == seen && spin < 4096; ++spin) {
                std::this_thread::yield();
                g = generation_.load(std::memory_order_acquire);
            }
            if (g == seen) {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return generation_.load(std::memory_order_acquire) != seen; });
                g = generation_.load(std::memory_order_acquire);
            }
            seen = g;
            if (quit_) return;
            drain();
            arrived_.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    Array<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::atomic<u64> generation_{0};
    std::atomic<u32> next_{0};
    std::atomic<u32> arrived_{0};  // workers done with the current run
    Job job_ = nullptr;
    void* ctx_ = nullptr;
    u32 count_ = 0;
    bool quit_ = false;
};

}  // namespace ecs::detail

const char* phase_name(Phase p) {
    switch (p) {
        case Phase::Startup: return "Startup";
        case Phase::FixedUpdate_Pre: return "FixedUpdate_Pre";
        case Phase::FixedUpdate_Physics: return "FixedUpdate_Physics";
        case Phase::FixedUpdate_Post: return "FixedUpdate_Post";
        case Phase::Render_Pre: return "Render_Pre";
        case Phase::Render_Submit: return "Render_Submit";
        case Phase::Editor: return "Editor";
        case Phase::Count: break;
    }
    return "?";
}

// ====================================================================== Table

namespace ecs::detail {

Table::Table(mem::IAllocator& alloc, u32 id, const ComponentMask& mask, Span<const ComponentInfo> infos)
    : alloc_(alloc), id_(id), mask_(mask), ids_(alloc), columns_(alloc), entities_(alloc), add_edges_(alloc),
      remove_edges_(alloc) {
    for (u8& c : col_of_) c = kNoColumn;
    for (usize c = 0; c < kMaxComponents; ++c) {
        if (!mask.test(c)) continue;
        const ComponentId cid = static_cast<ComponentId>(c);
        ids_.push_back(cid);
        const ComponentInfo& info = infos[c];
        if (info.is_tag) continue;
        col_of_[c] = static_cast<u8>(columns_.size());
        Column col;
        col.id = cid;
        col.size = info.size;
        col.align = info.align < mem::kDefaultAlign ? static_cast<u32>(mem::kDefaultAlign) : info.align;
        columns_.push_back(col);
    }
    MCE_ASSERT_MSG(columns_.size() < kNoColumn, "too many data components in one archetype");
}

Table::~Table() {
    for (Column& c : columns_) {
        if (c.data) alloc_.free(c.data);
        if (c.added) alloc_.free(c.added);
        if (c.changed) alloc_.free(c.changed);
    }
}

void Table::reserve(u32 capacity) {
    if (capacity <= capacity_) return;
    const usize rows = entities_.size();
    for (Column& c : columns_) {
        mem::Allocation m = alloc_.allocate(usize(c.size) * capacity, c.align);
        MCE_ASSERT_MSG(m.ptr != nullptr, "ECS column allocation failed");
        u8* fresh = static_cast<u8*>(m.ptr);
        if (c.data) {
            if (rows) std::memcpy(fresh, c.data, rows * c.size);
            alloc_.free(c.data);
        }
        c.data = fresh;
        for (u32** ticks : {&c.added, &c.changed}) {
            mem::Allocation t = alloc_.allocate(sizeof(u32) * capacity, alignof(u32));
            MCE_ASSERT_MSG(t.ptr != nullptr, "ECS tick column allocation failed");
            u32* fresh_ticks = static_cast<u32*>(t.ptr);
            if (*ticks) {
                if (rows) std::memcpy(fresh_ticks, *ticks, rows * sizeof(u32));
                alloc_.free(*ticks);
            }
            *ticks = fresh_ticks;
        }
    }
    entities_.reserve(capacity);
    capacity_ = capacity;
}

u32 Table::push_row(Entity e, u32 tick) { return push_rows(1, &e, tick); }

u32 Table::push_rows(u32 n, const Entity* es, u32 tick) {
    const u32 first = size();
    if (first + n > capacity_) {
        u32 cap = capacity_ ? capacity_ : 16;
        while (cap < first + n) cap *= 2;
        reserve(cap);
    }
    for (u32 i = 0; i < n; ++i) entities_.push_back(es[i]);
    for (Column& c : columns_) {
        std::memset(c.data + usize(first) * c.size, 0, usize(n) * c.size);
        for (u32 i = 0; i < n; ++i) c.added[first + i] = c.changed[first + i] = tick;
    }
    return first;
}

Entity Table::swap_remove(u32 row) {
    const u32 last = size() - 1;
    MCE_ASSERT_MSG(row <= last, "swap_remove row out of range");
    Entity moved = Entity::null;
    if (row != last) {
        for (Column& c : columns_) {
            std::memcpy(c.data + usize(row) * c.size, c.data + usize(last) * c.size, c.size);
            c.added[row] = c.added[last];
            c.changed[row] = c.changed[last];
        }
        entities_[row] = entities_[last];
        moved = entities_[row];
    }
    entities_.pop_back();
    return moved;
}

}  // namespace ecs::detail

// ====================================================================== Commands

Commands::Commands(World& w) : world_(w), bytes_(w.alloc_) {}

void* Commands::record(Op op, Entity e, ecs::ComponentId comp, usize payload_bytes) {
    const usize padded = align_up(payload_bytes, 8);
    const usize off = bytes_.size();
    bytes_.resize_uninitialized(off + sizeof(Header) + padded);
    Header h;
    h.op = op;
    h.pad = 0;
    h.comp = comp;
    h.payload = static_cast<u32>(padded);
    h.entity = e;
    std::memcpy(bytes_.data() + off, &h, sizeof(h));
    ++count_;
    return bytes_.data() + off + sizeof(Header);
}

Entity Commands::spawn() {
    const Entity e = world_.reserve_entity();
    record(Op::Spawn, e, ecs::kInvalidComponent, 0);
    return e;
}

void Commands::destroy(Entity e) { record(Op::Destroy, e, ecs::kInvalidComponent, 0); }

void Commands::spawn_raw(Entity reserved, Span<const ecs::RawComponent> components) {
    usize bytes = 0;
    for (const ecs::RawComponent& rc : components) bytes += 8 + align_up(world_.component(rc.id).size, 8);
    u8* p = static_cast<u8*>(record(Op::Spawn, reserved, ecs::kInvalidComponent, bytes));
    for (const ecs::RawComponent& rc : components) {
        const u32 size = world_.component(rc.id).size;
        std::memcpy(p, &rc.id, sizeof(rc.id));
        std::memcpy(p + 4, &size, sizeof(size));
        if (size) std::memcpy(p + 8, rc.data, size);
        p += 8 + align_up(size, 8);
    }
}

// ====================================================================== World

World::World(mem::IAllocator& alloc)
    : alloc_(alloc), components_(alloc), component_ids_(alloc), hooks_(alloc), generations_(alloc), locations_(alloc),
      free_list_(alloc), tables_(alloc), table_index_(alloc), system_names_all_(alloc), destroy_hooks_(alloc),
      events_(alloc), events_ordered_(alloc), resources_(alloc), commands_(*this) {
    for (usize i = 0; i < kPhaseCount; ++i) {
        systems_[i] = Array<SystemEntry>(alloc);
        system_name_cache_[i] = Array<const char*>(alloc);
        batch_starts_[i] = Array<u16>(alloc);
        batches_dirty_[i] = true;
        events_by_phase_[i] = Array<ecs::detail::EventQueueBase*>(alloc);
    }
    table_for(ecs::ComponentMask{});  // table 0: the empty archetype
}

World::~World() {
    pool_.reset();
    for (auto& r : resources_)
        if (r.value.ptr) r.value.destroy(alloc_, r.value.ptr);
}

void World::assert_not_in_phase(const char* what) const {
    MCE_ASSERT_MSG(!in_phase_, what);
    (void)what;
}

ecs::ComponentId World::register_component_info(const ecs::ComponentInfo& info) {
    if (const ecs::ComponentId* id = component_ids_.find(info.type_hash)) return *id;
    MCE_ASSERT_MSG(!batch_running_, "first use of a component inside a parallel batch: declare it in SystemAccess");
    MCE_ASSERT_MSG(components_.size() < ecs::kMaxComponents, "component type budget (256) exhausted");
    const auto id = static_cast<ecs::ComponentId>(components_.size());
    components_.push_back(info);
    hooks_.push_back(Hooks{});
    component_ids_.insert_or_assign(info.type_hash, id);
    return id;
}

// ---------------------------------------------------------------- entities

Entity World::reserve_entity() {
    MCE_ASSERT_MSG(!batch_running_, "entity reservation inside a parallel batch");
    u32 index;
    if (!free_list_.empty()) {
        index = free_list_.back();
        free_list_.pop_back();
    } else {
        index = static_cast<u32>(generations_.size());
        generations_.push_back(1);
        locations_.push_back(Location{});
    }
    locations_[index] = Location{kNoTable, 0};
    ++alive_count_;
    return Entity{index, generations_[index]};
}

bool World::alive(Entity e) const {
    return e.generation != 0 && e.index < generations_.size() && generations_[e.index] == e.generation;
}

void World::place_entity(Entity e, u32 table) {
    const u32 row = tables_[table]->push_row(e, write_tick());
    locations_[e.index] = Location{table, row};
    ++structure_version_;
}

Entity World::spawn() {
    assert_not_in_phase("spawn");
    const Entity e = reserve_entity();
    place_entity(e, kEmptyTable);
    return e;
}

void World::spawn_raw(Entity e, Span<const ecs::RawComponent> components) {
    assert_not_in_phase("spawn_raw");
    MCE_ASSERT_MSG(alive(e) && locations_[e.index].table == kNoTable, "spawn_raw expects a reserved, unplaced handle");
    ecs::ComponentMask mask;
    for (const ecs::RawComponent& rc : components) mask.set(rc.id);
    place_entity(e, table_for(mask).id());
    const Location loc = locations_[e.index];
    ecs::detail::Table& table = *tables_[loc.table];
    for (const ecs::RawComponent& rc : components)
        if (u8* cell = table.cell(rc.id, loc.row)) std::memcpy(cell, rc.data, components_[rc.id].size);
    for (ecs::ComponentId c : table.ids())
        if (hooks_[c].on_add) hooks_[c].on_add(*this, e);
}

void World::destroy(Entity e) {
    assert_not_in_phase("destroy");
    destroy_now(e);
}

void World::destroy_now(Entity e) {
    if (!alive(e)) return;
    // Destroy hooks may destroy other entities (hierarchy children) — never `e` itself, which
    // stays alive until below.
    for (usize i = 0; i < destroy_hooks_.size(); ++i) destroy_hooks_[i](*this, e);
    if (!alive(e)) return;
    const Location loc = locations_[e.index];
    if (loc.table != kNoTable) {
        ecs::detail::Table& t = *tables_[loc.table];
        for (ecs::ComponentId c : t.ids())
            if (hooks_[c].on_remove) hooks_[c].on_remove(*this, e);
        // Hooks must not move the entity; re-read the location defensively.
        const Location now = locations_[e.index];
        ecs::detail::Table& tt = *tables_[now.table];
        const Entity moved = tt.swap_remove(now.row);
        if (!moved.is_null()) locations_[moved.index].row = now.row;
        ++structure_version_;
    }
    if (++generations_[e.index] == 0) generations_[e.index] = 1;
    locations_[e.index] = Location{kNoTable, 0};
    free_list_.push_back(e.index);
    --alive_count_;
}

ecs::detail::Table& World::table_for(const ecs::ComponentMask& mask) {
    if (const u32* t = table_index_.find(mask)) return *tables_[*t];
    const u32 id = static_cast<u32>(tables_.size());
    auto* table = mem::make<ecs::detail::Table>(alloc_, alloc_, id, mask, components_.span());
    tables_.push_back(mem::UniquePtr<ecs::detail::Table>(alloc_, table));
    table_index_.insert_or_assign(mask, id);
    ++structure_version_;
    return *table;
}

u32 World::table_with(u32 from, ecs::ComponentId c, bool add) {
    ecs::detail::Table& src = *tables_[from];
    FlatMap<ecs::ComponentId, u32>& edges = add ? src.add_edges() : src.remove_edges();
    if (const u32* to = edges.find(c)) return *to;
    ecs::ComponentMask mask = src.mask();
    mask.assign(c, add);
    const u32 to = table_for(mask).id();
    // table_for may have grown tables_; re-fetch src before touching its edges.
    (add ? tables_[from]->add_edges() : tables_[from]->remove_edges()).insert_or_assign(c, to);
    return to;
}

u32 World::move_entity(Entity e, u32 to) {
    const Location loc = locations_[e.index];
    ecs::detail::Table& src = *tables_[loc.table];
    ecs::detail::Table& dst = *tables_[to];
    const u32 row = dst.push_row(e, write_tick());
    for (const ecs::detail::Column& col : dst.columns()) {
        if (const u8* from = src.cell(col.id, loc.row)) {
            std::memcpy(dst.cell(col.id, row), from, col.size);
            col.added[row] = src.added_ticks(col.id)[loc.row];
            col.changed[row] = src.changed_ticks(col.id)[loc.row];
        }
    }
    const Entity moved = src.swap_remove(loc.row);
    if (!moved.is_null()) locations_[moved.index].row = loc.row;
    locations_[e.index] = Location{to, row};
    ++structure_version_;
    return row;
}

void World::add_component_raw(Entity e, ecs::ComponentId c, const void* value) {
    if (locations_[e.index].table == kNoTable) place_entity(e, kEmptyTable);
    const Location loc = locations_[e.index];
    const bool had = tables_[loc.table]->mask().test(c);
    u32 table = loc.table, row = loc.row;
    if (!had) {
        table = table_with(loc.table, c, true);
        row = move_entity(e, table);
    }
    if (value && components_[c].size) {
        std::memcpy(tables_[table]->cell(c, row), value, components_[c].size);
        tables_[table]->changed_ticks(c)[row] = write_tick();
    }
    if (!had && hooks_[c].on_add) hooks_[c].on_add(*this, e);
}

void World::remove_component_raw(Entity e, ecs::ComponentId c) {
    const Location loc = locations_[e.index];
    if (loc.table == kNoTable || !tables_[loc.table]->mask().test(c)) return;
    if (hooks_[c].on_remove) hooks_[c].on_remove(*this, e);
    move_entity(e, table_with(locations_[e.index].table, c, false));
}

// ---------------------------------------------------------------- type-erased access

ecs::ComponentId World::find_component(u64 type_hash) const {
    const ecs::ComponentId* id = component_ids_.find(type_hash);
    return id ? *id : ecs::kInvalidComponent;
}

bool World::has(Entity e, ecs::ComponentId c) const {
    if (!alive(e) || c >= components_.size()) return false;
    const Location loc = locations_[e.index];
    return loc.table != kNoTable && tables_[loc.table]->mask().test(c);
}

const void* World::get_raw(Entity e, ecs::ComponentId c) const {
    if (!has(e, c)) return nullptr;
    const Location loc = locations_[e.index];
    return tables_[loc.table]->cell(c, loc.row);
}

void World::set_raw(Entity e, ecs::ComponentId c, const void* value) {
    if (!alive(e)) return;
    MCE_ASSERT_MSG(c < components_.size(), "set_raw: unregistered component id");
    if (has(e, c)) {
        const Location loc = locations_[e.index];
        if (value && components_[c].size) {
            std::memcpy(tables_[loc.table]->cell(c, loc.row), value, components_[c].size);
            tables_[loc.table]->changed_ticks(c)[loc.row] = write_tick();
        }
        return;
    }
    assert_not_in_phase("set_raw (structural)");
    add_component_raw(e, c, value);
}

void World::remove_raw(Entity e, ecs::ComponentId c) {
    assert_not_in_phase("remove_raw");
    if (!alive(e) || c >= components_.size()) return;
    remove_component_raw(e, c);
}

// ---------------------------------------------------------------- systems & phases

void World::add_system(Phase phase, const char* name, SystemFn fn, const char* after) {
    assert_not_in_phase("add_system");
    Array<SystemEntry>& list = systems_[static_cast<usize>(phase)];
    if (after) {
        bool found = false;
        for (const SystemEntry& s : list) found = found || StringView(s.name) == StringView(after);
        MCE_ASSERT_MSG(found, "system ordering: `after` target must be registered earlier in the same phase");
    }
    const auto id = static_cast<u16>(system_names_all_.size());
    system_names_all_.push_back(name);
    SystemEntry entry;
    entry.name = name;
    entry.fn = fn;
    entry.id = id;
    entry.after = after;
    list.push_back(std::move(entry));
    system_name_cache_[static_cast<usize>(phase)].push_back(name);
    batches_dirty_[static_cast<usize>(phase)] = true;
}

void World::set_system_access(Phase phase, const char* name, const SystemAccess& access) {
    assert_not_in_phase("set_system_access");
    SystemEntry* found = nullptr;
    for (SystemEntry& s : systems_[static_cast<usize>(phase)])
        if (StringView(s.name) == StringView(name)) found = &s;
    MCE_ASSERT_MSG(found != nullptr, "set_system_access: no such system in that phase");
    if (!found) return;
    SystemEntry& s = *found;
    s.declared = true;
    s.uses_commands = access.commands_;
    s.reads.clear();
    s.writes.clear();
    for (const ecs::ComponentInfo& c : access.writes_) s.writes.set(register_component_info(c));
    for (const ecs::ComponentInfo& c : access.reads_) {
        const ecs::ComponentId id = register_component_info(c);
        if (!s.writes.test(id)) s.reads.set(id);
    }
    s.resources.clear();
    for (u64 h : access.resources_) s.resources.push_back(h);
    s.events.clear();
    for (u64 h : access.events_) s.events.push_back(h);
    access_checks_ = true;
    batches_dirty_[static_cast<usize>(phase)] = true;
}

void World::set_parallel(u32 workers) {
    assert_not_in_phase("set_parallel");
    pool_.reset();
    if (workers > 0) pool_ = mem::UniquePtr<ecs::detail::SystemPool>(alloc_, mem::make<ecs::detail::SystemPool>(alloc_, workers));
}

u32 World::parallel_workers() const { return pool_ ? pool_->workers() : 0; }

bool World::conflicts(const SystemEntry& a, const SystemEntry& b) {
    // Commands::spawn reserves entity handles immediately (generations_/locations_ may grow), so a
    // recording system cannot overlap anything that looks entities up: it runs alone.
    if (!a.declared || !b.declared || a.uses_commands || b.uses_commands) return true;
    if (a.writes.intersects(b.writes) || a.writes.intersects(b.reads) || b.writes.intersects(a.reads)) return true;
    for (u64 r : a.resources)
        if (b.resources.contains(r)) return true;
    for (u64 ev : a.events)
        if (b.events.contains(ev)) return true;
    return false;
}

// Contiguous batches in registration order: a system joins the open batch only when it is
// declared, conflicts with no member, and its `after` target is not a member. Every conflicting
// pair therefore keeps its serial order; only commuting systems overlap.
void World::build_batches(Phase phase) {
    const usize p = static_cast<usize>(phase);
    const Array<SystemEntry>& list = systems_[p];
    Array<u16>& starts = batch_starts_[p];
    starts.clear();
    usize open = 0;
    for (usize i = 0; i < list.size(); ++i) {
        bool joins = i > 0 && list[i].declared && list[open].declared;
        for (usize j = open; joins && j < i; ++j) {
            if (conflicts(list[i], list[j])) joins = false;
            if (list[i].after && StringView(list[i].after) == StringView(list[j].name)) joins = false;
        }
        if (!joins) {
            open = i;
            starts.push_back(static_cast<u16>(i));
        }
    }
    batches_dirty_[p] = false;
}

void World::parallel_batches(Phase phase, Array<u16>& out) {
    if (batches_dirty_[static_cast<usize>(phase)]) build_batches(phase);
    out.clear();
    for (u16 s : batch_starts_[static_cast<usize>(phase)]) out.push_back(s);
}

Span<const char* const> World::system_names(Phase phase) const {
    return system_name_cache_[static_cast<usize>(phase)].span();
}

void World::set_profiling(bool on) {
    if (on && !profiling_) reset_system_timings();
    profiling_ = on;
}

void World::reset_system_timings() {
    for (Array<SystemEntry>& list : systems_)
        for (SystemEntry& e : list) {
            e.last_ms = 0.0;
            e.total_ms = 0.0;
            e.runs = 0;
        }
}

void World::system_timings(Array<SystemTiming>& out) const {
    out.clear();
    for (usize p = 0; p < kPhaseCount; ++p)
        for (const SystemEntry& e : systems_[p]) {
            SystemTiming t;
            t.name = e.name;
            t.phase = static_cast<Phase>(p);
            t.last_ms = e.last_ms;
            t.total_ms = e.total_ms;
            t.runs = e.runs;
            out.push_back(t);
        }
}

void World::run_system(SystemEntry& s, Frame& frame) {
    if (profiling_) {
        app::IClock& clock = app::steady_clock();
        const f64 t0 = clock.now_seconds();
        s.fn(frame);
        const f64 ms = (clock.now_seconds() - t0) * 1000.0;
        s.last_ms = ms;
        s.total_ms += ms;
        ++s.runs;
    } else {
        s.fn(frame);
    }
}

namespace {
struct BatchJob {
    World* world;
    Frame* frame;
    u16 phase;
    u16 first;
    u32 first_stamp;
    void (*run)(BatchJob& b, u32 index);
};
}  // namespace

void World::run_phase(Phase phase, Frame& frame) {
    MCE_ASSERT_MSG(!in_phase_, "phases do not nest");
    frame.phase = phase;
    in_phase_ = true;
    const usize p = static_cast<usize>(phase);
    Array<SystemEntry>& list = systems_[p];
    if (pool_ && batches_dirty_[p]) build_batches(phase);
    const Array<u16>* starts = pool_ ? &batch_starts_[p] : nullptr;
    for (usize i = 0; i < list.size();) {
        usize end = i + 1;
        if (starts) {
            // starts holds every batch's first index; find the next one after i.
            end = list.size();
            for (u16 st : *starts)
                if (st > i) {
                    end = st;
                    break;
                }
        }
        if (end - i == 1) {
            run_.system = list[i].id;
            run_.phase = static_cast<u16>(phase);
            run_.slot = static_cast<u16>(i);
            // The system's writes carry its run tick; its reader tick is its previous run.
            run_.stamp = ++change_tick_;
            run_.stamping = true;
            run_system(list[i], frame);
            list[i].last_run = run_.stamp;
        } else {
            // Same ticks as the serial order: member k of the batch is stamped first_stamp + k.
            BatchJob job{this, &frame, static_cast<u16>(phase), static_cast<u16>(i), change_tick_ + 1, nullptr};
            change_tick_ += static_cast<u32>(end - i);
            job.run = [](BatchJob& b, u32 k) {
                World& w = *b.world;
                ecs::detail::RunCtx ctx;
                ctx.system = w.systems_[b.phase][b.first + k].id;
                ctx.phase = b.phase;
                ctx.slot = static_cast<u16>(b.first + k);
                ctx.stamp = b.first_stamp + k;
                ctx.stamping = true;
                const World* prev_world = t_world;
                ecs::detail::RunCtx* prev_ctx = t_ctx;
                t_world = &w;
                t_ctx = &ctx;
                SystemEntry& s = w.systems_[b.phase][b.first + k];
                w.run_system(s, *b.frame);
                s.last_run = ctx.stamp;
                t_world = prev_world;
                t_ctx = prev_ctx;
            };
            batch_running_ = true;
            pool_->run(static_cast<u32>(end - i), [](void* c, u32 k) { auto& b = *static_cast<BatchJob*>(c); b.run(b, k); }, &job);
            batch_running_ = false;
        }
        i = end;
    }
    run_.stamping = false;
    run_.system = 0xFFFF;
    in_phase_ = false;
    sync(phase);
}

void World::tick(f64 dt, u64 tick_index, f32 time_scale) {
    tick_ = tick_index;
    Frame f(*this);
    f.tick = tick_index;
    f.dt = static_cast<f32>(dt);
    f.time_scale = time_scale;
    if (!started_) {
        started_ = true;
        run_phase(Phase::Startup, f);
    }
    run_phase(Phase::FixedUpdate_Pre, f);
    run_phase(Phase::FixedUpdate_Physics, f);
    run_phase(Phase::FixedUpdate_Post, f);
}

void World::render(f32 alpha) {
    Frame f(*this);
    f.tick = tick_;
    f.alpha = alpha;
    run_phase(Phase::Render_Pre, f);
    run_phase(Phase::Render_Submit, f);
}

void World::sync(Phase phase) {
    if (!commands_.empty()) {
        run_.stamp = ++change_tick_;
        run_.stamping = true;
        apply_commands();
        run_.stamping = false;
    }
    if (u32(change_tick_ - last_tick_clamp_) >= (1u << 29)) clamp_change_ticks();
    for (ecs::detail::EventQueueBase* q : events_by_phase_[static_cast<usize>(phase)]) q->swap();
}

void World::apply_commands() {
    // Hooks may enqueue more commands; apply until quiescent (bounded to catch hook ping-pong).
    for (int round = 0; round < 16 && !commands_.empty(); ++round) {
        Array<u8> bytes(std::move(commands_.bytes_));
        commands_.bytes_ = Array<u8>(alloc_);
        commands_.count_ = 0;

        using Op = Commands::Op;
        using Header = Commands::Header;
        auto for_each = [&](auto&& fn) {
            usize off = 0;
            while (off < bytes.size()) {
                Header h;
                std::memcpy(&h, bytes.data() + off, sizeof(h));
                const u8* payload = bytes.data() + off + sizeof(Header);
                fn(h, payload);
                off += sizeof(Header) + h.payload;
            }
        };

        // Pass 1: spawns.
        for_each([&](const Header& h, const u8* p) {
            if (h.op == Op::Spawn) {
                if (!alive(h.entity)) return;
                ecs::ComponentMask mask;
                for (usize off = 0; off < h.payload;) {
                    ecs::ComponentId c;
                    u32 size;
                    std::memcpy(&c, p + off, sizeof(c));
                    std::memcpy(&size, p + off + 4, sizeof(size));
                    mask.set(c);
                    off += 8 + align_up(size, 8);
                }
                place_entity(h.entity, table_for(mask).id());
                const Location loc = locations_[h.entity.index];
                for (usize off = 0; off < h.payload;) {
                    ecs::ComponentId c;
                    u32 size;
                    std::memcpy(&c, p + off, sizeof(c));
                    std::memcpy(&size, p + off + 4, sizeof(size));
                    if (size) std::memcpy(tables_[loc.table]->cell(c, loc.row), p + off + 8, size);
                    off += 8 + align_up(size, 8);
                }
                for (ecs::ComponentId c : tables_[loc.table]->ids())
                    if (hooks_[c].on_add) hooks_[c].on_add(*this, h.entity);
            } else if (h.op == Op::SpawnN) {
                u32 n;
                std::memcpy(&n, p, sizeof(n));
                const u32 size = components_[h.comp].size;
                const usize value_bytes = align_up(size, 8);
                ecs::ComponentMask mask;
                mask.set(h.comp);
                ecs::detail::Table& t = table_for(mask);
                const u32 tid = t.id();
                const u8* es = p + 8 + value_bytes;
                for (u32 i = 0; i < n; ++i) {
                    Entity e;
                    std::memcpy(&e, es + usize(i) * sizeof(Entity), sizeof(e));
                    if (!alive(e)) continue;
                    place_entity(e, tid);
                    if (size) std::memcpy(tables_[tid]->cell(h.comp, locations_[e.index].row), p + 8, size);
                    if (hooks_[h.comp].on_add) hooks_[h.comp].on_add(*this, e);
                }
            }
        });

        // Pass 2: add / remove / set, in recorded order.
        for_each([&](const Header& h, const u8* p) {
            if (!alive(h.entity)) return;
            switch (h.op) {
                case Op::Add:
                case Op::Set: add_component_raw(h.entity, h.comp, components_[h.comp].size ? p : nullptr); break;
                case Op::Remove: remove_component_raw(h.entity, h.comp); break;
                default: break;
            }
        });

        // Pass 3: destroys.
        for_each([&](const Header& h, const u8*) {
            if (h.op == Op::Destroy) destroy_now(h.entity);
        });
    }
    MCE_ASSERT_MSG(commands_.empty(), "hooks keep enqueueing commands (16 rounds)");
}

// ---------------------------------------------------------------- change detection

ecs::detail::RunCtx& World::ctx() { return t_world == this ? *t_ctx : run_; }
const ecs::detail::RunCtx& World::ctx() const { return t_world == this ? *t_ctx : run_; }

u32 World::write_tick() {
    const ecs::detail::RunCtx& c = ctx();
    return c.stamping ? c.stamp : ++change_tick_;
}

const World::SystemEntry* World::current_system_entry() const {
    const ecs::detail::RunCtx& c = ctx();
    if (c.system == 0xFFFF) return nullptr;
    return &systems_[c.phase][c.slot];
}

void World::check_component_access(const ecs::ComponentMask& reads, const ecs::ComponentMask& writes) const {
    const SystemEntry* s = current_system_entry();
    if (!s || !s->declared) return;
    for (usize i = 0; i < ecs::kMaxComponents; ++i) {
        if (writes.test(i)) MCE_ASSERT_MSG(s->writes.test(i), "system writes a component its SystemAccess does not declare");
        if (reads.test(i))
            MCE_ASSERT_MSG(s->writes.test(i) || s->reads.test(i), "system reads a component its SystemAccess does not declare");
    }
}

void World::check_component_access(ecs::ComponentId c, bool write) const {
    ecs::ComponentMask r, w;
    (write ? w : r).set(c);
    check_component_access(r, w);
}

void World::check_resource_access(u64 type_hash) const {
    const SystemEntry* s = current_system_entry();
    if (!s || !s->declared) return;
    MCE_ASSERT_MSG(s->resources.contains(type_hash), "system uses a resource its SystemAccess does not declare");
    (void)type_hash;
}

void World::check_event_access(u64 type_hash) const {
    const SystemEntry* s = current_system_entry();
    if (!s || !s->declared) return;
    MCE_ASSERT_MSG(s->events.contains(type_hash), "system uses an event its SystemAccess does not declare");
    (void)type_hash;
}

void World::check_commands_access() const {
    const SystemEntry* s = current_system_entry();
    if (!s || !s->declared) return;
    MCE_ASSERT_MSG(s->uses_commands, "system records Commands but its SystemAccess does not declare commands()");
}

u32 World::reader_tick() const {
    const SystemEntry* s = current_system_entry();
    return s ? s->last_run : 0;
}

void World::touch(Entity e, ecs::ComponentId c) {
    if (!get_raw(e, c)) return;
    const Location loc = locations_[e.index];
    tables_[loc.table]->changed_ticks(c)[loc.row] = write_tick();
}

bool World::is_changed(Entity e, ecs::ComponentId c, u32 since) const {
    if (!get_raw(e, c)) return false;
    const Location loc = locations_[e.index];
    return ecs::detail::tick_newer(tables_[loc.table]->changed_ticks(c)[loc.row], since, change_tick_);
}

bool World::is_added(Entity e, ecs::ComponentId c, u32 since) const {
    if (!get_raw(e, c)) return false;
    const Location loc = locations_[e.index];
    return ecs::detail::tick_newer(tables_[loc.table]->added_ticks(c)[loc.row], since, change_tick_);
}

// Keeps every stored age below kMaxChangeAge so the wrapping comparison stays exact (Bevy's
// check_change_ticks). Runs every 2^29 ticks from sync; ticks older than the window read as
// "very old", which is all a reader can ask of them.
void World::clamp_change_ticks() {
    const u32 now = change_tick_;
    auto clamp = [now](u32& t) {
        if (u32(now - t) > ecs::detail::kMaxChangeAge) t = now - ecs::detail::kMaxChangeAge;
    };
    for (auto& tp : tables_) {
        const u32 rows = tp->size();
        for (ecs::detail::Column& c : tp->columns_mut()) {
            for (u32 r = 0; r < rows; ++r) {
                clamp(c.added[r]);
                clamp(c.changed[r]);
            }
        }
    }
    for (auto& list : systems_)
        for (SystemEntry& s : list) clamp(s.last_run);
    last_tick_clamp_ = now;
}

void World::debug_set_change_tick(u32 tick) {
    // As if the ticks in between had elapsed: advance in clamp-sized steps.
    u32 remaining = tick - change_tick_;
    while (remaining) {
        const u32 step = remaining < (1u << 29) ? remaining : (1u << 29);
        change_tick_ += step;
        remaining -= step;
        clamp_change_ticks();
    }
}

ecs::ComponentId World::find_component_by_refl(u64 refl_id) const {
    if (refl_id == 0) return ecs::kInvalidComponent;
    for (usize i = 0; i < components_.size(); ++i)
        if (components_[i].refl_id == refl_id) return static_cast<ecs::ComponentId>(i);
    return ecs::kInvalidComponent;
}

// ---------------------------------------------------------------- events

void World::note_event_access(ecs::detail::EventQueueBase& q, bool producer) {
    const u16 system = ctx().system;
    if (system == 0xFFFF) return;
    auto& set = producer ? q.producers : q.consumers;
    if (!set.contains(system)) set.push_back(system);
}

void World::event_registry(Array<EventInfo>& out) const {
    out.clear();
    for (const ecs::detail::EventQueueBase* q : events_ordered_) {
        EventInfo info;
        info.name = q->name;
        info.producer_phase = q->producer_phase;
        info.total_pushed = q->total_pushed;
        info.readable = q->front_size();
        for (u16 s : q->producers) info.producers.push_back(system_names_all_[s]);
        for (u16 s : q->consumers) info.consumers.push_back(system_names_all_[s]);
        out.push_back(std::move(info));
    }
}

// ---------------------------------------------------------------- determinism & metrics

u64 World::state_hash(u64 seed) const {
    Hasher64 h(seed);
    h.u32v(alive_count_);
    for (const auto& tp : tables_) {
        const ecs::detail::Table& t = *tp;
        if (t.size() == 0) continue;
        h.u32v(t.size());
        for (ecs::ComponentId c : t.ids()) h.u64v(components_[c].type_hash);
        h.bytes(t.entities().data(), t.size() * sizeof(Entity));
        for (const ecs::detail::Column& col : t.columns()) h.bytes(col.data, usize(col.size) * t.size());
    }
    return h.value();
}

WorldMetrics World::metrics() const {
    WorldMetrics m;
    m.entities = alive_count_;
    m.tables = static_cast<u32>(tables_.size());
    u64 rows = 0, non_empty = 0;
    for (const auto& t : tables_) {
        if (t->size()) {
            rows += t->size();
            ++non_empty;
        }
    }
    m.avg_table_occupancy_x100 = non_empty ? static_cast<u32>(rows * 100 / non_empty) : 0;
    m.pending_commands = commands_.pending();
    m.components = static_cast<u32>(components_.size());
    m.structure_version = structure_version_;
    return m;
}

}  // namespace mce
