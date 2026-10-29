// coop/props/prop_throw.cpp -- see coop/props/prop_throw.h. The throwShit watch and the thrower's records.

#include "coop/props/prop_throw.h"

#include "coop/net/net_clock.h"
#include "coop/net/throw_bounds.h"
#include "coop/player/players_registry.h"
#include "coop/props/active_drive.h"   // NowMs: the clock a throw's rows count from
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace coop::prop_throw {
namespace {

namespace SG = ue_wrap::script_gate;
namespace R  = ue_wrap::reflection;
namespace tb = coop::net::throw_bounds;

// The one body every throw path runs, after the launch velocity is set. Declared on the pawn's class; the
// puppet's body is the same function, which the identity test below tells apart.
UE_FACT_FN(kRowThrowShit, "prop_throw", L"mainPlayer_C", L"throwShit", Declared);
constexpr int kTagThrow = 0x54485257;   // 'THRW'

struct Record {
    uint16_t              seq = 0;
    ue_wrap::CachedObjRef actor;
    uint64_t              startMs = 0;
    bool                  granted = false;
};
Record g_rec[tb::kMaxPendingThrows];
int    g_count = 0;
uint16_t g_seq = 0;

R::ParamOffset g_pinOff{L"InputPin"};

// The host's marks of thrown drop intents, by key. At most kMaxPendingThrows: a flood of throws past it is not elected.
struct KeyedMark { std::wstring key; Mark m; };
std::vector<KeyedMark> g_marks;
bool     g_watchRegistered = false;
bool     g_watchSettled = false;
bool     g_boundsSaid = false;
uint64_t g_nextWatchMs = 0;
uint32_t g_minted = 0, g_puppetIgnored = 0, g_noOwner = 0, g_evicted = 0, g_marksDropped = 0;

void Erase(int i) {
    for (int k = i; k + 1 < g_count; ++k) g_rec[k] = g_rec[k + 1];
    g_rec[--g_count] = Record{};
}

uint16_t NextSeq() {
    if (++g_seq == 0) ++g_seq;
    return g_seq;
}

void Mint(void* actor) {
    const uint64_t now = coop::active_drive::NowMs();
    for (int i = 0; i < g_count; ++i)
        if (g_rec[i].actor.Is(actor)) { Erase(i); break; }   // a re-throw: the newer record is the prop's
    if (g_count == tb::kMaxPendingThrows) {   // the oldest waits no longer
        Erase(0);
        if (++g_evicted == 1) UE_LOGW("prop_throw: %d throws await a grant -- the oldest record dropped", tb::kMaxPendingThrows);
    }
    Record& r = g_rec[g_count++];
    r.seq = NextSeq();
    r.actor.Set(actor);
    r.startMs = now;
    r.granted = false;
    ++g_minted;
    UE_LOGI("prop_throw: throw seq=%u actor=%p cls='%ls' (#%u)", static_cast<unsigned>(r.seq), actor,
            R::ClassNameOf(actor).c_str(), g_minted);
}

// POST: throwShit ran and set the launch velocity. The thrown actor is the owner of the component it was given.
void OnThrowShitPost(const SG::Call& c) {
    if (!coop::players::Registry::Get().IsLocal(c.object)) {
        if (++g_puppetIgnored == 1 || (g_puppetIgnored % 100) == 0)
            UE_LOGI("prop_throw: puppet throwShit ignored n=%u", g_puppetIgnored);
        return;
    }
    const int32_t off = g_pinOff.Of(c.function);
    void* comp = nullptr;
    if (off >= 0 && c.locals) std::memcpy(&comp, c.locals + off, sizeof(comp));
    void* actor = (comp && R::IsLive(comp)) ? R::OuterOf(comp) : nullptr;
    if (!actor || !R::IsLive(actor)) {
        if (++g_noOwner == 1) UE_LOGW("prop_throw: throwShit's component has no live owner -- no sequence minted");
        return;
    }
    Mint(actor);
}

// Register the watch, then ask the gate to resolve its name until it is live or settled dead: at most once a
// second, a refusal said once.
void EnsureWatch() {
    if (g_watchSettled) return;
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextWatchMs) return;
    g_nextWatchMs = now + 1000;
    if (!g_watchRegistered) g_watchRegistered = SG::WatchName(kRowThrowShit, kTagThrow, nullptr, &OnThrowShitPost);
    if (!g_watchRegistered) {
        // Refused while the gate is not armed yet: asked again. Refused by an armed gate (its name table is full): for good.
        if (SG::IsInstalled()) {
            g_watchSettled = true;
            UE_LOGE("prop_throw: the gate refused the throwShit watch -- no throw is seen, so none is elected");
        }
        return;
    }
    SG::ResolvePendingNames();
    if (SG::NameWatchLive(kRowThrowShit, kTagThrow)) {
        g_watchSettled = true;
        UE_LOGI("prop_throw: a throw is seen at the call (a script-gate watch on mainPlayer_C::throwShit)");
        if (!g_boundsSaid) {
            g_boundsSaid = true;
            UE_LOGI("prop_throw: bounds launch=%.0f cm/s flight=%.0f cm/s (longest flight %.3f s) "
                    "dropIntentAng=%.0f deg/s releaseAng=%.0f deg/s stepSlack=%.0f cm jitter=%u ms refusedRows=%d "
                    "grantWait=%u ms hostGrantFloor=%u ms flightBound=%u ms silence=%u ms pending=%d rowsPerSlot=%d",
                    tb::kMaxLaunchSpeedCmS, tb::kMaxFlightSpeedCmS, tb::kLongestFlightS,
                    tb::kMaxLaunchAngDegS, tb::kMaxBodyAngDegS, tb::kStepSlackCm, tb::kJitterMs, tb::kMaxRefusedRows, tb::kGrantTimeoutMs,
                    tb::kGrantTimeoutFloorMs, tb::kMaxThrowFlightMs, tb::kThrowSilenceMs, tb::kMaxPendingThrows,
                    tb::kMaxThrowRowsPerSlot);
        }
    } else if (SG::NameWatchSettled(kRowThrowShit, kTagThrow)) {
        g_watchSettled = true;
        UE_LOGE("prop_throw: the throwShit watch is dead -- no throw is seen, so none is elected");
    }
}

}  // namespace

void Tick() {
    EnsureWatch();
    if (g_count == 0 && g_marks.empty()) return;
    const uint64_t now = coop::active_drive::NowMs();
    for (int i = 0; i < g_count;) {
        // A record lives until its grant times out unmatched, or (granted) until its row ends on this peer (Ended).
        if ((!g_rec[i].granted && now - g_rec[i].startMs > tb::kGrantTimeoutMs) || !g_rec[i].actor.Alive()) Erase(i);
        else ++i;
    }
    for (size_t i = 0; i < g_marks.size();) {
        if (now - g_marks[i].m.ms > tb::kGrantTimeoutMs) g_marks.erase(g_marks.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
}

uint16_t SeqFor(void* actor) {
    for (int i = 0; i < g_count; ++i)
        if (g_rec[i].actor.Is(actor)) return g_rec[i].seq;
    return 0;
}

bool TakeForGrant(uint16_t seq, void** actor, uint64_t* startMs) {
    for (int i = 0; i < g_count; ++i) {
        Record& r = g_rec[i];
        if (r.seq != seq || r.granted || !r.actor.Alive()) continue;
        r.granted = true;   // kept until its row ends here (Ended): the adoption of this actor yields to it meanwhile
        if (actor) *actor = r.actor.Get();
        if (startMs) *startMs = r.startMs;
        return true;
    }
    return false;
}

void Ended(void* actor) {
    for (int i = 0; i < g_count; ++i)
        if (g_rec[i].granted && g_rec[i].actor.Is(actor)) { Erase(i); return; }
}

void NoteMark(const std::wstring& key, uint8_t slot, uint16_t seq, const float loc[3]) {
    for (size_t i = 0; i < g_marks.size(); ++i)
        if (g_marks[i].key == key) { g_marks.erase(g_marks.begin() + static_cast<std::ptrdiff_t>(i)); break; }
    if (g_marks.size() >= static_cast<size_t>(tb::kMaxPendingThrows)) {
        if (++g_marksDropped == 1) UE_LOGW("prop_throw: %d thrown drop intents await their spawn -- a further one is not elected", tb::kMaxPendingThrows);
        return;
    }
    KeyedMark km;
    km.key = key;
    km.m.slot = slot;
    km.m.seq = seq;
    km.m.loc[0] = loc[0]; km.m.loc[1] = loc[1]; km.m.loc[2] = loc[2];
    km.m.ms = coop::active_drive::NowMs();
    g_marks.push_back(std::move(km));
}

bool HasMarks() { return !g_marks.empty(); }

bool TakeMark(const std::wstring& key, Mark* out) {
    for (size_t i = 0; i < g_marks.size(); ++i) {
        if (g_marks[i].key != key) continue;
        if (out) *out = g_marks[i].m;
        g_marks.erase(g_marks.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    return false;
}

void OnDisconnect() {
    for (Record& r : g_rec) r = Record{};
    g_count = 0;
    g_marks.clear();
    g_minted = g_puppetIgnored = g_noOwner = g_evicted = g_marksDropped = 0;
}

namespace dev {
uint32_t Minted() { return g_minted; }
uint32_t PuppetIgnored() { return g_puppetIgnored; }
}  // namespace dev

}  // namespace coop::prop_throw
