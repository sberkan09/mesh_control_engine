#pragma once
// mce_ecs: archetype tables, generational entities, queries, phases + sync points, deferred
// Commands, double-buffered typed events, resources, state_hash (ecs-gameplay.md §2–§3).
//
// Safety property: nothing mutates table structure while a phase runs. Systems change structure
// only through Commands, applied at the sync point that ends each phase, in recorded order.
//
// Change detection (v0.5, ecs-gameplay.md §2.7/§3.6, FR-ENT-040): every data column carries two
// parallel u32 tick columns (added, changed). Writes stamp the current write tick: the running
// system's run tick inside a phase, the sync tick while commands apply, a fresh tick for direct
// calls outside a phase. Stamping writes: set/add, Commands, try_get_mut, touch, and mutable
// (non-const) query items for every row visited. A pointer kept and written later is not seen —
// call touch<T>(). Tags have no column and therefore no ticks.

#include <cstring>
#include <tuple>
#include <type_traits>
#include <utility>

#include "mce/containers/array.h"
#include "mce/containers/flat_map.h"
#include "mce/containers/hash_map.h"
#include "mce/ecs/entity.h"
#include "mce/memory/allocator.h"

namespace mce {

template <>
struct Hash<ecs::ComponentMask> {
    u64 operator()(const ecs::ComponentMask& m) const {
        u64 h = 0;
        for (usize i = 0; i < ecs::ComponentMask::kWords; ++i) h = hash_combine(h, m.word_at(i));
        return h;
    }
};

class World;
class Frame;

// The complete phase list (adding a phase is a design event).
enum class Phase : u8 {
    Startup,              // once, after scene load, deterministic order
    FixedUpdate_Pre,      // input apply, timers, gameplay pre
    FixedUpdate_Physics,  // owned by physics
    FixedUpdate_Post,     // transform propagation, triggers, gameplay post
    Render_Pre,           // per render frame: extract, interpolate ("RenderExtract" group, fix F9)
    Render_Submit,        // renderer only
    Editor,               // editor-only systems
    Count
};
inline constexpr usize kPhaseCount = static_cast<usize>(Phase::Count);
const char* phase_name(Phase p);

using SystemFn = void (*)(Frame& f);

namespace ecs::detail {

struct Column {
    ComponentId id = kInvalidComponent;
    u32 size = 0;
    u32 align = 0;
    u8* data = nullptr;
    u32* added = nullptr;    // per-row tick the component was added (change detection)
    u32* changed = nullptr;  // per-row tick of the last stamped write
};

// Wrapping tick comparison (Bevy-style): `tick` is newer than `since` when it is younger relative
// to `now`. Correct while every stored age stays below 2^32 — World::clamp_change_ticks keeps
// them below kMaxChangeAge.
inline constexpr u32 kMaxChangeAge = 3u << 30;
constexpr bool tick_newer(u32 tick, u32 since, u32 now) { return u32(now - tick) < u32(now - since); }

// One archetype: every entity with exactly this component set, stored column-wise.
class Table {
public:
    Table(mem::IAllocator& alloc, u32 id, const ComponentMask& mask, Span<const ComponentInfo> infos);
    ~Table();
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;

    u32 id() const { return id_; }
    const ComponentMask& mask() const { return mask_; }
    u32 size() const { return static_cast<u32>(entities_.size()); }
    Span<const Entity> entities() const { return entities_.span(); }
    Span<const Column> columns() const { return columns_.span(); }
    Span<const ComponentId> ids() const { return ids_.span(); }

    // Column data pointer for a component, or nullptr if absent (tags have no column).
    u8* column(ComponentId c) const {
        const u8 i = col_of_[c];
        return i == kNoColumn ? nullptr : columns_[i].data;
    }
    u8* cell(ComponentId c, u32 row) const {
        const u8 i = col_of_[c];
        return i == kNoColumn ? nullptr : columns_[i].data + usize(row) * columns_[i].size;
    }
    // Tick columns for a data component, or nullptr if absent / a tag.
    u32* added_ticks(ComponentId c) const {
        const u8 i = col_of_[c];
        return i == kNoColumn ? nullptr : columns_[i].added;
    }
    u32* changed_ticks(ComponentId c) const {
        const u8 i = col_of_[c];
        return i == kNoColumn ? nullptr : columns_[i].changed;
    }
    Span<Column> columns_mut() { return columns_.span(); }

    // Appends a zero-initialized row (added/changed ticks = `tick`). Returns its index.
    u32 push_row(Entity e, u32 tick = 0);
    // Appends `n` zero-initialized rows.
    u32 push_rows(u32 n, const Entity* es, u32 tick = 0);
    // Removes `row` by moving the last row into it. Returns the entity that moved (null if none).
    Entity swap_remove(u32 row);

    FlatMap<ComponentId, u32>& add_edges() { return add_edges_; }
    FlatMap<ComponentId, u32>& remove_edges() { return remove_edges_; }

private:
    static constexpr u8 kNoColumn = 0xFF;
    void reserve(u32 capacity);

    mem::IAllocator& alloc_;
    u32 id_;
    ComponentMask mask_;
    SmallVector<ComponentId, 8> ids_;
    Array<Column> columns_;
    Array<Entity> entities_;
    u32 capacity_ = 0;
    u8 col_of_[kMaxComponents];
    FlatMap<ComponentId, u32> add_edges_;
    FlatMap<ComponentId, u32> remove_edges_;
};

// Type-erased event queue (double-buffered; swapped at its producer phase's sync point).
class EventQueueBase {
public:
    virtual ~EventQueueBase() = default;
    virtual void swap() = 0;
    virtual usize back_size() const = 0;
    virtual usize front_size() const = 0;

    StringView name;
    Phase producer_phase = Phase::FixedUpdate_Pre;
    u64 total_pushed = 0;
    SmallVector<u16, 4> producers;  // system indices that pushed
    SmallVector<u16, 4> consumers;  // system indices that read
};

template <class T>
class EventQueue final : public EventQueueBase {
public:
    explicit EventQueue(mem::IAllocator& a) : front_(a), back_(a) {}
    void push(const T& ev) {
        back_.push_back(ev);
        ++total_pushed;
    }
    Span<const T> read() const { return front_.span(); }
    void swap() override {
        std::swap(front_, back_);
        back_.clear();
    }
    usize back_size() const override { return back_.size(); }
    usize front_size() const override { return front_.size(); }

private:
    Array<T> front_;
    Array<T> back_;
};

}  // namespace ecs::detail

// Deferred structural changes. Records into a byte stream; applied at the next sync point in
// fixed passes: spawns, then add/remove/set in recorded order, then destroys.
class Commands {
public:
    explicit Commands(World& w);

    // The handle is valid (alive) immediately; its components appear at the sync point.
    Entity spawn();
    template <class... Ts>
    Entity spawn(const Ts&... components);
    // Spawns n entities sharing one initial component value. Handles are appended to `out`.
    template <class T>
    void spawn_n(u32 n, const T& initial, Array<Entity>* out = nullptr);
    void destroy(Entity e);
    // Type-erased spawn of an already reserved handle (World::reserve_entity): prefab
    // instantiation inside a phase. `components` data is copied at record time.
    void spawn_raw(Entity reserved, Span<const ecs::RawComponent> components);
    template <class T>
    void add(Entity e, const T& value = T{});
    template <class T>
    void remove(Entity e);
    template <class T>
    void set(Entity e, const T& value);

    u32 pending() const { return count_; }
    bool empty() const { return count_ == 0; }

private:
    friend class World;
    enum class Op : u8 { Spawn, SpawnN, Destroy, Add, Remove, Set };
    struct Header {
        Op op;
        u8 pad;
        ecs::ComponentId comp;
        u32 payload;  // payload bytes following the header (8-byte padded)
        Entity entity;
    };
    static_assert(sizeof(Header) == 16);

    void* record(Op op, Entity e, ecs::ComponentId comp, usize payload_bytes);
    void clear() {
        bytes_.clear();
        count_ = 0;
    }

    World& world_;
    Array<u8> bytes_;
    u32 count_ = 0;
};

// Read/write access to typed events from a system.
class EventAccess {
public:
    explicit EventAccess(World& w) : world_(w) {}
    template <class T>
    void push(const T& ev);
    template <class T>
    Span<const T> read();

private:
    World& world_;
};

template <class... Cs>
class Query;

// The only context a system receives (fix F3: the per-tick context of record).
class Frame {
public:
    u64 tick = 0;          // fixed-step counter
    f32 dt = 0;            // exactly 1/sim_hz — never scaled (FR-RUN-050)
    f32 alpha = 0;         // render interpolation factor; valid in Render_Pre
    f32 time_scale = 1.0f;
    Phase phase = Phase::FixedUpdate_Pre;

    explicit Frame(World& w) : world_(&w) {}

    World& world() { return *world_; }
    Commands& commands();
    EventAccess events() { return EventAccess(*world_); }
    template <class T>
    T& resource();
    template <class T>
    T* try_resource();
    template <class... Cs>
    Query<Cs...> query();
    // Change detection relative to the running system's previous run (FR-ENT-040).
    template <class T>
    bool is_added(Entity e) const;
    template <class T>
    bool is_changed(Entity e) const;

private:
    World* world_;
};

namespace ecs::detail {
template <class C>
using component_t = std::remove_cvref_t<C>;
template <class C>
using column_ptr_t = std::conditional_t<std::is_const_v<std::remove_reference_t<C>>, const component_t<C>*, component_t<C>*>;
}  // namespace ecs::detail

// Frame-scoped view over every table containing Cs (plus with<>, minus without<>).
// Iteration order: tables in creation order, rows in row order — deterministic.
template <class... Cs>
class Query {
public:
    explicit Query(World& w);

    template <class... Ts>
    Query& with();
    template <class... Ts>
    Query& without();
    // Change filters (v0.5): only rows whose T was added / written after the reader tick — the
    // running system's previous run, or since() when given. T must be a data component.
    template <class... Ts>
    Query& added();
    template <class... Ts>
    Query& changed();
    // Overrides the reader tick (outside a system the default reader sees every row as new).
    Query& since(u32 tick) {
        since_ = tick;
        has_since_ = true;
        return *this;
    }

    // fn(Entity, Cs&...) or fn(Cs&...)
    template <class Fn>
    void each(Fn&& fn);
    usize count();

private:
    struct TickFilter {
        ecs::ComponentId id;
        bool added;  // false = changed
    };
    bool row_passes(const ecs::detail::Table& t, u32 row, u32 now) const;

    World& world_;
    ecs::ComponentMask required_;
    ecs::ComponentMask excluded_;
    ecs::ComponentId ids_[sizeof...(Cs) > 0 ? sizeof...(Cs) : 1];
    SmallVector<TickFilter, 2> tick_filters_;
    u32 since_ = 0;
    bool has_since_ = false;
};

struct WorldMetrics {
    u32 entities = 0;
    u32 tables = 0;
    u32 avg_table_occupancy_x100 = 0;  // average rows per non-empty table, x100
    u32 pending_commands = 0;
    u32 components = 0;
    u64 structure_version = 0;
};

struct EventInfo {
    StringView name;
    Phase producer_phase;
    u64 total_pushed;
    usize readable;  // events visible to readers right now
    SmallVector<StringView, 4> producers;
    SmallVector<StringView, 4> consumers;
};

class World {
public:
    explicit World(mem::IAllocator& alloc = mem::heap());
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // ---- components ------------------------------------------------------
    template <class T>
    ecs::ComponentId component_id() {
        return register_component_info(ecs::component_info<T>());
    }
    template <class... Ts>
    void register_components() {
        (component_id<Ts>(), ...);
    }
    const ecs::ComponentInfo& component(ecs::ComponentId id) const { return components_[id]; }
    usize component_count() const { return components_.size(); }

    using HookFn = void (*)(World& w, Entity e);
    template <class T>
    void set_hooks(HookFn on_add, HookFn on_remove) {
        const ecs::ComponentId c = component_id<T>();
        hooks_[c].on_add = on_add;
        hooks_[c].on_remove = on_remove;
    }
    // Runs for every entity about to be destroyed, before its component on_remove hooks and before
    // its row goes (the hierarchy uses it to destroy children first). May destroy other entities.
    void add_destroy_hook(HookFn fn) { destroy_hooks_.push_back(fn); }
    // The registered component whose mce_refl type name hashes to `refl_id`, or kInvalidComponent.
    ecs::ComponentId find_component_by_refl(u64 refl_id) const;

    // ---- entities (immediate; structural calls are illegal while a phase runs) -----
    Entity spawn();
    template <class... Ts>
    Entity spawn_with(const Ts&... components);
    // Places a reserved handle (reserve_entity) with type-erased components now; outside phases.
    void spawn_raw(Entity reserved, Span<const ecs::RawComponent> components);
    // Type-erased add/overwrite of one component (save load, tools); outside phases when
    // structural. `value` null = zero bytes.
    void add_raw(Entity e, ecs::ComponentId c, const void* value);
    void destroy(Entity e);
    bool alive(Entity e) const;
    template <class T>
    bool has(Entity e) const;
    template <class T>
    const T* try_get(Entity e) const;
    template <class T>
    T* try_get_mut(Entity e);
    // Data write; adds the component (structural) when missing.
    template <class T>
    void set(Entity e, const T& value);
    template <class T>
    void add(Entity e, const T& value = T{}) {
        set<T>(e, value);
    }
    template <class T>
    void remove(Entity e);
    u32 entity_count() const { return alive_count_; }

    // ---- change detection (v0.5, FR-ENT-040) ---------------------------------
    // The tick counter advances once per system run, per sync point and per direct (out-of-phase)
    // write. `since` is a tick previously read from change_tick() or a reader tick.
    u32 change_tick() const { return change_tick_; }
    // Tick a query/Frame helper compares against: the running system's previous run tick, or 0
    // outside a system.
    u32 reader_tick() const;
    bool in_system() const { return current_system_ != 0xFFFF; }
    template <class T>
    bool is_added(Entity e, u32 since) const;
    template <class T>
    bool is_changed(Entity e, u32 since) const;
    // Marks T on e as changed now (writes through a retained pointer are otherwise invisible).
    template <class T>
    void touch(Entity e);
    // Type-erased variants (prefab instantiation, inspector).
    void touch(Entity e, ecs::ComponentId c);
    bool is_changed(Entity e, ecs::ComponentId c, u32 since) const;
    bool is_added(Entity e, ecs::ComponentId c, u32 since) const;
    // The tick a write right now would be stamped with (advances it outside a phase).
    u32 write_tick();
    // Test hook: advance the counter to `tick` as if the ticks in between had elapsed (wrapping
    // forward; stored ticks are clamped exactly as periodic sync clamping would).
    void debug_set_change_tick(u32 tick);
    // Raw row pointer for component `c` (nullptr when absent or a tag). Does not stamp.
    void* get_raw(Entity e, ecs::ComponentId c) const;

    // ---- systems & phases -------------------------------------------------
    // Registration order within a phase IS execution order. `after` (optional) names a system
    // that must already be registered in the same phase — asserted, never reordered.
    void add_system(Phase phase, const char* name, SystemFn fn, const char* after = nullptr);
    Span<const char* const> system_names(Phase phase) const;
    void install_module(void (*fn)(World&)) { fn(*this); }

    // Runs one phase, then its sync point.
    void run_phase(Phase phase, Frame& frame);
    // Startup phase (once) + the three fixed phases.
    void tick(f64 dt, u64 tick_index, f32 time_scale = 1.0f);
    void render(f32 alpha);
    // Applies pending commands and swaps the given phase's event queues.
    void sync(Phase phase);
    bool in_phase() const { return in_phase_; }
    u64 current_tick() const { return tick_; }

    Commands& commands() { return commands_; }

    // ---- events -------------------------------------------------------------
    template <class T>
    void register_event(Phase producer_phase);
    template <class T>
    ecs::detail::EventQueue<T>* event_queue();
    void event_registry(Array<EventInfo>& out) const;

    // ---- resources (singletons; not entities) ---------------------------------
    template <class T>
    T& set_resource(T value);
    template <class T>
    T* try_resource();
    template <class T>
    T& resource() {
        T* r = try_resource<T>();
        MCE_ASSERT_MSG(r != nullptr, "missing world resource");
        return *r;
    }

    // ---- queries & determinism ------------------------------------------------------
    template <class... Cs>
    Query<Cs...> query() {
        return Query<Cs...>(*this);
    }
    // Order-total hash of every table: component identities, entities, and column bytes.
    u64 state_hash(u64 seed = kFnv64Offset) const;
    WorldMetrics metrics() const;

    // ---- internals used by Query/Commands/EventAccess (mce::ecs::detail level) ----
    Span<const mem::UniquePtr<ecs::detail::Table>> tables() const { return tables_.span(); }
    ecs::ComponentId register_component_info(const ecs::ComponentInfo& info);
    Entity reserve_entity();
    void note_event_access(ecs::detail::EventQueueBase& q, bool producer);

private:
    friend class Commands;
    struct Location {
        u32 table = kNoTable;
        u32 row = 0;
    };
    struct Hooks {
        HookFn on_add = nullptr;
        HookFn on_remove = nullptr;
    };
    struct SystemEntry {
        const char* name;
        SystemFn fn;
        u16 id;  // global system index (event producer/consumer attribution)
        u32 last_run = 0;  // change tick of this system's previous run (0 = never ran)
    };
    struct Resource {
        void* ptr = nullptr;
        void (*destroy)(mem::IAllocator&, void*) = nullptr;
    };
    static constexpr u32 kNoTable = ~u32(0);       // reserved by Commands::spawn, not yet placed
    static constexpr u32 kEmptyTable = 0;          // table 0 = the empty archetype

    ecs::detail::Table& table_for(const ecs::ComponentMask& mask);
    u32 table_with(u32 from, ecs::ComponentId c, bool add);
    // Moves e to table `to`, copying shared columns. Returns the new row.
    u32 move_entity(Entity e, u32 to);
    void place_entity(Entity e, u32 table);
    void add_component_raw(Entity e, ecs::ComponentId c, const void* value);
    void remove_component_raw(Entity e, ecs::ComponentId c);
    void destroy_now(Entity e);
    void apply_commands();
    void assert_not_in_phase(const char* what) const;
    void clamp_change_ticks();
    const SystemEntry* current_system_entry() const;

    mem::IAllocator& alloc_;
    Array<ecs::ComponentInfo> components_;
    HashMap<u64, ecs::ComponentId> component_ids_;
    Array<Hooks> hooks_;

    Array<u32> generations_;
    Array<Location> locations_;
    Array<u32> free_list_;
    u32 alive_count_ = 0;

    Array<mem::UniquePtr<ecs::detail::Table>> tables_;
    HashMap<ecs::ComponentMask, u32> table_index_;
    u64 structure_version_ = 0;

    Array<SystemEntry> systems_[kPhaseCount];
    Array<const char*> system_name_cache_[kPhaseCount];
    Array<const char*> system_names_all_;  // index = global system id
    u16 current_system_ = 0xFFFF;
    u16 current_phase_ = 0;      // valid while current_system_ is
    u16 current_slot_ = 0;       // index into systems_[current_phase_]

    Array<HookFn> destroy_hooks_;
    u32 change_tick_ = 1;        // 0 is reserved for "never"
    u32 stamp_tick_ = 0;         // write tick while stamping_ (system run / sync)
    bool stamping_ = false;
    u32 last_tick_clamp_ = 1;

    HashMap<u64, mem::UniquePtr<ecs::detail::EventQueueBase>> events_;
    Array<ecs::detail::EventQueueBase*> events_by_phase_[kPhaseCount];
    Array<ecs::detail::EventQueueBase*> events_ordered_;

    HashMap<u64, Resource> resources_;

    Commands commands_;
    bool in_phase_ = false;
    bool started_ = false;
    u64 tick_ = 0;
};

// =====================================================================================
// Template implementations
// =====================================================================================

template <class... Ts>
Entity World::spawn_with(const Ts&... components) {
    assert_not_in_phase("spawn_with");
    ecs::ComponentMask mask;
    const ecs::ComponentId ids[] = {component_id<Ts>()...};
    for (ecs::ComponentId c : ids) mask.set(c);
    Entity e = reserve_entity();
    const u32 t = table_for(mask).id();
    place_entity(e, t);
    const Location loc = locations_[e.index];
    ecs::detail::Table& table = *tables_[loc.table];
    usize i = 0;
    (
        [&] {
            if constexpr (!std::is_empty_v<Ts>) std::memcpy(table.cell(ids[i], loc.row), &components, sizeof(Ts));
            ++i;
        }(),
        ...);
    for (ecs::ComponentId c : ids)
        if (hooks_[c].on_add) hooks_[c].on_add(*this, e);
    return e;
}

template <class T>
bool World::has(Entity e) const {
    if (!alive(e)) return false;
    const auto it = component_ids_.find(kTypeHash<T>);
    if (!it) return false;
    const Location loc = locations_[e.index];
    return loc.table != kNoTable && tables_[loc.table]->mask().test(*it);
}

template <class T>
const T* World::try_get(Entity e) const {
    static_assert(!std::is_empty_v<T>, "tags have no data; use has<T>()");
    if (!alive(e)) return nullptr;
    const auto it = component_ids_.find(kTypeHash<T>);
    if (!it) return nullptr;
    const Location loc = locations_[e.index];
    if (loc.table == kNoTable) return nullptr;
    return reinterpret_cast<const T*>(tables_[loc.table]->cell(*it, loc.row));
}

template <class T>
T* World::try_get_mut(Entity e) {
    T* p = const_cast<T*>(static_cast<const World*>(this)->try_get<T>(e));
    if (p) {
        const Location loc = locations_[e.index];
        tables_[loc.table]->changed_ticks(*component_ids_.find(kTypeHash<T>))[loc.row] = write_tick();
    }
    return p;
}

template <class T>
void World::set(Entity e, const T& value) {
    if (!alive(e)) return;
    const ecs::ComponentId c = component_id<T>();
    const Location loc = locations_[e.index];
    if (loc.table != kNoTable && tables_[loc.table]->mask().test(c)) {
        if constexpr (!std::is_empty_v<T>) {
            std::memcpy(tables_[loc.table]->cell(c, loc.row), &value, sizeof(T));
            tables_[loc.table]->changed_ticks(c)[loc.row] = write_tick();
        }
        return;
    }
    assert_not_in_phase("add (structural set)");
    add_component_raw(e, c, std::is_empty_v<T> ? nullptr : &value);
}

template <class T>
void World::remove(Entity e) {
    assert_not_in_phase("remove");
    if (!alive(e)) return;
    remove_component_raw(e, component_id<T>());
}

template <class T>
bool World::is_added(Entity e, u32 since) const {
    const ecs::ComponentId* c = component_ids_.find(kTypeHash<T>);
    return c && is_added(e, *c, since);
}

template <class T>
bool World::is_changed(Entity e, u32 since) const {
    const ecs::ComponentId* c = component_ids_.find(kTypeHash<T>);
    return c && is_changed(e, *c, since);
}

template <class T>
void World::touch(Entity e) {
    if (const ecs::ComponentId* c = component_ids_.find(kTypeHash<T>)) touch(e, *c);
}

template <class T>
void World::register_event(Phase producer_phase) {
    auto [slot, inserted] = events_.try_emplace(kTypeHash<T>);
    if (!inserted) return;
    auto* q = mem::make<ecs::detail::EventQueue<T>>(alloc_, alloc_);
    q->name = type_name<T>();
    q->producer_phase = producer_phase;
    *slot = mem::UniquePtr<ecs::detail::EventQueueBase>(alloc_, q);
    events_by_phase_[static_cast<usize>(producer_phase)].push_back(q);
    events_ordered_.push_back(q);
}

template <class T>
ecs::detail::EventQueue<T>* World::event_queue() {
    auto* slot = events_.find(kTypeHash<T>);
    return slot ? static_cast<ecs::detail::EventQueue<T>*>(slot->get()) : nullptr;
}

template <class T>
T& World::set_resource(T value) {
    if (T* existing = try_resource<T>()) {
        *existing = std::move(value);
        return *existing;
    }
    T* p = mem::make<T>(alloc_, std::move(value));
    Resource r;
    r.ptr = p;
    r.destroy = [](mem::IAllocator& a, void* ptr) { mem::destroy(a, static_cast<T*>(ptr)); };
    resources_.insert_or_assign(kTypeHash<T>, r);
    return *p;
}

template <class T>
T* World::try_resource() {
    Resource* r = resources_.find(kTypeHash<T>);
    return r ? static_cast<T*>(r->ptr) : nullptr;
}

// ---- Commands ------------------------------------------------------------------------

template <class... Ts>
Entity Commands::spawn(const Ts&... components) {
    const Entity e = world_.reserve_entity();
    usize bytes = 0;
    ((bytes += 8 + align_up(std::is_empty_v<Ts> ? 0 : sizeof(Ts), 8)), ...);
    u8* p = static_cast<u8*>(record(Op::Spawn, e, ecs::kInvalidComponent, bytes));
    (
        [&] {
            const ecs::ComponentId c = world_.component_id<Ts>();
            const u32 size = std::is_empty_v<Ts> ? 0u : static_cast<u32>(sizeof(Ts));
            std::memcpy(p, &c, sizeof(c));
            std::memcpy(p + 4, &size, sizeof(size));
            if constexpr (!std::is_empty_v<Ts>) std::memcpy(p + 8, &components, sizeof(Ts));
            p += 8 + align_up(size, 8);
        }(),
        ...);
    return e;
}

template <class T>
void Commands::spawn_n(u32 n, const T& initial, Array<Entity>* out) {
    const ecs::ComponentId c = world_.component_id<T>();
    const usize value_bytes = align_up(std::is_empty_v<T> ? 0 : sizeof(T), 8);
    u8* p = static_cast<u8*>(record(Op::SpawnN, Entity::null, c, 8 + value_bytes + usize(n) * sizeof(Entity)));
    std::memcpy(p, &n, sizeof(n));
    if constexpr (!std::is_empty_v<T>) std::memcpy(p + 8, &initial, sizeof(T));
    Entity* es = reinterpret_cast<Entity*>(p + 8 + value_bytes);
    for (u32 i = 0; i < n; ++i) {
        const Entity e = world_.reserve_entity();
        std::memcpy(es + i, &e, sizeof(e));
        if (out) out->push_back(e);
    }
}

template <class T>
void Commands::add(Entity e, const T& value) {
    const ecs::ComponentId c = world_.component_id<T>();
    const usize size = std::is_empty_v<T> ? 0 : sizeof(T);
    void* p = record(Op::Add, e, c, size);
    if constexpr (!std::is_empty_v<T>) std::memcpy(p, &value, sizeof(T));
}

template <class T>
void Commands::remove(Entity e) {
    record(Op::Remove, e, world_.component_id<T>(), 0);
}

template <class T>
void Commands::set(Entity e, const T& value) {
    static_assert(!std::is_empty_v<T>, "tags have no data; use add<T>()");
    void* p = record(Op::Set, e, world_.component_id<T>(), sizeof(T));
    std::memcpy(p, &value, sizeof(T));
}

// ---- events & frame ---------------------------------------------------------------------

template <class T>
void EventAccess::push(const T& ev) {
    ecs::detail::EventQueue<T>* q = world_.event_queue<T>();
    MCE_ASSERT_MSG(q != nullptr, "event type not registered (World::register_event)");
    if (!q) return;
    world_.note_event_access(*q, true);
    q->push(ev);
}

template <class T>
Span<const T> EventAccess::read() {
    ecs::detail::EventQueue<T>* q = world_.event_queue<T>();
    MCE_ASSERT_MSG(q != nullptr, "event type not registered (World::register_event)");
    if (!q) return {};
    world_.note_event_access(*q, false);
    return q->read();
}

inline Commands& Frame::commands() { return world_->commands(); }

template <class T>
T& Frame::resource() {
    return world_->resource<T>();
}
template <class T>
T* Frame::try_resource() {
    return world_->try_resource<T>();
}
template <class... Cs>
Query<Cs...> Frame::query() {
    return Query<Cs...>(*world_);
}
template <class T>
bool Frame::is_added(Entity e) const {
    return world_->is_added<T>(e, world_->reader_tick());
}
template <class T>
bool Frame::is_changed(Entity e) const {
    return world_->is_changed<T>(e, world_->reader_tick());
}

// ---- Query ---------------------------------------------------------------------------------

template <class... Cs>
Query<Cs...>::Query(World& w) : world_(w), since_(w.reader_tick()), has_since_(w.in_system()) {
    static_assert(((!std::is_empty_v<ecs::detail::component_t<Cs>>) && ...), "tags go in with<>(), not the item list");
    usize i = 0;
    ((ids_[i++] = w.component_id<ecs::detail::component_t<Cs>>()), ...);
    for (usize k = 0; k < sizeof...(Cs); ++k) required_.set(ids_[k]);
}

template <class... Cs>
template <class... Ts>
Query<Cs...>& Query<Cs...>::with() {
    (required_.set(world_.component_id<Ts>()), ...);
    return *this;
}

template <class... Cs>
template <class... Ts>
Query<Cs...>& Query<Cs...>::without() {
    (excluded_.set(world_.component_id<Ts>()), ...);
    return *this;
}

template <class... Cs>
template <class... Ts>
Query<Cs...>& Query<Cs...>::added() {
    static_assert(((!std::is_empty_v<Ts>) && ...), "tags have no ticks; change filters need data components");
    ((required_.set(world_.component_id<Ts>()), tick_filters_.push_back(TickFilter{world_.component_id<Ts>(), true})), ...);
    return *this;
}

template <class... Cs>
template <class... Ts>
Query<Cs...>& Query<Cs...>::changed() {
    static_assert(((!std::is_empty_v<Ts>) && ...), "tags have no ticks; change filters need data components");
    ((required_.set(world_.component_id<Ts>()), tick_filters_.push_back(TickFilter{world_.component_id<Ts>(), false})), ...);
    return *this;
}

template <class... Cs>
bool Query<Cs...>::row_passes(const ecs::detail::Table& t, u32 row, u32 now) const {
    if (!has_since_) return true;  // no reader tick: every row is new
    for (const TickFilter& f : tick_filters_) {
        const u32* ticks = f.added ? t.added_ticks(f.id) : t.changed_ticks(f.id);
        if (!ecs::detail::tick_newer(ticks[row], since_, now)) return false;
    }
    return true;
}

template <class... Cs>
template <class Fn>
void Query<Cs...>::each(Fn&& fn) {
    // Mutable items stamp their row's changed tick (one write tick per each() call).
    constexpr bool kAnyMutable = (!std::is_const_v<std::remove_reference_t<Cs>> || ...);
    [[maybe_unused]] const u32 stamp = kAnyMutable ? world_.write_tick() : 0;
    const u32 now = world_.change_tick();
    for (const auto& tp : world_.tables()) {
        const ecs::detail::Table& t = *tp;
        const u32 rows = t.size();
        if (rows == 0 || !t.mask().contains_all(required_) || t.mask().intersects(excluded_)) continue;
        const Entity* es = t.entities().data();
        [&]<usize... I>(std::index_sequence<I...>) {
            std::tuple<ecs::detail::column_ptr_t<Cs>...> cols{
                reinterpret_cast<ecs::detail::column_ptr_t<Cs>>(t.column(ids_[I]))...};
            u32* stamps[sizeof...(Cs) > 0 ? sizeof...(Cs) : 1] = {
                (std::is_const_v<std::remove_reference_t<Cs>> ? nullptr : t.changed_ticks(ids_[I]))...};
            for (u32 r = 0; r < rows; ++r) {
                if (!tick_filters_.empty() && !row_passes(t, r, now)) continue;
                if constexpr (std::is_invocable_v<Fn&, Entity, decltype(*std::get<I>(cols))...>) {
                    fn(es[r], std::get<I>(cols)[r]...);
                } else {
                    fn(std::get<I>(cols)[r]...);
                }
                if constexpr (kAnyMutable) {
                    for (u32* st : stamps)
                        if (st) st[r] = stamp;
                }
            }
        }(std::index_sequence_for<Cs...>{});
    }
}

template <class... Cs>
usize Query<Cs...>::count() {
    usize n = 0;
    const u32 now = world_.change_tick();
    for (const auto& tp : world_.tables()) {
        const ecs::detail::Table& t = *tp;
        if (!t.mask().contains_all(required_) || t.mask().intersects(excluded_)) continue;
        if (tick_filters_.empty()) {
            n += t.size();
            continue;
        }
        for (u32 r = 0; r < t.size(); ++r) n += row_passes(t, r, now) ? 1 : 0;
    }
    return n;
}

}  // namespace mce
