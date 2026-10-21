#pragma once
// Generational handles (core.md §2.3, §3.3) — the only legal cross-frame engine reference type.
// HandleTable<T> = Bitsquid ID lookup table: sparse (index -> dense slot + generation),
// dense values, and a back-reference array. Stale handles fail, never alias.

#include "mce/containers/array.h"

namespace mce {

template <class Tag>
struct Handle {
    u32 index = 0;
    u32 generation = 0;  // 0 = never valid

    constexpr bool is_null() const { return generation == 0; }
    friend constexpr bool operator==(Handle a, Handle b) { return a.index == b.index && a.generation == b.generation; }
};

template <class T, class Tag = T>
class HandleTable {
public:
    using HandleT = Handle<Tag>;

    HandleTable() = default;
    explicit HandleTable(mem::IAllocator& a) : sparse_(a), dense_(a), dense_to_sparse_(a), free_list_(a) {}

    HandleT insert(T value) {
        u32 index;
        if (!free_list_.empty()) {
            index = free_list_.back();
            free_list_.pop_back();
        } else {
            index = static_cast<u32>(sparse_.size());
            sparse_.push_back(SparseEntry{0, 1});
        }
        SparseEntry& s = sparse_[index];
        s.dense = static_cast<u32>(dense_.size());
        dense_.push_back(std::move(value));
        dense_to_sparse_.push_back(index);
        return HandleT{index, s.generation};
    }

    bool is_alive(HandleT h) const {
        return h.generation != 0 && h.index < sparse_.size() && sparse_[h.index].generation == h.generation &&
               sparse_[h.index].dense != kDead;
    }

    T* try_get(HandleT h) { return is_alive(h) ? &dense_[sparse_[h.index].dense] : nullptr; }
    const T* try_get(HandleT h) const { return is_alive(h) ? &dense_[sparse_[h.index].dense] : nullptr; }

    bool erase(HandleT h) {
        if (!is_alive(h)) return false;
        SparseEntry& s = sparse_[h.index];
        const u32 slot = s.dense;
        const u32 last = static_cast<u32>(dense_.size() - 1);
        if (slot != last) {
            dense_[slot] = std::move(dense_[last]);
            dense_to_sparse_[slot] = dense_to_sparse_[last];
            sparse_[dense_to_sparse_[slot]].dense = slot;
        }
        dense_.pop_back();
        dense_to_sparse_.pop_back();
        s.dense = kDead;
        // Generation wraps after 2^32-1 reuses (~4000 years at 60 Hz); 0 is reserved for null.
        if (++s.generation == 0) s.generation = 1;
        free_list_.push_back(h.index);
        return true;
    }

    usize size() const { return dense_.size(); }
    Span<T> dense() { return dense_.span(); }
    Span<const T> dense() const { return dense_.span(); }
    // Handle of the dense element at `slot` (for iteration with handles).
    HandleT handle_at(usize slot) const {
        const u32 index = dense_to_sparse_[slot];
        return HandleT{index, sparse_[index].generation};
    }

private:
    static constexpr u32 kDead = ~u32(0);
    struct SparseEntry {
        u32 dense;
        u32 generation;
    };
    Array<SparseEntry> sparse_;
    Array<T> dense_;
    Array<u32> dense_to_sparse_;
    Array<u32> free_list_;
};

}  // namespace mce
