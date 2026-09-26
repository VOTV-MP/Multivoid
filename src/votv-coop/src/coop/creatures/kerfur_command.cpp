// coop/creatures/kerfur_command.cpp -- see coop/creatures/kerfur_command.h for the design + RE
// ground truth.

#include "coop/creatures/kerfur_command.h"

#include "coop/creatures/npc_sync.h"          // GetNpcIdForActor (host eid for the host's own menu use)
#include "coop/creatures/served_player.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/npc.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // Local() / kPeerIdHost
#include "coop/player/remote_player.h"
#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace coop::kerfur_command {
namespace {

namespace R  = ue_wrap::reflection;
namespace K  = ue_wrap::kerfur;
namespace P  = ue_wrap::profile;
namespace SP = coop::served_player;
namespace sg = ue_wrap::script_gate;

using coop::element::ElementId;
using coop::element::kInvalidId;

constexpr const wchar_t* kStartKill = L"startKill";
constexpr int kTagStartKill = 0x4B434B01;  // 'KCK' 1

std::atomic<coop::net::Session*> g_session{nullptr};
coop::net::Session* LoadSession() { return g_session.load(std::memory_order_acquire); }

// The startKill watch: an Omega serves a client only while it is live, since murder mode's chase would
// otherwise read that client; refused or dead, it is said once and every command runs for the host.
bool g_killWatched = false;
bool g_killLive = false;
bool g_killRefused = false;
bool g_saidUnserved = false;

// Its follow, patrol, hand-back and turn read GetPlayerPawn, its disc insert getMainPlayer.
constexpr uint8_t kOmegaSeams = SP::kPlayerPawn | SP::kMainPlayer;

// A client's request waits while the host cannot yet run it for the sender: no posed body for it (the
// seconds between a joiner's curtain and its first pose, a mid-join window rather than a verdict), or the
// Omega's seams and its startKill watch still settling -- since the verb's own reads would take the host.
// Only a sender's latest request per kerfur is kept, and a sender with requests waiting queues behind them.
// A leaver's go. Game thread.
struct Parked {
    coop::net::KerfurCommandPayload payload;
    uint8_t slot;
};
std::vector<Parked> g_parked;
bool g_parkSaid[coop::players::kMaxPeers] = {};

// Re-entrancy guard: the host executes a verb by re-invoking actionName via ProcessEvent (HostRunVerb),
// which re-enters the actionName interceptor. The thread-local flag makes TryRecordMenuCommand pass that
// re-entry through instead of recording it (else infinite loop). Game thread; the PE dispatch is
// synchronous on the same thread, so thread_local suffices.
thread_local bool t_inHostExec = false;

// ---- recorded actions (interceptor -> Tick) ----------------------------------------------------
struct PendingCmd {
    void*   actor = nullptr;
    int32_t internalIdx = -1;
    Command command = Command::Invalid;
    bool    isClient = false;
};
std::mutex g_qMutex;
std::vector<PendingCmd> g_queue;

const wchar_t* VerbName(Command c) {
    switch (c) {
        case Command::Follow:          return L"follow";
        case Command::Idle:            return L"idle";
        case Command::Patrol:          return L"patrol";
        case Command::FixServers:      return L"fix_servers";
        case Command::GetReports:      return L"get_reports";
        case Command::FixTransformers: return L"fix_transformers";
        default:                       return nullptr;
    }
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

bool HostRunVerb(void* actor, void* player, const wchar_t* name) {
    t_inHostExec = true;
    const bool ran = K::RunActionName(actor, player, name);
    t_inHostExec = false;
    return ran;
}

bool HasBody(uint8_t slot) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    return rp && rp->valid() && rp->HasPose();
}

// Settling, not refused: a refusal is final, and its commands run for the host instead of waiting.
bool Settling() {
    return (!g_killLive && !g_killRefused) || (!SP::SeamsReady(kOmegaSeams) && !SP::SeamsRefused(kOmegaSeams));
}

bool CanRunFor(uint8_t slot) { return HasBody(slot) && !Settling(); }

bool HasParked(uint8_t slot) {
    for (const Parked& p : g_parked)
        if (p.slot == slot) return true;
    return false;
}

void Park(const coop::net::KerfurCommandPayload& payload, uint8_t slot) {
    for (Parked& p : g_parked) {
        if (p.slot == slot && p.payload.elementId == payload.elementId) {
            p.payload = payload;  // a newer command for that kerfur replaces the waiting one
            return;
        }
    }
    g_parked.push_back(Parked{payload, slot});
    if (!g_parkSaid[slot]) {
        g_parkSaid[slot] = true;
        UE_LOGI("kerfur_command: slot %u's commands wait: the host has no body for it, or the Omega's seams are "
                "still settling", static_cast<unsigned>(slot));
    }
}

// The game's own guard on every relayed verb (bytecode @20950-@22108): a kerfur on a job -- fixing
// servers, getting reports, fixing transformers, states 3 to 5 -- refuses the command. Mirrored, as the kill
// guard is, so that a refused command hands the kerfur to nobody.
bool OnAJob(void* actor) {
    uint8_t state = 0, face = 0;
    bool spooky = false;
    return K::ReadKerfurState(actor, state, spooky, face) && state >= 3 && state <= 5;
}

// Apply a command to the host's real kerfur (eid already resolved + validated): the game's own verb, with
// the kerfur serving its requester -- its player-0 reads answered with that player's puppet by
// served_player, the host's own command handing it back to player 0. The verbs read no Player parameter
// (only take_object and equipment do, which are not relayed), so the host's player stands in.
void ExecuteHostCommand(ElementId eid, void* actor, Command c, uint8_t requesterSlot) {
    const wchar_t* verb = VerbName(c);
    if (!verb) return;
    if (OnAJob(actor)) {
        UE_LOGI("kerfur_command: verb '%ls' eid=%u from slot %u refused -- the kerfur is on a job (SP parity)",
                verb, eid, requesterSlot);
        return;
    }
    const uint8_t before = SP::ServedSlot(actor);
    uint8_t serves = requesterSlot;
    if (serves != coop::players::kPeerIdHost && !g_killLive) {
        if (!g_saidUnserved) {
            g_saidUnserved = true;
            UE_LOGW("kerfur_command: the Omega's startKill watch is refused -- a client's command runs for the host");
        }
        serves = coop::players::kPeerIdHost;
    }
    if (serves == coop::players::kPeerIdHost || !SP::Serve(actor, serves, kOmegaSeams)) {
        serves = coop::players::kPeerIdHost;  // a refused seam is said by served_player
        SP::Unserve(actor);
    }
    if (!HostRunVerb(actor, coop::players::Registry::Get().Local(), verb)) {
        const bool restored = before != coop::players::kPeerIdHost && SP::Serve(actor, before, kOmegaSeams);
        if (!restored) SP::Unserve(actor);
        UE_LOGW("kerfur_command: verb '%ls' eid=%u did not run -- the kerfur serves slot %u", verb, eid,
                static_cast<unsigned>(restored ? before : coop::players::kPeerIdHost));
        return;
    }
    UE_LOGI("kerfur_command: HOST ran verb '%ls' eid=%u (requester slot %u, serving slot %u)", verb, eid,
            requesterSlot, static_cast<unsigned>(serves));
}

// The kill verb's event sets the follow state and calls move() before it sets `kill`: murder mode's chase and
// attack are its own, not a command's, so the service ends at its entry, before the first read.
sg::Verdict OnStartKillPre(const sg::Call& c) {
    const uint8_t slot = SP::ServedSlot(c.object);
    if (slot == coop::players::kPeerIdHost) return sg::Verdict::Run;
    SP::Unserve(c.object);
    UE_LOGI("kerfur_command: kerfur %p turns murderous -- it no longer serves slot %u", c.object,
            static_cast<unsigned>(slot));
    return sg::Verdict::Run;
}

// The watch settles at once where it can, so the first command runs with it live: the name resolve is a
// game-thread engine call.
void SettleKillWatch() {
    if (!g_killWatched || g_killLive || g_killRefused) return;
    sg::ResolvePendingNames();
    if ((g_killLive = sg::ClassNameWatchLive(P::name::NpcClass_KerfurOmega, kStartKill, kTagStartKill))) {
        UE_LOGI("kerfur_command: the Omega's startKill is watched -- murder mode ends a client's service");
    } else if (sg::ClassNameWatchSettled(P::name::NpcClass_KerfurOmega, kStartKill, kTagStartKill)) {
        g_killRefused = true;
        UE_LOGE("kerfur_command: the startKill watch died at the name resolve -- an Omega serves no client for the "
                "rest of this process");
    }
}

// A request whose sender has a body: validated against the kerfur as it is now, then run.
void RunRequest(const coop::net::KerfurCommandPayload& payload, uint8_t senderPeerSlot) {
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

// In arrival order: a sender's requests all run in the tick it can be served.
void RunParked() {
    for (size_t i = 0; i < g_parked.size();) {
        if (!CanRunFor(g_parked[i].slot)) { ++i; continue; }
        const Parked p = g_parked[i];
        g_parked.erase(g_parked.begin() + static_cast<std::ptrdiff_t>(i));
        g_parkSaid[p.slot] = false;
        RunRequest(p.payload, p.slot);
    }
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

bool TryRecordMenuCommand(void* self, const std::wstring& name, bool isClient) {
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
    // Only a host serves, so the watch registers there, once.
    if (g_killWatched || g_killRefused || !session || !session->running() ||
        session->role() != coop::net::Role::Host)
        return;
    g_killWatched = sg::WatchClassName(P::name::NpcClass_KerfurOmega, kStartKill, kTagStartKill, &OnStartKillPre,
                                       nullptr);
    if (!g_killWatched) {
        g_killRefused = true;
        UE_LOGE("kerfur_command: the startKill watch was refused -- an Omega serves no client for the rest of this "
                "process");
        return;
    }
    SettleKillWatch();
}

void OnCommandRequest(const coop::net::KerfurCommandPayload& payload, uint8_t senderPeerSlot) {
    auto* s = LoadSession();
    if (!s || s->role() != coop::net::Role::Host) {
        UE_LOGW("kerfur_command: KerfurCommand received on a non-host -- dropping");
        return;
    }
    if (senderPeerSlot >= coop::players::kMaxPeers) return;
    if (!CanRunFor(senderPeerSlot) || HasParked(senderPeerSlot)) {
        Park(payload, senderPeerSlot);
        return;
    }
    RunRequest(payload, senderPeerSlot);
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

    // 2) the requests whose sender now has a body, in arrival order; 3) the startKill watch, until settled.
    if (!g_parked.empty()) RunParked();
    SettleKillWatch();
}

void OnPeerLeft(uint8_t slot) {
    for (auto it = g_parked.begin(); it != g_parked.end();) {
        if (it->slot == slot) it = g_parked.erase(it);
        else ++it;
    }
    if (slot < coop::players::kMaxPeers) g_parkSaid[slot] = false;
}

void OnDisconnect() {
    { std::lock_guard<std::mutex> lk(g_qMutex); g_queue.clear(); }
    g_parked.clear();
    for (bool& said : g_parkSaid) said = false;
}

}  // namespace coop::kerfur_command
