// ue_wrap/core/caller_locals.cpp -- see ue_wrap/core/caller_locals.h.

#include "ue_wrap/core/caller_locals.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ue_wrap::caller_locals {
namespace {

namespace R = ue_wrap::reflection;

// A TArray's header: the element buffer, the count and the capacity.
struct ArrayView {
    uint8_t* data;
    int32_t  num;
    int32_t  max;
};

constexpr size_t kActorBytes = sizeof(void*);
constexpr int32_t kWeakBytes = 8;   // a TWeakObjectPtr: {int32 index, int32 serial}

// The removal itself, with no logging: -1 and the reason when the array does not read as one.
int RemoveCore(uint8_t* locals, int32_t arrayOffset, bool (*drop)(void*, void*), void* ctx, const char** why,
               int32_t* keptOut) {
    *why = nullptr;
    if (!locals || !drop) { *why = "no locals or no predicate"; return -1; }
    if (arrayOffset < 0) { *why = "a negative array offset"; return -1; }
    ArrayView view;
    std::memcpy(&view, locals + arrayOffset, sizeof(view));
    if (view.num < 0 || view.num > view.max) { *why = "a count outside [0, max]"; return -1; }
    if (view.num > 0 && !view.data) { *why = "a null buffer with a positive count"; return -1; }
    int32_t kept = 0;
    for (int32_t i = 0; i < view.num; ++i) {
        void* actor;
        std::memcpy(&actor, view.data + static_cast<size_t>(i) * kActorBytes, kActorBytes);
        if (drop(ctx, actor)) continue;
        if (kept != i) std::memcpy(view.data + static_cast<size_t>(kept) * kActorBytes, &actor, kActorBytes);
        ++kept;
    }
    const int dropped = view.num - kept;
    std::memcpy(locals + arrayOffset + offsetof(ArrayView, num), &kept, sizeof(kept));
    *keptOut = kept;
    return dropped;
}

// RemoveCore plus the public contract on `remaining`: written on a removal, left as it was on a refusal.
// What RemoveActors runs, without its log, so the selftest proves the code the public entry runs.
int RemoveInto(uint8_t* locals, int32_t arrayOffset, bool (*drop)(void*, void*), void* ctx, int* remaining,
               const char** why) {
    int32_t kept = 0;
    const int dropped = RemoveCore(locals, arrayOffset, drop, ctx, why, &kept);
    if (dropped >= 0 && remaining) *remaining = kept;
    return dropped;
}

bool FitsHit(const HitLayout& l) {
    return l.size > 0 && l.actor >= 0 && l.component >= 0 && l.actor + kWeakBytes <= l.size &&
           l.component + kWeakBytes <= l.size;
}

std::atomic<bool> g_selftestPassed{false};

}  // namespace

int RemoveActors(uint8_t* locals, int32_t arrayOffset, bool (*drop)(void* ctx, void* actor), void* ctx,
                 int* remaining) {
    const char* why = nullptr;
    const int dropped = RemoveInto(locals, arrayOffset, drop, ctx, remaining, &why);
    if (dropped < 0) {
        static std::atomic<bool> s_said{false};
        if (!s_said.exchange(true, std::memory_order_relaxed))
            UE_LOGE("caller_locals: RemoveActors refused (offset %d): %s", arrayOffset, why ? why : "?");
    }
    return dropped;
}

bool ResolveHitLayout(HitLayout& layout) {
    void* hr = R::FindObject(L"HitResult", L"ScriptStruct");
    if (!hr) return false;
    layout.size = R::StructSize(hr);
    layout.actor = R::FindPropertyOffset(hr, L"Actor");
    layout.component = R::FindPropertyOffset(hr, L"Component");
    return FitsHit(layout);
}

bool ClearHitActor(uint8_t* hit, const HitLayout& layout) {
    if (!hit || !FitsHit(layout)) return false;
    const uint64_t null = kNullWeakObjectPtr;
    std::memcpy(hit + layout.actor, &null, kWeakBytes);
    std::memcpy(hit + layout.component, &null, kWeakBytes);
    return true;
}

bool RunSelftest() {
    int checks = 0, failed = 0;
    auto ok = [&](bool cond, const char* what) {
        ++checks;
        if (!cond) {
            ++failed;
            UE_LOGE("caller-locals selftest FAIL: %s", what);
        }
    };

    // A caller's locals with a TArray<AActor*> at offset 16 over five fake actors (a canary after them in
    // the buffer's spare capacity), dropping the middle one and the last.
    struct Drop { void* a; void* b; };
    const auto dropTwo = [](void* ctx, void* actor) {
        const Drop* d = static_cast<const Drop*>(ctx);
        return actor == d->a || actor == d->b;
    };
    const auto dropNone = [](void*, void*) { return false; };
    constexpr int32_t kArrayAt = 16;
    void* fake[8] = {};
    for (int i = 0; i < 5; ++i) fake[i] = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000 + 0x10 * i));
    void* const canary = reinterpret_cast<void*>(static_cast<uintptr_t>(0xCAFE0000));
    fake[5] = canary;
    alignas(16) uint8_t locals[96] = {};
    const ArrayView view{reinterpret_cast<uint8_t*>(fake), 5, 8};
    std::memcpy(locals + kArrayAt, &view, sizeof(view));
    Drop which{fake[2], fake[4]};
    void* const before[8] = {fake[0], fake[1], fake[2], fake[3], fake[4], fake[5], fake[6], fake[7]};
    const char* why = nullptr;
    int remaining = -7;
    const int dropped = RemoveActors(locals, kArrayAt, +dropTwo, &which, &remaining);
    ArrayView after;
    std::memcpy(&after, locals + kArrayAt, sizeof(after));
    ok(dropped == 2 && after.num == 3, "dropping the middle and the last of five leaves three");
    ok(fake[0] == before[0] && fake[1] == before[1] && fake[2] == before[3], "the rest keep their order");
    ok(fake[3] == before[3] && fake[4] == before[4] && fake[5] == canary && fake[6] == before[6],
       "bytes past the new count are untouched");
    ok(after.data == view.data && after.max == view.max, "the buffer and its capacity are not touched");

    ok(remaining == 3, "the count after the removal is written to remaining");

    // Nothing to drop changes nothing, and remaining is the count it was.
    remaining = -7;
    ok(RemoveActors(locals, kArrayAt, +dropNone, nullptr, &remaining) == 0 && remaining == 3,
       "no drop returns zero and remaining is the unchanged count");

    // An array that does not read as one is refused whole, and left as it was.
    uint8_t bad[96];
    std::memcpy(bad, locals, sizeof(bad));
    const ArrayView over{reinterpret_cast<uint8_t*>(fake), 9, 8};
    std::memcpy(bad + kArrayAt, &over, sizeof(over));
    uint8_t badCopy[96];
    std::memcpy(badCopy, bad, sizeof(bad));
    int refusedRemaining = -7;
    ok(RemoveInto(bad, kArrayAt, +dropTwo, &which, &refusedRemaining, &why) == -1 && why &&
           std::memcmp(bad, badCopy, sizeof(bad)) == 0 && refusedRemaining == -7,
       "a count past the capacity is refused, left alone, and writes no remaining");
    const ArrayView nodata{nullptr, 2, 8};
    std::memcpy(bad + kArrayAt, &nodata, sizeof(nodata));
    ok(RemoveInto(bad, kArrayAt, +dropTwo, &which, &refusedRemaining, &why) == -1 && refusedRemaining == -7,
       "a null buffer with a count is refused");
    const ArrayView negative{reinterpret_cast<uint8_t*>(fake), -1, 8};
    std::memcpy(bad + kArrayAt, &negative, sizeof(negative));
    ok(RemoveInto(bad, kArrayAt, +dropTwo, &which, &refusedRemaining, &why) == -1 && refusedRemaining == -7,
       "a negative count is refused");
    ok(RemoveInto(locals, -1, +dropTwo, &which, &refusedRemaining, &why) == -1 &&
           RemoveInto(nullptr, kArrayAt, +dropTwo, &which, &refusedRemaining, &why) == -1 && refusedRemaining == -7,
       "a negative offset and null locals are refused");

    // An FHitResult stand-in with synthetic member offsets: both weak pointers become the null value and no
    // other byte moves; a layout that does not fit, and a null block, are refused.
    constexpr int32_t kHitSize = 48;
    uint8_t hit[kHitSize];
    for (int i = 0; i < kHitSize; ++i) hit[i] = static_cast<uint8_t>(0x11 + i);
    uint8_t hitCopy[kHitSize];
    std::memcpy(hitCopy, hit, sizeof(hit));
    HitLayout layout;
    layout.actor = 16;
    layout.component = 24;
    layout.size = kHitSize;
    ok(ClearHitActor(hit, layout), "a fitting layout clears");
    uint64_t actorWeak, componentWeak;
    std::memcpy(&actorWeak, hit + layout.actor, 8);
    std::memcpy(&componentWeak, hit + layout.component, 8);
    ok(actorWeak == kNullWeakObjectPtr && componentWeak == kNullWeakObjectPtr, "both weak pointers read as null");
    bool others = true;
    for (int i = 0; i < kHitSize; ++i)
        if ((i < layout.actor || i >= layout.actor + 8) && (i < layout.component || i >= layout.component + 8) &&
            hit[i] != hitCopy[i])
            others = false;
    ok(others, "every other byte of the hit is unchanged");
    std::memcpy(hit, hitCopy, sizeof(hit));
    HitLayout past = layout;
    past.component = kHitSize - 4;
    ok(!ClearHitActor(hit, past) && std::memcmp(hit, hitCopy, sizeof(hit)) == 0,
       "a member past the size is refused and nothing is written");
    ok(!ClearHitActor(nullptr, layout) && !ClearHitActor(hit, HitLayout{}), "a null block and an unresolved layout are refused");

    if (failed == 0) UE_LOGI("caller-locals selftest: ALL PASS (%d checks)", checks);
    else             UE_LOGE("caller-locals selftest: %d/%d FAILED", failed, checks);
    g_selftestPassed.store(failed == 0);
    return g_selftestPassed.load();
}

bool SelftestPassed() { return g_selftestPassed.load(); }

}  // namespace ue_wrap::caller_locals
