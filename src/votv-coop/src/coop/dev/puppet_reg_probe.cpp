// coop/dev/puppet_reg_probe.cpp -- [dev] a puppet is registered before its begin-play. See the header.
#include "coop/dev/puppet_reg_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/actor_end_play.h"
#include "ue_wrap/facts/function_row.h"

#include <cstdint>

namespace coop::dev::puppet_reg_probe {
namespace {

namespace sg = ue_wrap::script_gate;
namespace EP = ue_wrap::actor_end_play;
namespace PL = coop::players;

UE_FACT_FN(kRowBeginPlay, "dev.puppet_reg_probe", L"mainPlayer_C", L"ReceiveBeginPlay", Declared);
constexpr int kTag = 0x50493030;  // 'PI00'

// One mainPlayer_C's begin-play. `slot` stays unknown until the pump tick finds the actor to be a
// registered puppet's; the local player and the skin preview never bind, so the end-of-play sink
// never judges them.
struct Rec {
    ue_wrap::CachedObjRef actor;
    uint8_t slot = PL::kPeerIdUnknown;
    bool registeredAtBegin = false;
    uint32_t life = 0;
    uint32_t seq = 0;
    bool abandoned = false;  // a valid puppet whose begin-play was not observed, said once
};
constexpr int kRecs = 8;
Rec g_recs[kRecs];
uint32_t g_seq = 0;
uint32_t g_lives[PL::kMaxPeers] = {};  // the puppets each slot has had in this process
bool g_fullSaid = false;

// The entry a new record takes: the first whose actor is dead or unused, else the unbound entry with the
// lowest seq. An entry bound to a slot is never taken while its actor lives; with every entry bound and
// live (more than eight mainPlayer_C in one world, which no game mode spawns) nothing is recorded.
Rec* Claim() {
    Rec* unbound = nullptr;
    for (Rec& r : g_recs) {
        if (!r.actor.Raw() || !r.actor.Alive()) return &r;
        if (r.slot == PL::kPeerIdUnknown && (!unbound || r.seq < unbound->seq)) unbound = &r;
    }
    if (!unbound && !g_fullSaid) {
        g_fullSaid = true;
        UE_LOGW("[PI-0] WARN record full");
    }
    return unbound;
}

Rec* Find(void* actor) {
    for (Rec& r : g_recs)
        if (r.actor.Is(actor)) return &r;
    return nullptr;
}

sg::Verdict OnBeginPlayPre(const sg::Call& call) {
    const uint32_t seq = ++g_seq;
    if (!call.object) return sg::Verdict::Run;
    Rec* r = Claim();
    if (!r) return sg::Verdict::Run;
    *r = Rec{};
    r->actor.Set(call.object);
    r->registeredAtBegin = PL::Registry::Get().IsPuppet(call.object);
    r->seq = seq;
    return sg::Verdict::Run;
}

void OnEndPlay(void* actor, EP::Reason /*reason*/) {
    for (Rec& r : g_recs) {
        if (r.slot >= PL::kMaxPeers || r.actor.Raw() != actor || !r.actor.Is(actor)) continue;
        coop::RemotePlayer* const p = PL::Registry::Get().Puppet(r.slot);
        if (p && p->GetActor() == actor)
            UE_LOGI("[PI-0] PASS slot=%u puppet at end-play life=%u", static_cast<unsigned>(r.slot), r.life);
        else
            UE_LOGI("[PI-0] FAIL slot=%u puppet not the slot's at its end-play life=%u",
                    static_cast<unsigned>(r.slot), r.life);
        return;
    }
}

void JudgeSlot(uint8_t slot) {
    coop::RemotePlayer* const p = PL::Registry::Get().Puppet(slot);
    if (!p || !p->valid()) return;
    void* const actor = p->GetActor();
    Rec* r = Find(actor);
    if (!r) {
        Rec* a = Claim();
        if (!a) return;
        *a = Rec{};
        a->actor.Set(actor);
        a->abandoned = true;
        a->seq = ++g_seq;
        UE_LOGI("[PI-0] ABANDONED slot=%u begin-play not observed", static_cast<unsigned>(slot));
        return;
    }
    if (r->abandoned || r->slot != PL::kPeerIdUnknown) return;
    r->slot = slot;
    r->life = ++g_lives[slot];
    if (r->registeredAtBegin)
        UE_LOGI("[PI-0] PASS slot=%u puppet at begin-play life=%u", static_cast<unsigned>(slot), r->life);
    else
        UE_LOGI("[PI-0] FAIL slot=%u puppet not registered at begin-play life=%u", static_cast<unsigned>(slot),
                r->life);
}

bool Arm() {
    if (!EP::IsInstalled()) {
        UE_LOGW("puppet_reg_probe: the end-of-play seam is not installed -- nothing to judge at end of play");
        return false;
    }
    if (!sg::WatchName(kRowBeginPlay, kTag, &OnBeginPlayPre, nullptr)) {
        UE_LOGW("puppet_reg_probe: the gate refused the watch on mainPlayer_C::ReceiveBeginPlay");
        return false;
    }
    if (!EP::AddSink(&OnEndPlay)) {
        UE_LOGW("puppet_reg_probe: the end-of-play sink table is full -- the begin-play watch is withdrawn");
        sg::UnwatchName(kRowBeginPlay, kTag, &OnBeginPlayPre, nullptr);
        return false;
    }
    UE_LOGI("puppet_reg_probe: armed -- each puppet judged at its begin-play and at its end of play");
    return true;
}

}  // namespace

void Tick() {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::puppet_reg_probe);
    if (!s_on) return;
    static int s_state = 0;  // 0 = not armed yet, 1 = armed, the watch not settled, 2 = judging, -1 = could not arm
    if (s_state == 0) s_state = Arm() ? 1 : -1;
    if (s_state < 0) return;
    if (s_state == 1) {
        sg::ResolvePendingNames();
        if (!sg::NameWatchSettled(kRowBeginPlay, kTag)) return;
        s_state = 2;
        if (!sg::NameWatchLive(kRowBeginPlay, kTag))
            UE_LOGE("puppet_reg_probe: the begin-play watch is dead -- every puppet will read ABANDONED");
    }
    for (uint8_t slot = 0; slot < PL::kMaxPeers; ++slot) JudgeSlot(slot);
}

}  // namespace coop::dev::puppet_reg_probe
