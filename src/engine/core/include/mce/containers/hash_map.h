#pragma once
// HashMap<K, V>: open addressing, linear probing, power-of-two capacity, Robin Hood displacement
// with backward-shift deletion and a 1-byte probe-distance pseudoslot per bucket (core.md §3.2).
//
// Iteration order is deterministic for a given operation sequence, but it must never drive
// sim-visible ordering (use FlatMap for that — FR-RUN-040).

#include <new>
#include <utility>

#include "mce/hash.h"
#include "mce/memory/allocator.h"

namespace mce {

template <class K, class V, class H = Hash<K>>
class HashMap {
public:
    struct Entry {
        K key;
        V value;
    };

    template <bool Const>
    class Iter {
        using MapT = std::conditional_t<Const, const HashMap, HashMap>;
        using EntryT = std::conditional_t<Const, const Entry, Entry>;

    public:
        Iter(MapT* m, usize i) : map_(m), i_(i) { skip(); }
        EntryT& operator*() const { return map_->entries_[i_]; }
        EntryT* operator->() const { return &map_->entries_[i_]; }
        Iter& operator++() {
            ++i_;
            skip();
            return *this;
        }
        bool operator==(const Iter& o) const { return i_ == o.i_; }
        bool operator!=(const Iter& o) const { return i_ != o.i_; }

    private:
        void skip() {
            while (i_ < map_->capacity_ && map_->meta_[i_] == 0) ++i_;
        }
        MapT* map_;
        usize i_;
    };

    HashMap() : alloc_(&mem::heap()) {}
    explicit HashMap(mem::IAllocator& a) : alloc_(&a) {}
    HashMap(const HashMap&) = delete;
    HashMap& operator=(const HashMap&) = delete;
    HashMap(HashMap&& o) noexcept
        : alloc_(o.alloc_), entries_(std::exchange(o.entries_, nullptr)), meta_(std::exchange(o.meta_, nullptr)),
          capacity_(std::exchange(o.capacity_, 0)), size_(std::exchange(o.size_, 0)) {}
    HashMap& operator=(HashMap&& o) noexcept {
        if (this != &o) {
            destroy_all();
            alloc_ = o.alloc_;
            entries_ = std::exchange(o.entries_, nullptr);
            meta_ = std::exchange(o.meta_, nullptr);
            capacity_ = std::exchange(o.capacity_, 0);
            size_ = std::exchange(o.size_, 0);
        }
        return *this;
    }
    ~HashMap() { destroy_all(); }

    usize size() const { return size_; }
    bool empty() const { return size_ == 0; }
    usize capacity() const { return capacity_; }

    V* find(const K& key) {
        const usize i = find_index(key);
        return i == kNone ? nullptr : &entries_[i].value;
    }
    const V* find(const K& key) const {
        const usize i = find_index(key);
        return i == kNone ? nullptr : &entries_[i].value;
    }
    bool contains(const K& key) const { return find_index(key) != kNone; }

    // Inserts if absent. Returns {value pointer, inserted}.
    template <class... Args>
    std::pair<V*, bool> try_emplace(const K& key, Args&&... args) {
        if (V* v = find(key)) return {v, false};
        reserve(size_ + 1);
        return {insert_new(K(key), V(std::forward<Args>(args)...)), true};
    }

    V& insert_or_assign(const K& key, V value) {
        if (V* v = find(key)) {
            *v = std::move(value);
            return *v;
        }
        reserve(size_ + 1);
        return *insert_new(K(key), std::move(value));
    }

    V& operator[](const K& key) { return *try_emplace(key).first; }

    bool erase(const K& key) {
        usize i = find_index(key);
        if (i == kNone) return false;
        entries_[i].~Entry();
        meta_[i] = 0;
        // Backward shift: pull following displaced entries one slot closer to home.
        usize next = (i + 1) & mask();
        while (meta_[next] > 1) {
            new (&entries_[i]) Entry(std::move(entries_[next]));
            entries_[next].~Entry();
            meta_[i] = static_cast<u8>(meta_[next] - 1);
            meta_[next] = 0;
            i = next;
            next = (next + 1) & mask();
        }
        --size_;
        return true;
    }

    void clear() {
        for (usize i = 0; i < capacity_; ++i) {
            if (meta_[i]) {
                entries_[i].~Entry();
                meta_[i] = 0;
            }
        }
        size_ = 0;
    }

    void reserve(usize n) {
        // Max load factor 7/8.
        if (n * 8 <= capacity_ * 7) return;
        usize cap = capacity_ ? capacity_ : 8;
        while (n * 8 > cap * 7) cap *= 2;
        rehash(cap);
    }

    Iter<false> begin() { return Iter<false>(this, 0); }
    Iter<false> end() { return Iter<false>(this, capacity_); }
    Iter<true> begin() const { return Iter<true>(this, 0); }
    Iter<true> end() const { return Iter<true>(this, capacity_); }

private:
    static constexpr usize kNone = ~usize(0);
    static constexpr u8 kMaxDist = 250;

    usize mask() const { return capacity_ - 1; }
    usize home(const K& key) const { return static_cast<usize>(H{}(key)) & mask(); }

    usize find_index(const K& key) const {
        if (size_ == 0) return kNone;
        usize i = home(key);
        for (u8 dist = 1;; ++dist) {
            // Robin Hood invariant: once the resident is closer to home than we would be, stop.
            if (meta_[i] < dist) return kNone;
            if (meta_[i] == dist && entries_[i].key == key) return i;
            i = (i + 1) & mask();
            if (dist == kMaxDist) return kNone;
        }
    }

    V* insert_new(K&& key, V&& value) {
        Entry carry{std::move(key), std::move(value)};
        usize i = home(carry.key);
        u8 dist = 1;
        Entry* placed = nullptr;
        for (;;) {
            if (meta_[i] == 0) {
                new (&entries_[i]) Entry(std::move(carry));
                meta_[i] = dist;
                ++size_;
                return placed ? &placed->value : &entries_[i].value;
            }
            if (meta_[i] < dist) {
                // Steal from the rich: swap the carried entry with the resident.
                std::swap(carry, entries_[i]);
                std::swap(dist, meta_[i]);
                if (!placed) placed = &entries_[i];
            }
            i = (i + 1) & mask();
            ++dist;
            if (dist >= kMaxDist) {
                // Pathological clustering: grow and retry. `placed` would dangle, so re-find.
                const K probe = placed ? placed->key : carry.key;
                rehash(capacity_ * 2);
                insert_new(std::move(carry.key), std::move(carry.value));
                return find(probe);
            }
        }
    }

    void rehash(usize new_cap) {
        Entry* old_entries = entries_;
        u8* old_meta = meta_;
        const usize old_cap = capacity_;

        mem::Allocation e = alloc_->allocate(sizeof(Entry) * new_cap, alignof(Entry) > mem::kDefaultAlign ? alignof(Entry) : mem::kDefaultAlign);
        mem::Allocation m = alloc_->allocate(new_cap, mem::kDefaultAlign);
        MCE_ASSERT_MSG(e.ptr && m.ptr, "HashMap out of memory");
        entries_ = static_cast<Entry*>(e.ptr);
        meta_ = static_cast<u8*>(m.ptr);
        for (usize i = 0; i < new_cap; ++i) meta_[i] = 0;
        capacity_ = new_cap;
        size_ = 0;

        for (usize i = 0; i < old_cap; ++i) {
            if (old_meta[i]) {
                insert_new(std::move(old_entries[i].key), std::move(old_entries[i].value));
                old_entries[i].~Entry();
            }
        }
        if (old_entries) alloc_->free(old_entries);
        if (old_meta) alloc_->free(old_meta);
    }

    void destroy_all() {
        if (!entries_) return;
        clear();
        alloc_->free(entries_);
        alloc_->free(meta_);
        entries_ = nullptr;
        meta_ = nullptr;
        capacity_ = 0;
    }

    mem::IAllocator* alloc_;
    Entry* entries_ = nullptr;
    u8* meta_ = nullptr;
    usize capacity_ = 0;
    usize size_ = 0;
};

}  // namespace mce
