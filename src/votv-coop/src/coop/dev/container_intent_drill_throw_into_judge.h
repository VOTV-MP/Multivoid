// coop/dev/container_intent_drill_throw_into_judge.h -- the throwinto arm's judgement of one try, split from the host's machine: the
// words of the client's two lines, the host's own reads of the try, and the forms decided on the two record-count deltas. A private
// header of coop/dev/container_intent_drill_throw_into*.cpp (src tree, not include/). See container_intent_drill_throw_into_host.cpp.

#pragma once

#include "container_intent_drill_throw_into.h"

#include <cstdint>
#include <string>

namespace coop::dev::container_intent_drill::detail::into {

inline constexpr uint64_t kHostSettleMs = 15000;   // the client's end line to the host's copy having settled

// The words of the client's end line (seq, c0, gone, rest) and of its late line (c1, stray, ov, adds, addms, alive, gr: the client's grabbing_actor is
// set), the second read after the host's own.
struct ClientEnd {
    long long seq = 0, c0 = -1, c1 = -1, stray = 0, ov = 0, adds = 0, addMs = 0, gone = 0, alive = 0, cpre = 0, cdes = 0, grab = 0;
    long long sa = 0, se = 0, sk = 0, fwh = 0;   // the client's stray terms (alive, excess, key) and whether its held drive was filed while held
    long long obs0 = -1, obs1 = -1, obsStray = 0;   // n=5: the first client's mirror before and after, and its stray copies
    int rest[3] = {};
};
bool ReadEnd(const std::string& s, ClientEnd& c);
bool ReadLate(const std::string& s, ClientEnd& c);
bool ReadObserver(const std::string& s, ClientEnd& c);   // n=5: the first client's late line (c0, c1, stray)

// The host's own reads of a try, taken once its copy has settled (or the bound passed) and before the client's second read.
struct Frozen {
    int h1 = -1, strayH = 0, sa = 0, se = 0, sk = 0;   // the stray count and its three terms (alive, excess, key)
    bool alive = false, settled = false, hadElem = false;
    float restCm = 0.f;
    FVector last{};
};

// The judgement of a try. `why` is the FAIL text, empty for a pass; `miss` when the prop rests outside the basket.
struct Outcome { bool miss = false; char why[200] = ""; };

// What the host knows of a try besides the two reads: the sub-run, the basket's start count, the record-count deltas, whether the
// throw was elected, and the throw intents and releases the prop_throw counters saw in it.
struct JudgeIn {
    int r = 0, c0 = 0, dh = 0, dc = 0;
    bool elected = false;
    uint32_t intSeen = 0, intThrown = 0, relThrown = 0;
};

// The forms are decided on the two record-count deltas and on where the prop is, in this order.
Outcome Judge(const JudgeIn& in, const Frozen& f, const ClientEnd& c);

}  // namespace coop::dev::container_intent_drill::detail::into
