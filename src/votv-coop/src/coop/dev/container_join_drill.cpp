// coop/dev/container_join_drill.cpp -- see coop/dev/container_join_drill.h.

#include "coop/dev/container_join_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/container_parity.h"
#include "coop/dev/container_poke.h"
#include "coop/element/bind_wait.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/container_contents_sync.h"
#include "coop/props/container_park.h"
#include "coop/session/join_progress.h"

#include "ue_wrap/core/log.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace coop::dev::container_join_drill {
namespace {

namespace CC  = coop::props::container_contents_sync;
namespace PAR = coop::dev::container_parity;
namespace BW  = coop::element::bind_wait;

constexpr int kSlots = coop::net::kMaxPeers;
constexpr uint16_t kContainerKind = static_cast<uint16_t>(coop::net::ReliableKind::ContainerContents);

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(coop::config_registry::rows::container_join_drill);
    return s;
}

// What the host did in one slot's join window.
struct Fire {
    bool fired = false;
    bool noReady = false;     // no world-ready peer when it fired: the edit had nobody to go to
    uint32_t eid = 0;         // the container it edited
    uint64_t refusals = 0;    // NoReadyRefusals() at the fire
    uint64_t retries = 0;     // NoReadyRetries() at the fire
};

// The pick walks the prop registry, so a pick that found nothing is tried again only every few ticks.
constexpr uint32_t kPickEveryTicks = 20;
enum class Verdict : uint8_t { None, Pass, Fail };

Fire g_fire[kSlots];
Verdict g_verdict[kSlots];
std::vector<PAR::Report> g_pending;   // taken off the door, judged once their slot is world-ready
bool g_parityOn = false;
bool g_done = false;
uint32_t g_tick = 0;
uint32_t g_nextPickTick = 0;

// The client's state.
bool g_leftIdle = false;
bool g_sent = false;

struct Pick {
    uint32_t eid = 0;
    bool found = false;
};

void PickFirst(uint32_t eid, void* ctx) {
    Pick* p = static_cast<Pick*>(ctx);
    if (p->found) return;
    int32_t records = -1;
    float vol = 0.f;
    if (CC::ContentsDigest(eid, records, vol) && records >= 1) { p->eid = eid; p->found = true; }
}

void CountOne(uint32_t, void* ctx) { ++*static_cast<size_t*>(ctx); }

int WorldReadyPeers(const coop::net::Session& s) {
    int n = 0;
    for (int slot = 1; slot < kSlots; ++slot)
        if (s.IsSlotWorldReady(slot)) ++n;
    return n;
}

// One attempt: one walk of the world containers. True when an edit was made (or failed) for the slot; false
// when there was nothing to edit, and the next attempt waits kPickEveryTicks.
bool FireInSlot(coop::net::Session& s, int slot) {
    Pick pick;
    CC::ForEachWorldContainer(&PickFirst, &pick);
    void* actor = pick.found ? coop::element::LivePropActor(pick.eid) : nullptr;
    if (!actor) {
        g_nextPickTick = g_tick + kPickEveryTicks;
        return false;
    }
    Fire& f = g_fire[slot];
    f.fired = true;
    f.eid = pick.eid;
    const int ready = WorldReadyPeers(s);
    f.noReady = (ready == 0);
    int32_t before = -1, after = -1;
    if (!coop::dev::container_poke::Extract(actor, before, after, pick.eid)) {
        UE_LOGW("[CONTAINER-JOIN] host FAIL: extract did not resolve on eid=%u", pick.eid);
        g_verdict[slot] = Verdict::Fail;
        return true;
    }
    f.refusals = CC::NoReadyRefusals();
    f.retries = CC::NoReadyRetries();
    UE_LOGI("[CONTAINER-JOIN] host fired extract on eid=%u records %d -> %d while slot %d is not world-ready "
            "(world-ready peers: %d)", pick.eid, before, after, slot, ready);
    UE_LOGI("[CONTAINER-JOIN] host fire counters: no-ready refusals=%llu retries=%llu",
            static_cast<unsigned long long>(f.refusals), static_cast<unsigned long long>(f.retries));
    if (before == after) {
        UE_LOGW("[CONTAINER-JOIN] host FAIL: the extract changed nothing (inert trigger)");
        g_verdict[slot] = Verdict::Fail;
    }
    return true;
}

void JudgeSlot(int slot, const PAR::Report& r) {
    if (g_verdict[slot] != Verdict::None) return;   // the fire itself already failed
    const Fire& f = g_fire[slot];
    if (!f.fired) {
        UE_LOGW("[CONTAINER-JOIN] host FAIL: slot %d reported, but no edit was fired in its join window", slot);
        g_verdict[slot] = Verdict::Fail;
        return;
    }
    bool ok = true;
    std::string firstFail;
    if (!PAR::JudgeReport(r, firstFail)) {
        UE_LOGW("[CONTAINER-JOIN] host FAIL: slot %d the parity report differs (first: %s)", slot, firstFail.c_str());
        ok = false;
    }
    if (f.noReady && !CC::NoReadyRefusedEid(f.eid)) {
        UE_LOGW("[CONTAINER-JOIN] host FAIL: the edited container eid=%u never met a no-ready refusal -- the window "
                "was not exercised", f.eid);
        ok = false;
    }
    // Nothing after the fire fans this container out before the judge: the joiner got it by the targeted replay.
    if (f.noReady && CC::SentBaselineKnown(f.eid)) {
        UE_LOGW("[CONTAINER-JOIN] host FAIL: eid=%u kept a sent baseline that no peer received", f.eid);
        ok = false;
    }
    if (CC::NoReadyRetries() != 0) {
        UE_LOGW("[CONTAINER-JOIN] host FAIL: %llu container sends were re-armed with no world-ready peer",
                static_cast<unsigned long long>(CC::NoReadyRetries()));
        ok = false;
    }
    g_verdict[slot] = ok ? Verdict::Pass : Verdict::Fail;
    if (ok) UE_LOGI("[CONTAINER-JOIN] host PASS slot %d", slot);
}

void MaybeDone(const coop::net::Session& s) {
    if (g_done) return;
    int passed = 0;
    for (int slot = 1; slot < kSlots; ++slot) {
        if (g_verdict[slot] == Verdict::Pass) { ++passed; continue; }
        if (g_fire[slot].fired || s.IsSlotConnected(slot)) return;   // a verdict is still owed, or it failed
    }
    if (passed == 0) return;
    g_done = true;
    UE_LOGI("[CONTAINER-JOIN] host DONE: %d slot(s) PASS", passed);
}

void HostTick(coop::net::Session& s) {
    if (!g_parityOn) { PAR::Enable(); g_parityOn = true; }
    PAR::Tick();
    ++g_tick;
    bool firedThisTick = false;   // one edit per tick: the next slot's pick sees the container as it now is
    for (int slot = 1; slot < kSlots; ++slot) {
        if (!s.IsSlotConnected(slot)) {   // a vacated slot starts over, and so do the reports it left queued
            g_fire[slot] = Fire{};
            g_verdict[slot] = Verdict::None;
            g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(),
                                           [slot](const PAR::Report& p) { return p.slot == slot; }),
                            g_pending.end());
            continue;
        }
        if (firedThisTick || g_fire[slot].fired || s.IsSlotWorldReady(slot) || g_tick < g_nextPickTick) continue;
        firedThisTick = FireInSlot(s, slot);
    }
    PAR::Report r;
    while (PAR::TakeReport(r)) g_pending.push_back(std::move(r));
    bool judged = false;
    for (size_t i = 0; i < g_pending.size();) {
        const int slot = g_pending[i].slot;
        if (slot < 1 || slot >= kSlots) {
            UE_LOGW("[CONTAINER-JOIN] host FAIL: a parity report came from slot %d", slot);
            g_pending.erase(g_pending.begin() + i);
            continue;
        }
        if (!s.IsSlotWorldReady(slot)) { ++i; continue; }   // judged once the slot is world-ready
        JudgeSlot(slot, g_pending[i]);
        g_pending.erase(g_pending.begin() + i);
        judged = true;
    }
    if (judged) MaybeDone(s);
}

void ClientTick(coop::net::Session& s) {
    if (g_sent) return;
    const bool idle = coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
    if (!g_leftIdle) {
        if (!idle) g_leftIdle = true;
        return;
    }
    if (!idle) return;
    if (coop::props::container_park::Size() != 0) return;
    if (BW::HeldOf(kContainerKind) != BW::ReplayedOf(kContainerKind) + BW::DroppedOf(kContainerKind) +
                                          BW::GivenUpOf(kContainerKind)) return;
    g_sent = true;
    size_t n = 0;
    CC::ForEachWorldContainer(&CountOne, &n);
    if (PAR::SendReport(s))
        UE_LOGI("[CONTAINER-JOIN] client sent parity n=%zu", n);
    else
        UE_LOGW("[CONTAINER-JOIN] client FAIL: the parity report was not sent");
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!Enabled() || !session) return;
    if (session->role() == coop::net::Role::Host) HostTick(*session);
    else if (session->role() == coop::net::Role::Client) ClientTick(*session);
}

void OnDisconnect() {
    if (!Enabled()) return;
    for (int slot = 0; slot < kSlots; ++slot) {
        g_fire[slot] = Fire{};
        g_verdict[slot] = Verdict::None;
    }
    g_pending.clear();
    g_tick = 0;
    g_nextPickTick = 0;
    g_done = false;
    g_leftIdle = false;
    g_sent = false;
    PAR::OnDisconnect();
}

}  // namespace coop::dev::container_join_drill
