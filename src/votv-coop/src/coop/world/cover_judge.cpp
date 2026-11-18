// coop/world/cover_judge.cpp -- see coop/world/cover_judge.h.
//
// The host's half of the cover lane: a client's ask judged on the host's own desk or station, the change run on the
// host's copy, and the desk's plates and each station's door sent when they change and to a joiner. The desk's and the
// station's layout is in ue_wrap/desk/analog_desk and ue_wrap/devices/generator; the lane's shared helpers are in
// coop/world/cover_shared.h.

#include "coop/world/cover_judge.h"

#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
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
using CS::kAskEveryMs;
using CS::kScanEveryMs;

constexpr uint8_t  kFixedPlate = AD::kFixedPlate;
constexpr float    kDeskReachCm = 400.f;  // a client's ask, from its eye to the plate
constexpr float    kPlateNearCm = 300.f;  // a plate set back, from the prop to its place
constexpr float    kDoorReachCm = 600.f;  // a client's door press, from its eye to the station
constexpr uint64_t kBodyWaitMs = 5000;    // an ask waits this long for its sender's body here

struct Ask { int slot; N::CoverPayload p; uint64_t since; };
std::vector<Ask> g_asks;
uint64_t g_lastAskMs[N::kMaxPeers] = {};
uint64_t g_nextScanMs = 0;
bool g_deskSaid = false;
uint8_t g_deskSent = 0;
std::vector<int8_t> g_doorSent;  // by station: -1 not said, else opened

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
// A granted take-off's prop, born on its lid, moves from the plate to its asker's hand over the next frames and can
// brush the lid again on the way. As the desk's own panelTakeoff waits for the prop's end-overlap, the host's desk does
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

// HOST, each tick: a fresh prop seen off its lid is armed, and the hold ends with the last one, as the end-overlap ends
// panelTakeoff.
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
        // Off its lid in a player's hold, as the game's take-off hands it straight to its taker: until that hold arrives
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
        Answer(o.slot, N::kCoverDesk, o.plate, eid == EL::kInvalidId ? N::CoverReason::Busy : N::CoverReason::Ok,
               eid == EL::kInvalidId ? 0u : static_cast<uint32_t>(eid));
        UE_LOGI("cover_sync: HOST slot %d's plate %u is prop eid %u -- answered", o.slot, o.plate,
                eid == EL::kInvalidId ? 0u : static_cast<uint32_t>(eid));
        g_owed.erase(g_owed.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

// A take-off: the plate off on the host's desk, and its prop the host's, spawned where the plate was, so every peer
// has it. The asker takes it into its hand when its copy arrives.
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
    const bool reach = seen && Dist2(eye, plateAt) <= kDeskReachCm * kDeskReachCm;
    if (!reach)
        UE_LOGI("cover_sync: HOST slot %d's eye (%.0f,%.0f,%.0f) is %.0f cm from plate %u at (%.0f,%.0f,%.0f)", a.slot,
                eye.X, eye.Y, eye.Z, seen ? std::sqrt(Dist2(eye, plateAt)) : -1.f, plate, plateAt.X, plateAt.Y, plateAt.Z);
    const bool busy = AD::ProcessOr(desk, false);
    if (a.p.op == N::kCoverOpUnscrew) {
        // The ask carries the state the sender wants (the game's verb is a toggle): a plate already so is left, so two
        // asks or a stale one cannot invert each other.
        const bool want = (a.p.bits & 1u) != 0;
        const bool already = PlateLoose(bits, plate) == want;
        const bool ok = reach && plate != kFixedPlate && PlateOn(bits, plate) && !busy && !already;
        if (ok) {
            const uint8_t bolt = static_cast<uint8_t>(1u << (4 + plate));
            WritePlates(desk, static_cast<uint8_t>(want ? (bits | bolt) : (bits & ~bolt)));
        } else if (reach && already) {
            Answer(a.slot, N::kCoverDesk, plate, N::CoverReason::Already);
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
        ue_wrap::FVector at{};
        const bool near = prop && mesh && E::TryGetActorLocation(prop, at) &&
                          Dist2(at, E::GetComponentLocation(mesh)) <= kPlateNearCm * kPlateNearCm;
        const bool fresh = IsFresh(prop);  // not yet off its lid in a hold on the host
        const bool ok = near && !fresh && AD::PlateOfProp(prop) == plate && !PlateOn(bits, plate);
        if (ok) {
            WritePlates(desk, static_cast<uint8_t>(bits | (1u << plate)));
            CC::CallParamlessNamed(prop, L"K2_DestroyActor");
            SoundDetach(desk, at);
        }
        UE_LOGI("cover_sync: HOST slot %d sets plate %u back (prop eid %u) -- %hs", a.slot, plate, a.p.eid,
                ok ? "taken" : PlateOn(bits, plate) ? "already on" : fresh ? "refused, not yet off its lid" : "refused");
    }
    g_nextScanMs = 0;  // the change goes out at once
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
    const bool ok = reach && !MovingOr(gen, true) && OpenedOr(gen, !want) != want;
    if (ok) OpenDoor(gen, want);
    UE_LOGI("cover_sync: HOST slot %d %hs station %u's door -- %hs", a.slot, want ? "opens" : "shuts", a.p.index,
            ok ? "taken" : reach ? "refused by the station" : "out of reach, refused");
}

}  // namespace

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
    const uint64_t now = NowMs();
    if (now < g_nextScanMs) return;
    g_nextScanMs = now + kScanEveryMs;
    uint8_t bits = 0;
    if (void* desk = AD::Find(); desk && AD::ReadPlates(desk, bits) && (!g_deskSaid || bits != g_deskSent)) {
        if (g_deskSaid) SendDesk(bits, -1);
        if (g_deskSaid) UE_LOGI("cover_sync: HOST the desk's plates on=0x%x unbolted=0x%x -- sent", bits & 0xF, bits >> 4);
        g_deskSaid = true;
        g_deskSent = bits;
    }
    std::vector<void*> gens;
    GEN::ReadGenerators(gens);
    if (g_doorSent.size() < gens.size()) g_doorSent.resize(gens.size(), -1);
    for (size_t i = 0; i < gens.size(); ++i) {
        bool open = false;
        if (!gens[i] || !GEN::ReadOpened(gens[i], open)) continue;
        if (g_doorSent[i] == (open ? 1 : 0)) continue;
        if (g_doorSent[i] >= 0) {
            SendDoor(static_cast<int>(i), open, -1);
            UE_LOGI("cover_sync: HOST station %zu's door %hs -- sent", i, open ? "opened" : "shut");
        }
        g_doorSent[i] = open ? 1 : 0;
    }
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
    if (!CS::IsHost() || senderSlot <= 0 || senderSlot >= static_cast<int>(N::kMaxPeers) || g_asks.size() >= 64) return;
    const bool desk = p.what == N::kCoverDesk &&
                      (p.op == N::kCoverOpUnscrew || p.op == N::kCoverOpTakeOff || p.op == N::kCoverOpPutBack) &&
                      p.index < N::kCoverDeskPanels;
    const bool door = p.what == N::kCoverStation && p.op == N::kCoverOpDoor;
    if (!desk && !door) return;
    const uint64_t now = NowMs();
    if (now - g_lastAskMs[senderSlot] < kAskEveryMs) return;
    g_lastAskMs[senderSlot] = now;
    g_asks.push_back({senderSlot, p, now});
}

void OnDisconnect() {
    g_asks.clear();
    g_owed.clear();
    g_fresh.clear();
    for (uint64_t& t : g_lastAskMs) t = 0;
    g_deskSaid = false;
    g_doorSent.clear();
}

}  // namespace coop::cover_judge
