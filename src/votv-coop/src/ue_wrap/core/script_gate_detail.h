// ue_wrap/core/script_gate_detail.h -- PRIVATE seam between the two script_gate translation units:
// script_gate.cpp (the loop's detour, exact watches, the holds) and script_gate_names.cpp (name watches
// and their resolve). Not public API.

#pragma once

#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace ue_wrap::script_gate::detail {

namespace R = ue_wrap::reflection;

// Fixed open-addressing tables, never rehashed and never freed: a slot's key is published last
// with release, so the loop's reader sees an empty slot or a whole entry, and a retired watch
// only clears its enabled flag, leaving the probe chain intact. Two key spaces, two tables: an
// exact watch keys on the UFunction pointer, a name watch on the FName's two indices (its
// unresolved placeholder holds a second slot). What fills a table is its KEYED slots, retired
// ones included, so the cap counts those: at most 3/8 of a table, the load at which a miss
// probes under two slots. A name watch, class-scoped or not, holds two keyed slots, so the name table
// takes 768 name watches. A two-peer smoke on 0.9.0n held 163-166 per peer (the measurement the
// 11-bit table was sized on) and the 0.10.0 port's held 374-378; 12 bits leave room for both and
// for the watches each polled state moved onto a watch adds.
constexpr int kSlotBits = 12;
constexpr int kSlots = 1 << kSlotBits;
constexpr int kMaxKeyed = kSlots * 3 / 8;

struct Entry {
    std::atomic<std::uint64_t> key{0};
    std::atomic<bool> enabled{false};
    int tag = 0;
    PreFn pre = nullptr;
    PostFn post = nullptr;
    const wchar_t* name = nullptr;   // a name watch's registered literal; null for an exact one
    const wchar_t* className = nullptr;  // a class-scoped name watch's class literal, else null
    std::uint64_t classKey = 0;       // that class's FName key once resolved; 0 matches any owner
    std::atomic<bool> resolved{true}; // a name watch is inert until its FName is known
    std::atomic<bool> dead{false};    // a name watch that can never fire: a full table, or its class has no such body
    std::atomic<bool> unjudged{false};  // a class-scoped watch whose class was not loaded when it resolved
};
extern Entry g_fnTable[kSlots];
extern Entry g_nameTable[kSlots];
extern std::atomic<int> g_fnWatches;      // enabled exact watches
extern std::atomic<int> g_nameWatches;    // enabled name watches, placeholders included
extern std::atomic<int> g_namesPending;
extern std::atomic<int> g_unjudged;       // class-scoped watches waiting for their class to load
extern std::uint64_t g_judgedAtClassSet; // the class set's number at the last judging pass; game thread
extern int g_fnKeyed;                    // keyed slots, under the registration mutex
extern int g_nameKeyed;
extern std::mutex g_regMutex;   // registration only; never on the call path

inline std::uint64_t HashKey(std::uint64_t key) {
    return (key >> 4) * 0x9E3779B97F4A7C15ull;
}
// The hash's top bits pick the slot: exactly kSlotBits of them, so every slot is a home slot.
inline int SlotOf(std::uint64_t key) {
    return static_cast<int>(HashKey(key) >> (64 - kSlotBits));
}
inline std::uint64_t NameKey(const R::FName& n) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(n.ComparisonIndex)) << 32) |
           static_cast<std::uint32_t>(n.Number);
}

// The FName key of the class that owns `fn`'s body, its outer; read only after a class-scoped
// entry has matched the function's name.
inline std::uint64_t OwnerKey(void* fn) {
    void* owner = R::OuterOf(fn);
    return owner ? NameKey(R::NameOf(owner)) : 0;
}

// Whether entry `e`, whose key matched, is scoped to a class other than the one that owns `fn`'s
// body. `owner` caches that class's key across the entries of one probe (0 until first read).
inline bool OtherOwner(const Entry& e, void* fn, std::uint64_t& owner) {
    if (e.classKey == 0) return false;
    if (owner == 0) owner = OwnerKey(fn);
    return owner != e.classKey;
}

template <class T> inline T Read(const void* base, size_t off) {
    T v; std::memcpy(&v, reinterpret_cast<const std::uint8_t*>(base) + off, sizeof(T)); return v;
}

bool Register(Entry* table, std::atomic<int>& count, int& keyed, std::uint64_t key, int tag, PreFn pre,
              PostFn post, const wchar_t* name, const wchar_t* className, std::uint64_t classKey, bool resolved,
              bool enabled = true, bool unjudged = false);
bool Retire(Entry* table, std::atomic<int>& count, std::uint64_t key, int tag, PreFn pre, PostFn post,
            const wchar_t* name);
bool RefusedUninstalled(const char* what);
bool WatchFn(void* ufunction, int tag, PreFn pre, PostFn post);
bool UnwatchFn(void* ufunction, int tag, PreFn pre, PostFn post);

}  // namespace ue_wrap::script_gate::detail
