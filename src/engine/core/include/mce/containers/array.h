#pragma once
// Array<T>: growable, allocator-aware sequence — the default sequence type in engine code.
// SmallVector<T, N>: same API with N elements stored inline before spilling to the allocator.
// No exceptions; out-of-memory asserts. Growth: 1.5x (min 4).

#include <cstring>
#include <initializer_list>
#include <new>
#include <type_traits>
#include <utility>

#include "mce/memory/allocator.h"

#if defined(_MSC_VER)
#define MCE_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#define MCE_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

namespace mce {

namespace detail {
template <class T, usize N>
struct InlineStorage {
    alignas(T) unsigned char bytes[N * sizeof(T)];
    T* ptr() { return reinterpret_cast<T*>(bytes); }
    const T* ptr() const { return reinterpret_cast<const T*>(bytes); }
};
template <class T>
struct InlineStorage<T, 0> {
    T* ptr() { return nullptr; }
    const T* ptr() const { return nullptr; }
};
}  // namespace detail

template <class T, usize InlineN>
class ArrayImpl {
public:
    using value_type = T;
    using iterator = T*;
    using const_iterator = const T*;

    ArrayImpl() : alloc_(&mem::heap()) { init_inline(); }
    explicit ArrayImpl(mem::IAllocator& a) : alloc_(&a) { init_inline(); }
    ArrayImpl(std::initializer_list<T> init, mem::IAllocator& a = mem::heap()) : alloc_(&a) {
        init_inline();
        reserve(init.size());
        for (const T& v : init) push_back(v);
    }
    ArrayImpl(const ArrayImpl& o) : alloc_(o.alloc_) {
        init_inline();
        reserve(o.size_);
        for (usize i = 0; i < o.size_; ++i) new (data_ + i) T(o.data_[i]);
        size_ = o.size_;
    }
    ArrayImpl(ArrayImpl&& o) noexcept : alloc_(o.alloc_) {
        init_inline();
        steal(std::move(o));
    }
    ArrayImpl& operator=(const ArrayImpl& o) {
        if (this != &o) {
            clear();
            reserve(o.size_);
            for (usize i = 0; i < o.size_; ++i) new (data_ + i) T(o.data_[i]);
            size_ = o.size_;
        }
        return *this;
    }
    ArrayImpl& operator=(ArrayImpl&& o) noexcept {
        if (this != &o) {
            clear();
            release_heap();
            alloc_ = o.alloc_;
            init_inline();
            steal(std::move(o));
        }
        return *this;
    }
    ~ArrayImpl() {
        clear();
        release_heap();
    }

    usize size() const { return size_; }
    usize capacity() const { return capacity_; }
    bool empty() const { return size_ == 0; }
    T* data() { return data_; }
    const T* data() const { return data_; }
    mem::IAllocator& allocator() const { return *alloc_; }

    T& operator[](usize i) {
        MCE_ASSERT_MSG(i < size_, "Array index out of range");
        return data_[i];
    }
    const T& operator[](usize i) const {
        MCE_ASSERT_MSG(i < size_, "Array index out of range");
        return data_[i];
    }
    T& front() { return (*this)[0]; }
    const T& front() const { return (*this)[0]; }
    T& back() { return (*this)[size_ - 1]; }
    const T& back() const { return (*this)[size_ - 1]; }

    T* begin() { return data_; }
    T* end() { return data_ + size_; }
    const T* begin() const { return data_; }
    const T* end() const { return data_ + size_; }

    Span<T> span() { return Span<T>(data_, size_); }
    Span<const T> span() const { return Span<const T>(data_, size_); }
    operator Span<T>() { return span(); }              // NOLINT(google-explicit-constructor)
    operator Span<const T>() const { return span(); }  // NOLINT(google-explicit-constructor)

    void reserve(usize n) {
        if (n > capacity_) reallocate(n);
    }

    template <class... Args>
    T& emplace_back(Args&&... args) {
        if (size_ == capacity_) {
            // Construct into the new buffer first: args may alias an element of this array.
            const usize new_cap = grow_capacity(size_ + 1);
            T* fresh = allocate_buffer(new_cap);
            new (fresh + size_) T(std::forward<Args>(args)...);
            relocate(fresh, size_);
            capacity_ = new_cap;
        } else {
            new (data_ + size_) T(std::forward<Args>(args)...);
        }
        return data_[size_++];
    }
    T& push_back(const T& v) { return emplace_back(v); }
    T& push_back(T&& v) { return emplace_back(std::move(v)); }

    void pop_back() {
        MCE_ASSERT_MSG(size_ > 0, "pop_back on empty Array");
        data_[--size_].~T();
    }

    void clear() {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            for (usize i = 0; i < size_; ++i) data_[i].~T();
        }
        size_ = 0;
    }

    void resize(usize n) {
        grow_for(n);
        for (usize i = size_; i < n; ++i) new (data_ + i) T();
        shrink_to(n);
    }
    void resize(usize n, const T& fill) {
        grow_for(n);
        for (usize i = size_; i < n; ++i) new (data_ + i) T(fill);
        shrink_to(n);
    }
    // Grows without value-initializing trivially constructible elements (columns, scratch).
    void resize_uninitialized(usize n) {
        static_assert(std::is_trivially_default_constructible_v<T> && std::is_trivially_destructible_v<T>);
        grow_for(n);
        size_ = n;
    }

    // O(1) removal; does not preserve order.
    void erase_swap(usize i) {
        MCE_ASSERT_MSG(i < size_, "erase_swap index out of range");
        if (i != size_ - 1) data_[i] = std::move(data_[size_ - 1]);
        pop_back();
    }
    // O(n) removal preserving order.
    void erase(usize i) {
        MCE_ASSERT_MSG(i < size_, "erase index out of range");
        for (usize j = i; j + 1 < size_; ++j) data_[j] = std::move(data_[j + 1]);
        pop_back();
    }
    void insert(usize i, T v) {
        MCE_ASSERT_MSG(i <= size_, "insert index out of range");
        if (i == size_) {
            emplace_back(std::move(v));
            return;
        }
        emplace_back(std::move(data_[size_ - 1]));
        for (usize j = size_ - 2; j > i; --j) data_[j] = std::move(data_[j - 1]);
        data_[i] = std::move(v);
    }

    // Index of the first element equal to v, or size() when absent.
    usize find(const T& v) const {
        for (usize i = 0; i < size_; ++i)
            if (data_[i] == v) return i;
        return size_;
    }
    bool contains(const T& v) const { return find(v) != size_; }

private:
    void init_inline() {
        data_ = inline_.ptr();
        capacity_ = InlineN;
        size_ = 0;
    }
    bool is_inline() const { return InlineN > 0 && data_ == inline_.ptr(); }

    // The resize family grows like emplace_back (1.5x steps), so growing a buffer a chunk at a time is amortized
    // linear; from a zero-capacity array it allocates exactly `n` (no 4-element floor). reserve() stays exact (it is the caller's size hint).
    void grow_for(usize n) {
        if (n > capacity_) reallocate(capacity_ == 0 ? n : grow_capacity(n));
    }

    usize grow_capacity(usize needed) const {
        usize c = capacity_ + capacity_ / 2;
        if (c < 4) c = 4;
        return c < needed ? needed : c;
    }

    T* allocate_buffer(usize n) {
        mem::Allocation m = alloc_->allocate(n * sizeof(T), alignof(T) > mem::kDefaultAlign ? alignof(T) : mem::kDefaultAlign);
        MCE_ASSERT_MSG(m.ptr != nullptr, "Array out of memory");
        return static_cast<T*>(m.ptr);
    }

    // Moves the first `count` elements into `fresh` and adopts it as the buffer.
    void relocate(T* fresh, usize count) {
        if constexpr (std::is_trivially_copyable_v<T>) {
            if (count) std::memcpy(static_cast<void*>(fresh), static_cast<const void*>(data_), count * sizeof(T));
        } else {
            for (usize i = 0; i < count; ++i) {
                new (fresh + i) T(std::move(data_[i]));
                data_[i].~T();
            }
        }
        release_heap();
        data_ = fresh;
    }

    void reallocate(usize n) {
        T* fresh = allocate_buffer(n);
        relocate(fresh, size_);
        capacity_ = n;
    }

    void release_heap() {
        if (data_ && !is_inline()) alloc_->free(data_);
        data_ = inline_.ptr();
        capacity_ = InlineN;
    }

    void shrink_to(usize n) {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            for (usize i = n; i < size_; ++i) data_[i].~T();
        }
        size_ = n;
    }

    void steal(ArrayImpl&& o) {
        if (o.is_inline()) {
            for (usize i = 0; i < o.size_; ++i) {
                new (data_ + i) T(std::move(o.data_[i]));
                o.data_[i].~T();
            }
            size_ = o.size_;
            o.size_ = 0;
        } else {
            data_ = o.data_;
            size_ = o.size_;
            capacity_ = o.capacity_;
            o.init_inline();
        }
    }

    mem::IAllocator* alloc_;
    T* data_ = nullptr;
    usize size_ = 0;
    usize capacity_ = 0;
    MCE_NO_UNIQUE_ADDRESS detail::InlineStorage<T, InlineN> inline_;
};

template <class T>
using Array = ArrayImpl<T, 0>;

template <class T, usize N>
using SmallVector = ArrayImpl<T, N>;

}  // namespace mce
