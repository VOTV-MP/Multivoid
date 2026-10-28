// coop/creatures/kerfus_state.cpp -- see coop/creatures/kerfus_state.h.

#include "coop/creatures/kerfus_state.h"

#include "coop/element/bind_wait.h"
#include "coop/element/element.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/prop.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/active_drive.h"
#include "coop/props/prop_drive_host.h"

#include "ue_wrap/actors/kerfus.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <unordered_map>

namespace coop::kerfus_state {
namespace {

namespace EL = coop::element;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace KF = coop::net::kerfus_state_flags;
namespace UK = ue_wrap::kerfus;

UE_FACT_FN(kRowReceiveTick, "kerfus_state", L"p_kerfus_C", L"ReceiveTick", Declared);

constexpr int kTagTick = 0x4B465401;  // 'KFT' 1

// Energy alone is sent on a step of half a unit, at most once a second: the look-at shows it, and it
// drains a unit in 30 s and charges one in 5 s.
constexpr float    kEnergyStep     = 0.5f;
constexpr uint64_t kEnergyPeriodMs = 1000;
constexpr uint16_t kStateKind = static_cast<uint16_t>(coop::net::ReliableKind::KerfusState);

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched = false;
bool g_saidLive = false;

// The three fields as the wire carries them; false while they do not resolve.
bool Snapshot(void* k, EL::ElementId eid, coop::net::KerfusStatePayload& p) {
    bool active = false, charging = false;
    float energy = 0.f;
    if (!UK::ReadActive(k, active) || !UK::ReadCharging(k, charging) || !UK::ReadEnergy(k, energy)) return false;
    p = coop::net::KerfusStatePayload{};
    p.elementId = static_cast<uint32_t>(eid);
    p.flags = static_cast<uint8_t>((active ? KF::kActive : 0) | (charging ? KF::kCharging : 0));
    p.energy = energy;
    return true;
}

// ---- host ----------------------------------------------------------------------------------------

// What the host last sent for a Kerfus, and whether it holds it under the drive. Keyed on the actor,
// its slot index re-checked, since a world change can hand a new Kerfus the same address.
struct HostRec {
    int32_t  idx = -1;
    EL::ElementId eid = EL::kInvalidId;  // its prop eid, fixed for the actor's life once enrolled
    bool     sent = false;
    uint8_t  flags = 0;
    float    energy = 0.f;
    uint64_t sentMs = 0;
    bool     driven = false;
};
std::unordered_map<void*, HostRec> g_host;

void OnTickPost(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != coop::net::Role::Host || !c.object) return;
    void* k = c.object;
    coop::net::KerfusStatePayload now{};
    const int32_t idx = R::InternalIndexOf(k);
    HostRec& r = g_host[k];
    if (r.idx != idx) r = HostRec{idx};
    if (r.eid == EL::kInvalidId) r.eid = EL::Registry::Get().EidForActor(k);  // once, then per record
    if (!Snapshot(k, r.eid, now)) return;
    // The drive: held every pass while on (a hand that took it and let go is claimed again), let go
    // on the off edge.
    if (now.flags & KF::kActive) {
        coop::prop_drive_host::Claim(k, "kerfus");
        r.driven = true;
    } else if (r.driven) {
        coop::prop_drive_host::Release(k);
        r.driven = false;
    }
    const uint64_t ms = coop::active_drive::NowMs();
    const bool edge = !r.sent || now.flags != r.flags;
    const bool step = std::fabs(now.energy - r.energy) >= kEnergyStep && ms - r.sentMs >= kEnergyPeriodMs;
    if (!edge && !step) return;
    if (now.elementId == 0 || now.elementId == static_cast<uint32_t>(EL::kInvalidId)) return;  // not enrolled yet
    if (s->connected()) s->SendReliable(coop::net::ReliableKind::KerfusState, &now, sizeof(now));
    if (edge)
        UE_LOGI("kerfus_state: HOST Kerfus eid=%u %s%s energy=%.1f", now.elementId,
                (now.flags & KF::kActive) ? "ON" : "OFF", (now.flags & KF::kCharging) ? " charging" : "",
                now.energy);
    r.sent = true;
    r.flags = now.flags;
    r.energy = now.energy;
    r.sentMs = ms;
}

// ---- client --------------------------------------------------------------------------------------

std::set<uint32_t> g_wrongClassSaid;  // eids a state named that are no Kerfus here (said once a session)

// False while the Kerfus's mirror is not bound here: the state is held in bind_wait for its bind.
bool Apply(const coop::net::KerfusStatePayload& p) {
    EL::Prop* el = EL::MirrorManager<EL::Prop>::Instance().Get(static_cast<EL::ElementId>(p.elementId));
    void* k = el ? el->GetActor() : nullptr;
    if (!k || !R::IsLiveByIndex(k, el->GetInternalIdx())) return false;
    bool wasActive = false;
    if (!UK::IsKerfus(k) || !UK::ReadActive(k, wasActive)) {
        if (g_wrongClassSaid.insert(p.elementId).second)
            UE_LOGW("kerfus_state: a state named eid=%u, which is no Kerfus here -- dropped", p.elementId);
        return true;
    }
    const bool active = (p.flags & KF::kActive) != 0;
    UK::WriteActive(k, active);
    UK::WriteCharging(k, (p.flags & KF::kCharging) != 0);
    UK::WriteEnergy(k, p.energy);
    // upd() is the game's own refresh after `active` changes: the face, the sounds, the camera.
    if (active != wasActive) {
        UK::RunUpd(k, false);
        UE_LOGI("kerfus_state: CLIENT Kerfus eid=%u %s (energy %.1f)", p.elementId, active ? "ON" : "OFF", p.energy);
    }
    return true;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_watched) g_watched = sg::WatchName(kRowReceiveTick, kTagTick, nullptr, &OnTickPost);
}

void Tick() {
    if (!g_saidLive && g_watched) {
        // This lane drives its own watch to live, and says so once.
        sg::ResolvePendingNames();
        if (sg::NameWatchLive(kRowReceiveTick, kTagTick)) {
            g_saidLive = true;
            UE_LOGI("kerfus_state: the Kerfus's tick is watched -- a host sends its state");
        }
    }
}

void OnPeerWorldReady(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    int sent = 0;
    for (auto it = g_host.begin(); it != g_host.end();) {
        void* k = it->first;
        if (!R::IsLiveByIndex(k, it->second.idx)) { it = g_host.erase(it); continue; }
        coop::net::KerfusStatePayload p{};
        if (Snapshot(k, it->second.eid, p) && p.elementId != 0 &&
            p.elementId != static_cast<uint32_t>(EL::kInvalidId) &&
            s->SendReliableToSlot(slot, coop::net::ReliableKind::KerfusState, &p, sizeof(p)))
            ++sent;
        ++it;
    }
    if (sent) UE_LOGI("kerfus_state: slot %d's world is up -- sent %d Kerfus state(s)", slot, sent);
}

namespace {
// A state held in bind_wait runs again once its Kerfus is bound: the whole of OnState.
void ReplayState(const uint8_t* bytes, size_t n, int /*senderSlot*/) {
    coop::net::KerfusStatePayload p{};
    if (n != sizeof p) return;
    std::memcpy(&p, bytes, sizeof p);
    OnState(p);
}
}  // namespace

void OnState(const coop::net::KerfusStatePayload& payload) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;  // from the host alone: its receive edge refuses a client's copy
    if (Apply(payload)) return;
    // Latest state wins. Not deferred: no bind is coming for this eid, so there is nothing to wait for.
    if (!coop::element::bind_wait::Hold(payload.elementId, kStateKind, /*senderSlot=*/0, &ReplayState, &payload,
                                        sizeof payload, /*coalesce=*/true))
        coop::element::bind_wait::NoteNeverBound(kStateKind, payload.elementId);
}

bool HostKnows(uint32_t eid) {
    for (const auto& kv : g_host)
        if (kv.second.eid == eid) return true;
    return false;
}

void OnDisconnect() {
    g_host.clear();
    g_wrongClassSaid.clear();
}

}  // namespace coop::kerfus_state
