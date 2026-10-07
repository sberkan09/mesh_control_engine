#define MCE_SPIKE_ENGINE_BUILD 1
#include "spike_api.h"

namespace mce::spike {
namespace {

// The one registry. A real engine uses HashMap; a linear table keeps the spike dependency-free.
u64 g_hashes[64];
u32 g_count = 0;
u32 g_naive = 0;

}  // namespace

u32 type_id_from_hash(u64 type_hash) {
    for (u32 i = 0; i < g_count; ++i) {
        if (g_hashes[i] == type_hash) return i;
    }
    g_hashes[g_count] = type_hash;
    return g_count++;
}

u32 registered_type_count() { return g_count; }

u32 next_naive_id() { return g_naive++; }

template <class T>
void EventLog<T>::push(const T& e) {
    items_[count_ % 16] = e;
    ++count_;
}

template <class T>
u32 EventLog<T>::count() const {
    return count_;
}

template <class T>
T EventLog<T>::last() const {
    return items_[(count_ + 15) % 16];
}

template class MCE_SPIKE_INSTANTIATION_API EventLog<Damage>;

}  // namespace mce::spike
