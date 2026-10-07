#pragma once
// FlatMap<K, V>: sorted Array of key/value pairs. Binary-search lookup, O(n) insert/erase,
// deterministic (key-ordered) iteration — the map to use for anything sim-visible (FR-RUN-040).

#include "mce/containers/array.h"

namespace mce {

template <class K, class V>
class FlatMap {
public:
    struct Entry {
        K key;
        V value;
    };

    FlatMap() = default;
    explicit FlatMap(mem::IAllocator& a) : entries_(a) {}

    usize size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }
    void clear() { entries_.clear(); }
    void reserve(usize n) { entries_.reserve(n); }

    V* find(const K& key) {
        const usize i = lower_bound(key);
        return (i < entries_.size() && entries_[i].key == key) ? &entries_[i].value : nullptr;
    }
    const V* find(const K& key) const {
        const usize i = lower_bound(key);
        return (i < entries_.size() && entries_[i].key == key) ? &entries_[i].value : nullptr;
    }
    bool contains(const K& key) const { return find(key) != nullptr; }

    V& insert_or_assign(const K& key, V value) {
        const usize i = lower_bound(key);
        if (i < entries_.size() && entries_[i].key == key) {
            entries_[i].value = std::move(value);
            return entries_[i].value;
        }
        entries_.insert(i, Entry{key, std::move(value)});
        return entries_[i].value;
    }

    std::pair<V*, bool> try_emplace(const K& key, V value = V()) {
        const usize i = lower_bound(key);
        if (i < entries_.size() && entries_[i].key == key) return {&entries_[i].value, false};
        entries_.insert(i, Entry{key, std::move(value)});
        return {&entries_[i].value, true};
    }

    V& operator[](const K& key) { return *try_emplace(key).first; }

    bool erase(const K& key) {
        const usize i = lower_bound(key);
        if (i < entries_.size() && entries_[i].key == key) {
            entries_.erase(i);
            return true;
        }
        return false;
    }

    Entry* begin() { return entries_.begin(); }
    Entry* end() { return entries_.end(); }
    const Entry* begin() const { return entries_.begin(); }
    const Entry* end() const { return entries_.end(); }
    Span<const Entry> entries() const { return entries_.span(); }

private:
    usize lower_bound(const K& key) const {
        usize lo = 0, hi = entries_.size();
        while (lo < hi) {
            const usize mid = lo + (hi - lo) / 2;
            if (entries_[mid].key < key) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    Array<Entry> entries_;
};

}  // namespace mce
