// coop/dev/bucket_drill_watch.cpp -- the bucket drill's own watch on a plain bucket's ReceiveTick, for the client's copy the arm names:
// the pin arm reads the copy's water at its first POST after the drill's own dip, and the visuals arm reads the drawn water at the entry
// of each tick (what each row left to draw) and at every POST from the host's row on. The lane's own watch (coop/props/bucket_pin) writes
// the host's row at the tick's PRE and may cancel the tick; every entry of the gate still fires, so this one sees a refused tick's PRE
// and never its POST. A second watch, on deleteAlc, counts the POSTs of the alc arm's copy: a body the lane cancelled has none.

#include "bucket_drill_detail.h"

#include "coop/props/bucket_sync.h"

#include "ue_wrap/actors/bucket.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <cmath>

namespace coop::dev::bucket_drill::detail {

namespace sg = ue_wrap::script_gate;
namespace B  = ue_wrap::bucket;
namespace BS = coop::bucket_sync;

UE_FACT_FN(kRowTick, "dev.bucket_drill", L"prop_bucket_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowAlc, "dev.bucket_drill", L"prop_bucket_C", L"deleteAlc", Declared);

namespace {

constexpr int kTag = 0x42445431;   // 'BDT1'
constexpr int kTagAlc = 0x42445432;   // 'BDT2'
constexpr float kPinEps = 0.01f;   // the pinned water equals the row's to a hundredth
constexpr float kInterpEps = 0.1f;
constexpr int kMaxRowGaps = 8;

// The drawn water's distance from the water at the entry of the first tick after a row was applied to the copy.
struct RowGap { uint32_t seq; float gap; };

ue_wrap::CachedObjRef g_copy;
float g_want = 0.f;
bool g_visuals = false, g_registered = false, g_settled = false, g_dead = false;
uint32_t g_eid = 0, g_lastSeq = 0, g_target = 0;
RowGap g_gaps[kMaxRowGaps];
int g_nGaps = 0;
TickSample g_s;
ue_wrap::CachedObjRef g_alcCopy;
unsigned long long g_alcPosts = 0;
bool g_alcRegistered = false, g_alcSettled = false, g_alcDead = false;

// A deleteAlc body that ran on the armed copy: the lane's cancel leaves no POST.
void OnAlcPost(const sg::Call& c) {
    if (c.object && c.object == g_alcCopy.Get()) ++g_alcPosts;
}

// The visuals judge: the first tick entry after each row applied to the copy notes how far the drawn water stands from the water.
sg::Verdict OnPre(const sg::Call& c) {
    if (!g_visuals || g_nGaps >= kMaxRowGaps || !c.object || c.object != g_copy.Get()) return sg::Verdict::Run;
    const uint32_t seq = BS::dev::AppliedSeq(g_eid);
    B::Plain p;
    if (seq == g_lastSeq || !B::ReadPlain(c.object, p)) return sg::Verdict::Run;
    g_lastSeq = seq;
    g_gaps[g_nGaps++] = {seq, std::fabs(p.heightInterp - p.height)};
    return sg::Verdict::Run;
}

void OnPost(const sg::Call& c) {
    if (!c.object || c.object != g_copy.Get()) return;
    if (!g_visuals && g_s.posts > 0) return;   // the pin arm reads the first tick only
    if (g_visuals && (g_target == 0 || BS::dev::AppliedSeq(g_eid) < g_target)) return;   // a tick before the row says nothing of it
    B::Plain p;
    if (!B::ReadPlain(c.object, p)) return;
    ++g_s.posts;
    g_s.height = p.height;
    g_s.interp = p.heightInterp;
    g_s.empty = p.empty;
    const bool ok = g_visuals ? std::fabs(p.heightInterp - p.height) < kInterpEps && p.empty == (p.height <= 0.f)
                              : std::fabs(p.height - g_want) < kPinEps;
    g_s.good = g_s.good || ok;
}

}  // namespace

bool WatchStart() {
    if (g_registered) return !g_dead;
    g_registered = true;
    if (!sg::WatchName(kRowTick, kTag, &OnPre, &OnPost)) {
        g_dead = true;
        g_settled = true;
        return false;
    }
    return true;
}

bool WatchSettled() {
    if (!g_registered) WatchStart();
    if (g_dead) {
        Invalid("the gate took no watch on prop_bucket_C ReceiveTick");
        return false;
    }
    if (!g_settled) {
        sg::ResolvePendingNames();
        if (!sg::NameWatchSettled(kRowTick, kTag)) return false;
        g_settled = true;
        if (!sg::NameWatchLive(kRowTick, kTag)) {
            g_dead = true;
            Invalid("the drill's watch on prop_bucket_C ReceiveTick settled dead");
            return false;
        }
    }
    return true;
}

bool AlcWatchSettled() {
    if (g_alcDead) return false;
    if (!g_alcRegistered) {
        g_alcRegistered = true;
        if (!sg::WatchName(kRowAlc, kTagAlc, nullptr, &OnAlcPost)) {
            g_alcDead = true;
            Invalid("the gate took no watch on prop_bucket_C deleteAlc");
            return false;
        }
    }
    if (!g_alcSettled) {
        sg::ResolvePendingNames();
        if (!sg::NameWatchSettled(kRowAlc, kTagAlc)) return false;
        g_alcSettled = true;
        if (!sg::NameWatchLive(kRowAlc, kTagAlc)) {
            g_alcDead = true;
            Invalid("the drill's watch on prop_bucket_C deleteAlc settled dead");
            return false;
        }
    }
    return true;
}

void AlcWatchArm(void* copy) {
    g_alcCopy.Set(copy);
    g_alcPosts = 0;
}

unsigned long long AlcPosts() { return g_alcPosts; }

void WatchArm(void* copy, float wantHeight, bool judgeVisuals, uint32_t eid) {
    g_copy.Set(copy);
    g_want = wantHeight;
    g_visuals = judgeVisuals;
    g_eid = eid;
    g_lastSeq = eid ? BS::dev::AppliedSeq(eid) : 0;
    g_target = 0;
    g_nGaps = 0;
    g_s = TickSample{};
}

void WatchTarget(uint32_t seq) { g_target = seq; }

TickSample WatchRead() {
    TickSample t = g_s;
    for (int i = 0; g_target != 0 && i < g_nGaps; ++i) {
        if (g_gaps[i].seq < g_target) continue;
        t.gap0 = g_gaps[i].gap;
        break;
    }
    return t;
}

void WatchReset() {
    g_copy.Reset();
    g_want = 0.f;
    g_visuals = false;
    g_eid = g_lastSeq = g_target = 0;
    g_nGaps = 0;
    g_s = TickSample{};
    g_alcCopy.Reset();
    g_alcPosts = 0;
}

}  // namespace coop::dev::bucket_drill::detail
