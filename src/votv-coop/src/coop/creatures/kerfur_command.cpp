// coop/creatures/kerfur_command.cpp -- see coop/creatures/kerfur_command.h for the design + RE
// ground truth.

#include "coop/creatures/kerfur_command.h"

#include "coop/creatures/npc_sync.h"          // GetNpcIdForActor (host eid for the host's own menu use)
#include "coop/creatures/served_player.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/npc.h"
#include "coop/element/registry.h"
#include "coop/net/intent_queue.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/refusal_tally.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // Local() / kPeerIdHost
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"     // PerSlotState
#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/facts/function_row.h"

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

UE_FACT_FN(kRowStartKill, "kerfur_command", L"kerfurOmega_C", L"startKill", Declared);
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

// A sender's requests run in arrival order from a bounded queue at a bounded rate. The head waits, with its
// token given back, while the host cannot yet run it for the sender: no posed body for it (the seconds between a
// joiner's curtain and its first pose, a mid-join window rather than a verdict), or the Omega's seams and its
// startKill watch still settling -- since the verb's own reads would take the host. A request past the cap, or
// one that can never run (an unknown sender or command, an eid outside the host range), is refused into the
// tally. Per-slot state: a leaver's queue goes with its occupant. Game thread.
constexpr coop::net::IntentBudget kBudget{4.0f, 2.0f};
constexpr size_t kMaxPending = 8;

struct Slot {
    coop::net::IntentQueue<coop::net::KerfurCommandPayload> queue;
    bool waitSaid = false;  // the head's wait, said once a streak
};
coop::roster_ledger::PerSlotState<Slot> g_slots;
coop::net::RefusalTally g_tally{"kerfur_command"};

// Instruments (the kerfur_flood drill reads them against a baseline), per sender.
coop::roster_ledger::PerSlotState<uint32_t> g_runs;
coop::roster_ledger::PerSlotState<uint32_t> g_refused;

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

// One refusal into the tally, and into the sender's count when the slot is a client's (1 and up).
void Refuse(uint8_t slot, const char* why) {
    if (slot >= 1 && slot < coop::players::kMaxPeers) ++g_refused[slot];
    g_tally.Note(slot, why, coop::net::RefusalTally::Clock::now());
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
    if ((g_killLive = sg::NameWatchLive(kRowStartKill, kTagStartKill))) {
        UE_LOGI("kerfur_command: the Omega's startKill is watched -- murder mode ends a client's service");
    } else if (sg::NameWatchSettled(kRowStartKill, kTagStartKill)) {
        g_killRefused = true;
        UE_LOGE("kerfur_command: the startKill watch died at the name resolve -- an Omega serves no client for the "
                "rest of this process");
    }
}

// A refusal at the serve, said through the tally. It is not in RefusedFor: the request entered RunRequest, so it is
// a run, and each request is a run or a refusal, never both, which is what the drill's accounting holds.
void RefuseAtServe(uint8_t slot, const char* why) {
    g_tally.Note(slot, why, coop::net::RefusalTally::Clock::now());
}

// A request whose sender has a body: validated against the kerfur as it is now, then run.
void RunRequest(const coop::net::KerfurCommandPayload& payload, uint8_t senderPeerSlot) {
    ++g_runs[senderPeerSlot];
    const auto eid = static_cast<ElementId>(payload.elementId);
    auto* el = coop::element::MirrorManager<coop::element::Npc>::Instance().Get(eid);
    void* actor = el ? el->GetActor() : nullptr;
    const int32_t idx = el ? el->GetInternalIdx() : -1;
    if (!actor || !R::IsLiveByIndex(actor, idx)) {
        RefuseAtServe(senderPeerSlot, "no live kerfur (stale)");
        return;
    }
    if (!K::IsKerfurActor(actor)) {
        RefuseAtServe(senderPeerSlot, "targets a non-kerfur element");
        return;
    }
    if (K::ReadKill(actor)) {  // the BP's own actionName guard (a murderfur refuses the menu)
        RefuseAtServe(senderPeerSlot, "kill mode (SP parity)");
        return;
    }
    ExecuteHostCommand(eid, actor, static_cast<Command>(payload.command), senderPeerSlot);
}

// Each sender's queue head, once a token allows it: waiting while the host cannot serve the sender, else run.
void ServeQueues() {
    for (uint8_t slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        Slot& sl = g_slots[slot];
        sl.queue.Serve(kBudget, coop::net::NowMs, [&](const coop::net::KerfurCommandPayload& p) {
            if (!CanRunFor(slot)) {
                if (!sl.waitSaid) {
                    sl.waitSaid = true;
                    UE_LOGI("kerfur_command: slot %u's commands wait: the host has no body for it, or the Omega's "
                            "seams are still settling", static_cast<unsigned>(slot));
                }
                return coop::net::Served::Wait;
            }
            sl.waitSaid = false;
            RunRequest(p, slot);
            return coop::net::Served::Done;
        });
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
    g_killWatched = sg::WatchName(kRowStartKill, kTagStartKill, &OnStartKillPre, nullptr);
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
    // Slot 0 is the host, whose own requests never arrive over the wire.
    if (senderPeerSlot == 0 || senderPeerSlot >= coop::players::kMaxPeers) {
        Refuse(senderPeerSlot, "unknown sender");
        return;
    }
    if (payload.command > static_cast<uint8_t>(Command::FixTransformers)) {
        Refuse(senderPeerSlot, "unknown command");
        return;
    }
    // Every legitimate target is a host-range element; an out-of-range id cannot change while the request waits.
    if (!coop::element::Registry::IsAllowedHostAllocatedEid(static_cast<ElementId>(payload.elementId))) {
        Refuse(senderPeerSlot, "eid outside the host range");
        return;
    }
    coop::net::KerfurCommandPayload copy = payload;
    if (!g_slots[senderPeerSlot].queue.Push(std::move(copy), kMaxPending)) Refuse(senderPeerSlot, "queue full");
}

void NoteRefused(uint8_t senderPeerSlot, const char* why) { Refuse(senderPeerSlot, why); }

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

    // 2) each sender's queued request, once its pace and its body allow; 3) the refusals' summed line once its
    // window has passed, both only on a running host: the queues are the host's, and the roster's bringup resets them
    // off this thread, so no Tick touches them outside a running host session; 4) the startKill watch, until settled.
    if (s->running() && s->role() == coop::net::Role::Host) {
        ServeQueues();
        g_tally.Flush(coop::net::RefusalTally::Clock::now());
    }
    SettleKillWatch();
}

uint32_t RunsFor(int slot) { return slot >= 1 && slot < coop::players::kMaxPeers ? g_runs[slot] : 0; }
uint32_t RefusedFor(int slot) { return slot >= 1 && slot < coop::players::kMaxPeers ? g_refused[slot] : 0; }

void OnDisconnect() {
    { std::lock_guard<std::mutex> lk(g_qMutex); g_queue.clear(); }
    g_tally.Clear();
}

}  // namespace coop::kerfur_command
