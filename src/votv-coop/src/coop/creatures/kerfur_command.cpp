// coop/kerfur_command.cpp -- see coop/kerfur_command.h for the design + RE ground truth.

#include "coop/creatures/kerfur_command.h"

#include "coop/element/mirror_manager.h"
#include "coop/element/npc.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/creatures/npc_sync.h"          // GetNpcIdForActor (host eid for the host's own menu use)
#include "coop/player/players_registry.h"  // Local() / Puppet(slot) / kPeerIdHost (the follow target body)
#include "coop/player/remote_player.h"     // RemotePlayer::GetActor (the puppet body actor)
#include "ue_wrap/engine/engine.h"         // GetActorLocation
#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace coop::kerfur_command {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace K = ue_wrap::kerfur;

using coop::element::ElementId;
using coop::element::kInvalidId;

std::atomic<coop::net::Session*> g_session{nullptr};
coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }

// Re-entrancy guard: the host executes a non-follow verb by re-invoking actionName via
// ProcessEvent (HostRunVerb), which re-enters the actionName interceptor. The thread-local flag
// makes TryRecordMenuCommand pass that re-entry through instead of recording it (else infinite
// loop). Game thread; the PE dispatch is synchronous on the same thread, so thread_local suffices.
thread_local bool t_inHostExec = false;

// enum_kerfurCommand::idle == b1 (1). We park an owned-follow kerfur in idle so the BP's own
// move() issues no competing player-0 follow proxies while our loop drives the path.
constexpr uint8_t kKerfurStateIdle = 1;
constexpr uint64_t kFollowReissueMs = 500;   // re-path toward the owner ~2 Hz (the BP re-issues on each move-complete; this matches)
constexpr float    kFollowAcceptCm  = 120.f; // stop ~1.2 m from the owner (the BP follow uses 100)

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// ---- recorded actions (interceptor -> Tick) ----------------------------------------------------
struct PendingCmd {
    void*   actor = nullptr;
    int32_t internalIdx = -1;
    Command command = Command::Invalid;
    bool    isClient = false;
};
std::mutex g_qMutex;
std::vector<PendingCmd> g_queue;

// ---- ownership-follow map (game-thread only: Tick + OnCommandRequest both run on the net pump) --
std::unordered_map<ElementId, uint8_t> g_followOwner;  // host kerfur eid -> owner peer slot

const wchar_t* VerbName(Command c) {
    switch (c) {
        case Command::Idle:            return L"idle";
        case Command::Patrol:          return L"patrol";
        case Command::FixServers:      return L"fix_servers";
        case Command::GetReports:      return L"get_reports";
        case Command::FixTransformers: return L"fix_transformers";
        default:                       return nullptr;  // Follow handled separately
    }
}

// The requesting player's BODY actor to follow: the host's own mainPlayer_C (kPeerIdHost) or a
// remote client's puppet (a bare skeletal-mesh actor). Null while the owner is momentarily absent.
void* ResolveOwnerBody(uint8_t ownerSlot) {
    auto& reg = coop::players::Registry::Get();
    if (ownerSlot == coop::players::kPeerIdHost) return reg.Local();
    if (coop::RemotePlayer* p = reg.Puppet(ownerSlot)) return p->GetActor();
    return nullptr;
}

// Resolve the WIRE eid (the host's id) off the client's adopted/mirror kerfur Element.
ElementId FindWireEidForNpcActor(void* actor) {
    if (!actor) return kInvalidId;
    std::vector<coop::element::Npc*> elems;
    coop::element::MirrorManager<coop::element::Npc>::Instance().Snapshot(elems);
    ElementId nonMirror = kInvalidId;
    for (coop::element::Npc* el : elems) {
        if (!el || el->GetActor() != actor) continue;
        if (el->IsMirror()) return el->GetId();
        nonMirror = el->GetId();
    }
    return nonMirror;
}

void HostRunVerb(void* actor, void* player, const wchar_t* name) {
    t_inHostExec = true;
    K::RunActionName(actor, player, name);
    t_inHostExec = false;
}

// Apply a command to the host's real kerfur (eid already resolved + validated).
void ExecuteHostCommand(ElementId eid, void* actor, Command c, uint8_t ownerSlot) {
    if (c == Command::Follow) {
        g_followOwner[eid] = ownerSlot;
        K::SetCommandState(actor, kKerfurStateIdle);  // silence the BP's player-0 mover
        UE_LOGI("kerfur_command: HOST follow eid=%u -> owner slot %u (host-driven MoveTo loop)",
                eid, ownerSlot);
    } else {
        g_followOwner.erase(eid);  // any other command ends an owned-follow
        if (const wchar_t* verb = VerbName(c)) {
            HostRunVerb(actor, coop::players::Registry::Get().Local(), verb);
            UE_LOGI("kerfur_command: HOST ran verb '%ls' eid=%u (requester slot %u)",
                    verb, eid, ownerSlot);
        }
    }
}

void RunFollowLoop() {
    static uint64_t sLastMs = 0;
    const uint64_t now = NowMs();
    if (now - sLastMs < kFollowReissueMs) return;
    sLastMs = now;
    auto& mgr = coop::element::MirrorManager<coop::element::Npc>::Instance();
    std::vector<ElementId> dead;
    for (const auto& [eid, ownerSlot] : g_followOwner) {
        coop::element::Npc* el = mgr.Get(eid);
        void* kerfur = el ? el->GetActor() : nullptr;
        if (!kerfur || !R::IsLiveByIndex(kerfur, el->GetInternalIdx())) { dead.push_back(eid); continue; }
        void* body = ResolveOwnerBody(ownerSlot);
        if (!body || !R::IsLive(body)) continue;  // owner momentarily gone -- keep the binding, skip this tick
        const auto loc = E::GetActorLocation(body);
        K::SetCommandState(kerfur, kKerfurStateIdle);  // re-assert (defensive vs any BP write)
        K::IssueFollowMoveTo(kerfur, body, loc.X, loc.Y, loc.Z, kFollowAcceptCm);
    }
    for (ElementId eid : dead) g_followOwner.erase(eid);
}

}  // namespace

Command CommandFromActionName(const std::wstring& name) {
    if (name == L"follow")           return Command::Follow;
    if (name == L"idle")             return Command::Idle;
    if (name == L"patrol")           return Command::Patrol;
    if (name == L"fix_servers")      return Command::FixServers;
    if (name == L"get_reports")      return Command::GetReports;
    if (name == L"fix_transformers") return Command::FixTransformers;
    return Command::Invalid;  // turn_off (kerfur_convert) / take_object / equipment / pat
}

bool TryRecordMenuCommand(void* self, const std::wstring& name, bool isClient, bool /*isHost*/) {
    if (t_inHostExec) return false;  // our own host actionName replay -- pass through
    if (CommandFromActionName(name) == Command::Invalid) return false;  // not a relayed verb
    auto* s = LoadSession();
    if (!s || !s->running() || !s->connected()) return false;  // SP / not connected -> leave it
    PendingCmd pc;
    pc.actor = self;
    pc.internalIdx = R::InternalIndexOf(self);
    pc.command = CommandFromActionName(name);
    pc.isClient = isClient;
    { std::lock_guard<std::mutex> lk(g_qMutex); g_queue.push_back(pc); }
    return true;  // cancel the local dispatch on BOTH roles -- the host runs it authoritatively
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void OnCommandRequest(const coop::net::KerfurCommandPayload& payload, uint8_t senderPeerSlot) {
    auto* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Host) {
        UE_LOGW("kerfur_command: KerfurCommand received on a non-host -- dropping");
        return;
    }
    if (!coop::element::Registry::IsAllowedHostAllocatedEid(
            static_cast<ElementId>(payload.elementId))) {
        UE_LOGW("kerfur_command: request eid=%u outside the host range -- dropped (slot %u)",
                payload.elementId, senderPeerSlot);
        return;
    }
    const auto eid = static_cast<ElementId>(payload.elementId);
    auto* el = coop::element::MirrorManager<coop::element::Npc>::Instance().Get(eid);
    void* actor = el ? el->GetActor() : nullptr;
    const int32_t idx = el ? el->GetInternalIdx() : -1;
    if (!actor || !R::IsLiveByIndex(actor, idx)) {
        UE_LOGW("kerfur_command: command eid=%u from slot %u -- no live kerfur (stale) -- dropped",
                payload.elementId, senderPeerSlot);
        return;
    }
    if (!K::IsKerfurActor(actor)) {
        UE_LOGW("kerfur_command: command eid=%u targets a non-kerfur element -- dropped", payload.elementId);
        return;
    }
    if (K::ReadKill(actor)) {  // the BP's own actionName guard (a murderfur refuses the menu)
        UE_LOGI("kerfur_command: command eid=%u denied -- kerfur is in kill mode (SP parity)", payload.elementId);
        return;
    }
    if (payload.command > static_cast<uint8_t>(Command::FixTransformers)) {
        UE_LOGW("kerfur_command: unknown command=%u eid=%u -- dropped", payload.command, payload.elementId);
        return;
    }
    ExecuteHostCommand(eid, actor, static_cast<Command>(payload.command), senderPeerSlot);
}

void Tick() {
    auto* s = LoadSession();
    if (!s) return;

    // 1) drain recorded actions (interceptor cancelled the local dispatch; we act here).
    std::vector<PendingCmd> ready;
    { std::lock_guard<std::mutex> lk(g_qMutex); if (!g_queue.empty()) ready.swap(g_queue); }
    for (const PendingCmd& pc : ready) {
        if (!pc.actor || !R::IsLiveByIndex(pc.actor, pc.internalIdx)) continue;  // died before drain
        if (pc.isClient) {
            const ElementId eid = FindWireEidForNpcActor(pc.actor);
            if (eid == kInvalidId) {
                UE_LOGW("kerfur_command[client]: command %u on an UNTRACKED kerfur -- no request "
                        "(not adopted yet / rogue local)", static_cast<unsigned>(pc.command));
                continue;
            }
            coop::net::KerfurCommandPayload p{};
            p.elementId = static_cast<uint32_t>(eid);
            p.command   = static_cast<uint8_t>(pc.command);
            if (s->connected())
                s->SendReliable(coop::net::ReliableKind::KerfurCommand, &p, sizeof(p));
            UE_LOGI("kerfur_command[client]: sent command=%u eid=%u",
                    static_cast<unsigned>(pc.command), eid);
        } else {
            // The host's own menu use: requester = host; execute locally.
            const ElementId eid = coop::npc_sync::GetNpcIdForActor(pc.actor);
            if (eid == kInvalidId) {
                UE_LOGW("kerfur_command[host]: own command on an UNTRACKED kerfur -- skipped");
                continue;
            }
            ExecuteHostCommand(eid, pc.actor, pc.command, coop::players::kPeerIdHost);
        }
    }

    // 2) advance the ownership-follow loop (host only).
    if (s->role() == coop::net::Role::Host && !g_followOwner.empty()) RunFollowLoop();
}

void OnPeerDisconnect(uint8_t slot) {
    // Game thread (subsystems::DisconnectSlot, from the net_pump per-slot disconnect edge). End every
    // owned-follow this leaver held so the kerfur isn't pinned to idle forever + the loop stops
    // chasing a now-null puppet. Restore State=follow (b0) so the host's BP resumes following the host.
    auto& mgr = coop::element::MirrorManager<coop::element::Npc>::Instance();
    for (auto it = g_followOwner.begin(); it != g_followOwner.end();) {
        if (it->second != slot) { ++it; continue; }
        coop::element::Npc* el = mgr.Get(it->first);
        void* actor = el ? el->GetActor() : nullptr;
        if (actor && R::IsLiveByIndex(actor, el->GetInternalIdx()))
            K::SetCommandState(actor, 0);  // b0 = follow -> the BP resumes (now follows the host)
        UE_LOGI("kerfur_command: owner slot %u left -- released owned-follow eid=%u (restored to follow)",
                slot, it->first);
        it = g_followOwner.erase(it);
    }
}

void OnDisconnect() {
    { std::lock_guard<std::mutex> lk(g_qMutex); g_queue.clear(); }
    g_followOwner.clear();
}

}  // namespace coop::kerfur_command
