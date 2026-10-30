// coop/dev/container_intent_drill_throw_into_host.h -- the throwinto host's state and the verbs it ends a step with, shared by the host's
// fixture half (container_intent_drill_throw_into_host.cpp) and its verdict half (container_intent_drill_throw_into_verdict.cpp). A
// private header of coop/dev/container_intent_drill_throw_into*.cpp (src tree, not include/).

#pragma once

#include "container_intent_drill_throw_into.h"
#include "container_intent_drill_throw_into_judge.h"

#include "ue_wrap/core/cached_obj_ref.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::container_intent_drill::detail::into {

enum class HStep : uint8_t { WaitPeer, Bind, Need, ItemBind, Settle, Dingus, WaitTake, WaitArm, Trace, Late, WaitSecond, Tail, Done };

struct Held { bool have = false, fail = false; std::string line; };

// The host's run: one field per thing a step reads of an earlier one.
struct HostRun {
    HStep    step = HStep::WaitPeer;
    uint64_t ms = 0, nextMs = 0, findMs = 0, endSeenMs = 0;
    int      r = 1, t = 1, slot = 0;
    ue_wrap::CachedObjRef target, item;
    uint32_t targetEid = 0, itemEid = 0, elemEid = 0;
    FVector  packAt{};
    Meter    m;
    std::vector<uint32_t> set, live;   // the live drive element ids at the arm line; the ones live now
    bool     armRead = false;
    bool     newTarget = false;        // the control's own basket was spawned and is not yet bound
    bool     ownBasket = false;        // this sub-run (n >= 2) has spawned its own basket
    const coop::net::Session* session = nullptr;   // the session the host's tick runs on: n=5 asks it for a second world-ready client
    bool     through = false;          // the try now is the one aimed through the box (measured, not judged)
    std::string heldFixture;           // a fixture failure held until after the measure tail
    int      trigBefore = -1;          // the target's trigger state before dingus()
    FVector  settleLast{};
    int      settleStill = 0;          // checks the target stayed put
    bool     settleItem = false;       // the settled target goes on to the item line (the control's)
    bool     dingusItem = false;       // the armed target goes on to the item line (the control's), not the fixture line
    std::string armRest, endRest;
    uint32_t int0 = 0, intThrown0 = 0, relThrown0 = 0, elected0 = 0;
    Frozen   f;
    ClientEnd end;
    Held     held[kLastRun];           // the judged lines, printed together once all the sub-runs are judged
};
extern HostRun g_host;

void HNext(HStep s);
bool HExpired(uint64_t bound);
// The host's end with the FAIL line; the client is told so it does not wait out its bounds.
void HFail(const char* fmt, ...);
void FlushHeld();

// The verdict half (container_intent_drill_throw_into_verdict.cpp).
void StepHWaitTake();
void StepHWaitArm();
void StepHTrace(const coop::net::Session& s, bool due);
void StepHLate(bool due);
void StepHWaitSecond();   // n=4 is over and a second client is connected but not world-ready: its edge is waited for
bool HClientPrefail();    // a client's fixture failure before n=5 is held as the sub-run's verdict and the next sub-run begins

}  // namespace coop::dev::container_intent_drill::detail::into
