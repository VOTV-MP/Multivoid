// harness/autotest/autotest_islive_drill.cpp -- a deterministic zero-AV drill for the CachedObjRef
// discipline. Gated by VOTVCOOP_RUN_ISLIVE_DRILL=1, on its own worker thread.
//
// A bare IsLive on a freed pointer faults only once the freed page has been DECOMMITTED, so in a
// live process the fault is nondeterministic. The drill manufactures that case with
// VirtualAlloc/VirtualFree and proves both halves in one run: a legacy bare IsLive on the
// decommitted "object" takes exactly one first-chance access violation, absorbs it and returns
// false; CachedObjRef::Alive() on the same input returns false with ZERO of them, reading array
// slots only.
//
// The drill's VEH is registered for the drill body alone and removed before it returns, never
// during a normal boot, and counts only faults inside the drill's own page, so an unrelated fault
// cannot contaminate the verdict. It touches no engine object -- the fake never enters
// GUObjectArray and its planted index is out of range, which IsLiveByIndex rejects -- and it
// structurally cannot exercise the ABA impostor.

#include "harness/autotest.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;

std::atomic<uint64_t> g_drillAvCount{0};
volatile uint8_t* g_drillPage = nullptr;

LONG WINAPI DrillVeh(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord &&
        ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        const uintptr_t fault = static_cast<uintptr_t>(ep->ExceptionRecord->ExceptionInformation[1]);
        const uintptr_t page = reinterpret_cast<uintptr_t>(g_drillPage);
        if (page && fault >= page && fault < page + 0x1000)
            g_drillAvCount.fetch_add(1, std::memory_order_relaxed);
    }
    return EXCEPTION_CONTINUE_SEARCH;  // never swallow -- observe only
}

void RunDrill() {
    UE_LOGI("islive_drill: === deterministic zero-AV drill START ===");

    // A committed page posing as a UObject: plant an out-of-range InternalIndex
    // so the ByIndex path resolves to "no such slot" (false) without ever
    // needing a real engine object.
    uint8_t* page = static_cast<uint8_t*>(
        ::VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!page) { UE_LOGW("islive_drill: VirtualAlloc failed -- ABORT"); return; }
    g_drillPage = page;
    constexpr int32_t kFakeIdx = 0x7FFFFFF0;  // far past any real NumElements
    *reinterpret_cast<int32_t*>(page + ue_wrap::profile::off::UObject_InternalIndex) = kFakeIdx;

    // Capture while the page is committed (Set derefs once -- the fresh-same-task
    // contract, satisfied here by construction).
    ue_wrap::CachedObjRef ref;
    ref.Set(page);
    const bool capturedIdxOk = (ref.Idx() == kFakeIdx);

    // Sanity leg on the COMMITTED page: both forms answer without faulting.
    const bool legacyCommitted = R::IsLive(page);       // false (idx out of range)
    const bool cachedCommitted = ref.Alive();           // false (no such slot)

    // THE case: decommit (address stays reserved -- nothing can remap it), so a
    // read of page memory now faults exactly like a GC-freed+decommitted object.
    if (!::VirtualFree(page, 0x1000, MEM_DECOMMIT)) {
        UE_LOGW("islive_drill: VirtualFree(MEM_DECOMMIT) failed -- ABORT");
        g_drillPage = nullptr;
        ::VirtualFree(page, 0, MEM_RELEASE);
        return;
    }

    void* veh = ::AddVectoredExceptionHandler(1, &DrillVeh);
    g_drillAvCount.store(0, std::memory_order_relaxed);

    // Leg (i): legacy bare IsLive derefs the decommitted page -> 1 first-chance
    // AV, absorbed by IsLive's SEH (the dead-man's brake), returns false. The
    // caller-attribution WARN naming THIS function is expected in the log.
    const bool legacyAnswer = R::IsLive(page);
    const uint64_t avAfterLegacy = g_drillAvCount.load(std::memory_order_relaxed);

    // Leg (ii): CachedObjRef::Alive() on the same input -> false, ZERO AVs.
    const bool cachedAnswer = ref.Alive();
    const uint64_t avAfterCached = g_drillAvCount.load(std::memory_order_relaxed);

    if (veh) ::RemoveVectoredExceptionHandler(veh);
    g_drillPage = nullptr;
    ::VirtualFree(page, 0, MEM_RELEASE);

    const bool pass =
        capturedIdxOk &&
        !legacyCommitted && !cachedCommitted &&
        !legacyAnswer && avAfterLegacy == 1 &&
        !cachedAnswer && avAfterCached == 1;  // unchanged by leg (ii)

    UE_LOGI("islive_drill: committed{legacy=%d cached=%d} decommitted{legacy=%d avs=%llu} "
            "cachedref{alive=%d avs_delta=%llu} capturedIdxOk=%d",
            legacyCommitted ? 1 : 0, cachedCommitted ? 1 : 0,
            legacyAnswer ? 1 : 0, static_cast<unsigned long long>(avAfterLegacy),
            cachedAnswer ? 1 : 0,
            static_cast<unsigned long long>(avAfterCached - avAfterLegacy),
            capturedIdxOk ? 1 : 0);
    if (pass) {
        UE_LOGI("islive_drill: VERDICT PASS -- legacy faults exactly once (absorbed), "
                "CachedObjRef answers false with zero AVs");
    } else {
        UE_LOGW("islive_drill: VERDICT FAIL -- see the line above (expected: "
                "committed 0/0, decommitted legacy=0 avs=1, cachedref alive=0 delta=0)");
    }
    UE_LOGI("islive_drill: DONE");
    ue_wrap::log::Flush();  // verdict lines are INFO (buffered); the runner polls the log
}

}  // namespace

DWORD WINAPI IsLiveDrillThread(LPVOID) { RunDrill(); return 0; }

}  // namespace harness::autotest
