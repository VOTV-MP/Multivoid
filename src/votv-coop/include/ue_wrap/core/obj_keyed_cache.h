// ue_wrap/core/obj_keyed_cache.h -- a small cache of one value per UObject, for a lookup keyed by a class or a
// function that more than one object of the kind asks about in turn (the single-slot reflection::InstanceOffset and
// ParamOffset resolve again at every switch). A key is held as a CachedObjRef, so an entry whose object died, or whose
// slot a successor took, reads as absent. A full cache replaces its oldest entry. Game thread.

#pragma once

#include "ue_wrap/core/cached_obj_ref.h"

namespace ue_wrap {

template <typename T, int N>
class ObjKeyedCache {
public:
    // The value held for `key`, or null when none is.
    T* Find(void* key) {
        for (Entry& e : entries_)
            if (e.key.Is(key)) return &e.value;
        return nullptr;
    }

    // Hold `value` for `key` (not yet held), in place of the oldest entry when the cache is full.
    T& Insert(void* key, const T& value) {
        Entry& e = entries_[next_];
        next_ = (next_ + 1) % N;
        e.key.Set(key);
        e.value = value;
        return e.value;
    }

    void Clear() {
        for (Entry& e : entries_) e.key.Reset();
        next_ = 0;
    }

private:
    struct Entry {
        CachedObjRef key;
        T value{};
    };
    Entry entries_[N];
    int next_ = 0;
};

}  // namespace ue_wrap
