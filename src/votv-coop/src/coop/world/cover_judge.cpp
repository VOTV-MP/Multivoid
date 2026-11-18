// coop/world/cover_judge.cpp -- see coop/world/cover_judge.h.
//
// The host's half of the cover lane: a client's ask judged on the host's own desk or station, the change run on the
// host's copy, and the desk's plates and each station's door sent when they change and to a joiner. The desk's and the
// station's layout is in ue_wrap/desk/analog_desk and ue_wrap/devices/generator; the lane's shared helpers are in
// coop/world/cover_shared.h.

#include "coop/world/cover_judge.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/intent_authority.h"
#include "coop/element/registry.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_hold.h"
#include "coop/world/cover_shared.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/analog_desk.h"
#include "ue_wrap/devices/generator.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"

#include <cmath>
#include <vector>

namespace coop::cover_judge {
namespace {

namespace R   = ue_wrap::reflection;
namespace E   = ue_wrap::engine;
namespace CC  = ue_wrap::component_calls;
namespace GEN = ue_wrap::generator;
namespace AD  = ue_wrap::analog_desk;
namespace EL  = coop::element;
namespace N   = coop::net;
namespace CS  = coop::cover_sync;

using CS::Dist2;
using CS::NowMs;
using CS::OpenDoor;
using CS::OpenedOr;
using CS::MovingOr;
using CS::PlateLoose;
using CS::PlateOn;
using CS::SoundDetach;
using CS::WritePlates;
using CS::AwayFromPlate;

constexpr uint8_t  kFixedPlate = AD::kFixedPlate;
constexpr float    kDeskReachCm = 400.f;  // a client's ask, from its eye to the plate
constexpr float    kDoorReachCm = 600.f;  // a client's door press, from its eye to the station
constexpr uint64_t kBodyWaitMs = 5000;    // an ask waits this long for its sender's body here
constexpr size_t   kAskQueueMax = 64;     // asks waiting, over all senders

struct Ask { int slot; N::CoverPayload p; uint64_t since; };
std::vector<Ask> g_asks;
// A sender's budget of asks: four at once, two a second. Past it an ask is dropped and its sender told, once in
// kRateSayMs, that it asked too fast. Both clear with the slot's occupant.
constexpr N::IntentBudget kAskBudget{4.f, 2.f};
constexpr uint64_t kRateSayMs = 10000;
coop::roster_ledger::PerSlotState<N::IntentBucket> g_bucket;
coop::roster_ledger::PerSlotState<uint64_t> g_nextRateSayMs;
bool g_deskSaid = false;
uint8_t g_deskSent = 0;

void Send(const N::CoverPayload& p, int slot) {
    if (slot >= 0) CS::Session()->SendReliableToSlot(slot, N::ReliableKind::CoverState, &p, sizeof(p));
    else CS::Session()->SendReliable(N::ReliableKind::CoverState, &p, sizeof(p));
}

void SendDesk(uint8_t bits, int slot) {
    N::CoverPayload p{};
    p.op = N::kCoverOpState;
    p.what = N::kCoverDesk;
    p.bits = bits;
    Send(p, slot);
}

void SendDoor(int index, bool open, int slot) {
    N::CoverPayload p{};
    p.op = N::kCoverOpState;
    p.what = N::kCoverStation;
    p.index = static_cast<uint8_t>(index);
    p.bits = open ? 1 : 0;
    Send(p, slot);
}

// An ask's answer to its sender: the verdict in `bits`, and a granted take-off's prop in `eid`.
void Answer(int slot, uint8_t what, uint8_t index, N::CoverReason why, uint32_t eid = 0) {
    N::CoverPayload p{};
    p.op = N::kCoverOpAnswer;
    p.what = what;
    p.index = index;
    p.bits = static_cast<uint8_t>(why);
    p.eid = eid;
    Send(p, slot);
}

bool EyeOf(int slot, ue_wrap::FVector& eye) {
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(static_cast<uint8_t>(slot));
    return rp.valid() && rp.TryGetEyePosition(eye);
}

// A granted take-off's answer waits for its prop's element id, which the spawn seam gives it after the spawn returns.
struct Owed {
    int slot = -1;
    uint8_t plate = 0;
    ue_wrap::CachedObjRef prop;
    uint64_t since = 0;
};
std::vector<Owed> g_owed;
// A granted take-off's prop, born on its lid, falls off it by its own physics and can brush the lid again until its asker
// picks it up by hand, as any prop. As the desk's own panelTakeoff waits for the prop's end-overlap, the host's desk does
// not set that prop back until it has been seen off its lid in a hold: panelTakeoff is held while it is fresh, and the
// desk's set-back is refused for it at the gate, since an end-overlap of another actor clears the flag between ticks.
struct Fresh {
    ue_wrap::CachedObjRef prop;
    uint8_t plate = 0;
    uint64_t since = 0;
    bool touched = false;  // seen on its lid: a first read before the spawn's overlaps register does not arm it
};
std::vector<Fresh> g_fresh;
constexpr uint64_t kFreshMaxMs = 60000;  // a prop left on its lid this long is the desk's again
constexpr uint64_t kUnheldArmMs = 3000;  // a prop no one has taken by this is armed once off its lid

// HOST, each tick, only while a granted take-off's prop is fresh: a fresh prop seen off its lid is armed, and the hold ends
// with the last one, as the end-overlap ends panelTakeoff. This stays on a clock because the prop's leaving its lid is
// physics motion read against its lid (the desk's end-overlap is any actor's and clears the same flag, so it does not say
// this prop left; the end-overlap was not measured), a player's hold has no announcement, and a prop nobody takes is armed
// by time alone.
void TendFresh() {
    if (g_fresh.empty()) return;
    void* const desk = AD::Find();
    const uint64_t now = NowMs();
    for (size_t i = 0; i < g_fresh.size();) {
        Fresh& f = g_fresh[i];
        void* const prop = f.prop.Get();
        void* const lid = desk ? AD::Lid(desk, f.plate) : nullptr;
        const bool gone = !prop || !lid || now - f.since > kFreshMaxMs;
        const bool on = !gone && E::IsOverlappingActor(lid, prop);
        if (on) f.touched = true;
        // Off its lid in a player's hold: until that hold arrives
        // the host's copy falls off the lid by its own physics, and the hold lifts it back through it. A prop nobody
        // takes is armed off its lid after kUnheldArmMs.
        const bool held = !gone && coop::prop_hold::WhoHolds(prop, 0) != coop::prop_hold::Holder::None;
        const bool away = !gone && AwayFromPlate(desk, f.plate, prop);
        if (gone || (!on && (f.touched || now - f.since > 1000) && ((held && away) || now - f.since > kUnheldArmMs))) {
            if (!gone) {
                ue_wrap::FVector pa{}, la{};
                E::TryGetActorLocation(prop, pa);
                E::TryGetComponentLocation(lid, la);
                UE_LOGI("cover_sync: HOST plate %u's prop has left its lid -- it can be set back (prop (%.0f,%.0f,%.0f), "
                        "lid (%.0f,%.0f,%.0f), %.0f cm)", f.plate, pa.X, pa.Y, pa.Z, la.X, la.Y, la.Z, std::sqrt(Dist2(pa, la)));
            }
            g_fresh.erase(g_fresh.begin() + static_cast<std::ptrdiff_t>(i));
            if (g_fresh.empty() && desk) AD::WriteTakeoff(desk, false);
            continue;
        }
        ++i;
    }
    if (!g_fresh.empty() && desk) AD::WriteTakeoff(desk, true);
}

// Each tick while a take-off's answer is owed: the prop's element id is given by the spawn seam after the spawn returns,
// and nothing else announces it here, so it is read until it binds or the prop is gone (2 s).
void PayOwed() {
    const uint64_t now = NowMs();
    for (size_t i = 0; i < g_owed.size();) {
        Owed& o = g_owed[i];
        void* const prop = o.prop.Get();
        const EL::ElementId eid = prop ? EL::Registry::Get().EidForActor(prop) : EL::kInvalidId;
        if (eid == EL::kInvalidId && prop && now - o.since < 2000) {
            ++i;
            continue;
        }
        // The plate did come off: the asker is told so (Ok); a prop whose id never bound is said in the log, not as a refusal.
        Answer(o.slot, N::kCoverDesk, o.plate, N::CoverReason::Ok, eid == EL::kInvalidId ? 0u : static_cast<uint32_t>(eid));
        UE_LOGI("cover_sync: HOST slot %d's plate %u is prop eid %u -- answered%hs", o.slot, o.plate,
                eid == EL::kInvalidId ? 0u : static_cast<uint32_t>(eid), eid == EL::kInvalidId ? " (no element id bound)" : "");
        g_owed.erase(g_owed.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

// A take-off: the plate off on the host's desk, and its prop the host's, spawned where the plate was, so every peer
// has it. The asker picks it up by hand, as any prop; no hold is handed to it.
void* TakeOff(void* desk, uint8_t plate, uint8_t bits) {
    void* const mesh = AD::PlateMesh(desk, plate);
    void* const cls = R::FindClass(AD::PlatePropClass(plate));
    if (!mesh || !cls) {
        UE_LOGW("cover_sync: HOST plate %u's mesh or prop class (%ls) is not here", plate, AD::PlatePropClass(plate));
        return nullptr;
    }
    const ue_wrap::FVector at = E::GetComponentLocation(mesh);
    const ue_wrap::FRotator rot = E::GetComponentWorldRotation(mesh);
    AD::WriteTakeoff(desk, true);  // the prop born touching its lid is not set back at once
    // The plates' state goes out BEFORE the prop is spawned (the updPanels the write runs announces it, OnDeskUpdated), so
    // a client has the plate off when the prop's mirror reaches its lid: a mirror born on a lid whose plate is still on
    // would read as a plate set back.
    WritePlates(desk, static_cast<uint8_t>(bits & ~(1u << plate)));
    void* const prop = E::BeginDeferredSpawn(cls, at, rot);
    if (!prop || !E::FinishDeferredSpawn(prop, at, rot)) {  // no plate made: it stays on
        AD::WriteTakeoff(desk, false);
        WritePlates(desk, bits);
        UE_LOGW("cover_sync: HOST plate %u's prop did not spawn -- the plate stays on", plate);
        return nullptr;
    }
    SoundDetach(desk, at);
    return prop;
}

void JudgeDesk(const Ask& a) {
    void* const desk = AD::Find();
    uint8_t bits = 0;
    const uint8_t plate = a.p.index;
    if (!desk || !AD::ReadPlates(desk, bits) || plate >= N::kCoverDeskPanels) {
        UE_LOGW("cover_sync: HOST slot %d asked of plate %u with no desk here -- refused", a.slot, plate);
        Answer(a.slot, N::kCoverDesk, plate, N::CoverReason::NoStation);
        return;
    }
    ue_wrap::FVector eye{};
    void* const mesh = AD::PlateMesh(desk, plate);
    const bool seen = mesh && EyeOf(a.slot, eye);
    const ue_wrap::FVector plateAt = mesh ? E::GetComponentLocation(mesh) : ue_wrap::FVector{};
    // The drill's red (cover_judge_off): the ask is applied wherever its sender stands.
    static const bool s_judgeOff = coop::config::ResolveFlag(coop::config_registry::rows::cover_judge_off);
    const bool reach = s_judgeOff || (seen && Dist2(eye, plateAt) <= kDeskReachCm * kDeskReachCm);
    if (!reach)
        UE_LOGI("cover_sync: HOST slot %d's eye (%.0f,%.0f,%.0f) is %.0f cm from plate %u at (%.0f,%.0f,%.0f)", a.slot,
                eye.X, eye.Y, eye.Z, seen ? std::sqrt(Dist2(eye, plateAt)) : -1.f, plate, plateAt.X, plateAt.Y, plateAt.Z);
    const bool busy = AD::ProcessOr(desk, false);
    if (a.p.op == N::kCoverOpUnscrew) {
        // The ask carries the state the sender wants (the game's verb is a toggle): a plate already so is left, so two
        // asks or a stale one cannot invert each other.
        // The drill's red (cover_wanted_off): the ask toggles the plate whatever state it carries.
        static const bool s_wantedOff = coop::config::ResolveFlag(coop::config_registry::rows::cover_wanted_off);
        const bool want = s_wantedOff ? !PlateLoose(bits, plate) : (a.p.bits & 1u) != 0;
        const bool already = PlateLoose(bits, plate) == want;
        const bool ok = reach && plate != kFixedPlate && PlateOn(bits, plate) && !busy && !already;
        if (ok) {
            const uint8_t bolt = static_cast<uint8_t>(1u << (4 + plate));
            WritePlates(desk, static_cast<uint8_t>(want ? (bits | bolt) : (bits & ~bolt)));
        } else {
            Answer(a.slot, N::kCoverDesk, plate,
                   !reach ? N::CoverReason::OutOfReach : already ? N::CoverReason::Already : N::CoverReason::Busy);
        }
        UE_LOGI("cover_sync: HOST slot %d %hs plate %u -- %hs", a.slot, want ? "unbolts" : "bolts", plate,
                ok ? "taken" : !reach ? "out of reach, refused" : already ? "already so, left" : "refused by the desk");
    } else if (a.p.op == N::kCoverOpTakeOff) {
        const bool ok = reach && PlateLoose(bits, plate) && PlateOn(bits, plate) && !busy;
        void* const prop = ok ? TakeOff(desk, plate, bits) : nullptr;
        if (prop) {
            g_owed.emplace_back();
            g_owed.back().slot = a.slot;
            g_owed.back().plate = plate;
            g_owed.back().prop.Set(prop);
            g_owed.back().since = NowMs();
            g_fresh.emplace_back();
            g_fresh.back().prop.Set(prop);
            g_fresh.back().plate = plate;
            g_fresh.back().since = NowMs();
        } else {
            Answer(a.slot, N::kCoverDesk, plate, reach ? N::CoverReason::Busy : N::CoverReason::OutOfReach);
        }
        UE_LOGI("cover_sync: HOST slot %d takes plate %u off -- %hs", a.slot, plate,
                prop ? "taken" : ok ? "the plate's prop was not made" : reach ? "refused by the desk" : "out of reach, refused");
    } else {  // a plate set back
        void* const prop = EL::LiveActorOfType(static_cast<EL::ElementId>(a.p.eid), EL::ElementType::Prop);
        // The prop is a host element: the sender must reach it, hold it or leave it unheld, and the host's copy must touch
        // the plate's lid, as the desk's own overlap would test it.
        const bool mine = prop && AD::PlateOfProp(prop) == plate;
        const bool reached =
            mine && EL::IntentTarget::ForClientIntent(*CS::Session(), static_cast<uint8_t>(a.slot), kDeskReachCm)
                        .Authorize(prop).outcome == EL::IntentOutcome::Ok;
        // The drill's red (cover_hold_judge_off): a prop nobody carried back is set back, whoever holds it, wherever it is.
        static const bool s_holdOff = coop::config::ResolveFlag(coop::config_registry::rows::cover_hold_judge_off);
        const bool heldOk =
            mine && (s_holdOff || coop::prop_hold::WhoHolds(prop, a.slot) == coop::prop_hold::Holder::None);
        const bool touches = mine && (s_holdOff || E::IsOverlappingActor(AD::Lid(desk, plate), prop));
        const bool fresh = !s_holdOff && IsFresh(prop);  // not yet off its lid in a hold on the host
        const bool on = PlateOn(bits, plate);
        const bool ok = mine && reached && heldOk && touches && !fresh && !on;
        N::CoverReason why = N::CoverReason::Busy;
        const char* said = fresh ? "refused, not yet off its lid" : "refused";
        if (ok) {
            ue_wrap::FVector at{};
            E::TryGetActorLocation(prop, at);
            WritePlates(desk, static_cast<uint8_t>(bits | (1u << plate)));
            CC::CallParamlessNamed(prop, L"K2_DestroyActor");
            SoundDetach(desk, at);
            said = "taken";
        } else if (on) {
            why = N::CoverReason::Already, said = "already on";
        } else if (!mine) {
            said = "refused, not this plate's prop";
        } else if (!reached) {
            why = N::CoverReason::OutOfReach, said = "refused, the prop is out of reach";
        } else if (!heldOk) {
            why = N::CoverReason::NotHeld, said = "refused, another player holds it";
        } else if (!touches) {
            why = N::CoverReason::OutOfReach, said = "refused, the prop is not on the lid";
        }
        if (!ok) Answer(a.slot, N::kCoverDesk, plate, why);
        UE_LOGI("cover_sync: HOST slot %d sets plate %u back (prop eid %u) -- %hs", a.slot, plate, a.p.eid, said);
    }
}

void JudgeDoor(const Ask& a) {
    void* const gen = GEN::At(a.p.index);
    ue_wrap::FVector eye{}, at{};
    const bool seen = gen && EyeOf(a.slot, eye) && E::TryGetActorLocation(gen, at);
    const bool reach = seen && Dist2(eye, at) <= kDoorReachCm * kDoorReachCm;
    if (!reach)
        UE_LOGI("cover_sync: HOST slot %d's eye (%.0f,%.0f,%.0f) is %.0f cm from station %u at (%.0f,%.0f,%.0f)", a.slot,
                eye.X, eye.Y, eye.Z, seen ? std::sqrt(Dist2(eye, at)) : -1.f, a.p.index, at.X, at.Y, at.Z);
    const bool want = (a.p.bits & 1u) != 0;
    const bool moving = MovingOr(gen, true);
    const bool already = OpenedOr(gen, !want) == want;
    const bool ok = reach && !moving && !already;
    if (ok) OpenDoor(gen, want);
    else Answer(a.slot, N::kCoverStation, a.p.index,
                !gen ? N::CoverReason::NoStation : !reach ? N::CoverReason::OutOfReach
                : already ? N::CoverReason::Already : N::CoverReason::Busy);
    UE_LOGI("cover_sync: HOST slot %d %hs station %u's door -- %hs", a.slot, want ? "opens" : "shuts", a.p.index,
            ok ? "taken" : reach ? "refused by the station" : "out of reach, refused");
}

}  // namespace

// The plates and the doors are sent by the game's own announcements (OnDeskUpdated, OnDoorFinished); this is what remains on
// a tick: the queued asks, and the two waits above.
void Tick() {
    PayOwed();
    TendFresh();
    std::vector<Ask> asks;
    asks.swap(g_asks);
    for (const Ask& a : asks) {
        ue_wrap::FVector eye{};
        if (!EyeOf(a.slot, eye) && NowMs() - a.since < kBodyWaitMs) {  // a joiner's first pose follows its ready edge
            g_asks.push_back(a);
            continue;
        }
        if (a.p.what == N::kCoverDesk) JudgeDesk(a);
        else JudgeDoor(a);
    }
}

void OnDoorFinished(void* gen) {
    if (!CS::IsHost() || !CS::Session()->connected()) return;
    const int32_t index = GEN::IndexOf(gen);
    bool open = false;
    if (index < 0 || index > 255 || !GEN::ReadOpened(gen, open)) {
        UE_LOGW("cover_sync: HOST a station's door finished but its place (%d) or its state did not read -- not sent", index);
        return;
    }
    SendDoor(index, open, -1);
    UE_LOGI("cover_sync: HOST station %d's door %hs -- sent", index, open ? "opened" : "shut");
}

void OnDeskUpdated(void* desk) {
    if (!CS::IsHost() || !CS::Session()->connected()) return;
    // The drill's red (cover_announce_off): the host's own change is never sent, so the unbolt it judged stays its own.
    static const bool s_announceOff = coop::config::ResolveFlag(coop::config_registry::rows::cover_announce_off);
    uint8_t bits = 0;
    if (s_announceOff || !desk || !AD::ReadPlates(desk, bits) || (g_deskSaid && bits == g_deskSent)) return;
    SendDesk(bits, -1);
    g_deskSaid = true;
    g_deskSent = bits;
    UE_LOGI("cover_sync: HOST the desk's plates on=0x%x unbolted=0x%x -- sent", bits & 0xF, bits >> 4);
}

bool IsFresh(void* actor) {
    for (const Fresh& f : g_fresh)
        if (actor && f.prop.Is(actor)) return true;
    return false;
}

bool AnyFresh() { return !g_fresh.empty(); }

void SendJoinSnapshotForSlot(int slot) {
    if (!CS::IsHost() || slot <= 0) return;
    uint8_t bits = 0;
    if (void* desk = AD::Find(); desk && AD::ReadPlates(desk, bits)) SendDesk(bits, slot);
    std::vector<void*> gens;
    GEN::ReadGenerators(gens);
    for (size_t i = 0; i < gens.size() && i < 256; ++i) {
        bool open = false;
        if (gens[i] && GEN::ReadOpened(gens[i], open)) SendDoor(static_cast<int>(i), open, slot);
    }
}

void OnIntent(int senderSlot, const coop::net::CoverPayload& p) {
    if (!CS::IsHost() || senderSlot <= 0 || senderSlot >= static_cast<int>(N::kMaxPeers)) return;
    const bool desk = p.what == N::kCoverDesk &&
                      (p.op == N::kCoverOpUnscrew || p.op == N::kCoverOpTakeOff || p.op == N::kCoverOpPutBack) &&
                      p.index < N::kCoverDeskPanels;
    const bool door = p.what == N::kCoverStation && p.op == N::kCoverOpDoor;
    if (!desk && !door) return;
    const uint64_t now = NowMs();
    if (g_asks.size() >= kAskQueueMax || !g_bucket[senderSlot].TakeN(kAskBudget, now, 1.0f)) {
        if (now < g_nextRateSayMs[senderSlot]) return;
        g_nextRateSayMs[senderSlot] = now + kRateSayMs;
        UE_LOGI("cover_sync: HOST slot %d asks faster than it is answered; its asks are dropped", senderSlot);
        Answer(senderSlot, p.what, p.index, N::CoverReason::Rate);
        return;
    }
    g_asks.push_back({senderSlot, p, now});
}

// A slot that left or was replaced is owed no answer, and its queued asks are not judged for its successor.
static void OnSlotReplaced(int slot, const coop::roster_ledger::Row&, const coop::roster_ledger::Row&) {
    for (size_t i = 0; i < g_asks.size();) {
        if (g_asks[i].slot == slot) g_asks.erase(g_asks.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
    for (size_t i = 0; i < g_owed.size();) {
        if (g_owed[i].slot == slot) g_owed.erase(g_owed.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
}

void Install() { coop::roster_ledger::SubscribeSlotReplaced(&OnSlotReplaced); }

void OnDisconnect() {
    g_asks.clear();
    g_owed.clear();
    // The hold TendFresh keeps on the desk's set-back ends with the records, as it ends with the last fresh prop.
    if (!g_fresh.empty())
        if (void* const desk = AD::Find()) AD::WriteTakeoff(desk, false);
    g_fresh.clear();
    g_deskSaid = false;
}

}  // namespace coop::cover_judge
