// coop/creatures/kerfur_command.cpp -- see coop/creatures/kerfur_command.h for the design + RE
// ground truth.

#include "coop/creatures/kerfur_command.h"

#include "coop/creatures/kerfur_command_answer.h"  // the host's answer to a refused or waiting command
#include "coop/creatures/kerfur_command_wire.h"    // a command's bytes, and the chunks that bring them to the host
#include "coop/creatures/kerfur_disc_carry.h"      // the held disc a get_reports carries
#include "coop/creatures/npc_sync.h"          // GetNpcIdForActor (host eid for the host's own menu use)
#include "coop/creatures/served_player.h"
#include "coop/element/intent_authority.h"  // the reach judge a client intent is held to
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
#include "coop/props/container_reach.h"    // kReachUU: the arm length the use trace is cast with
#include "coop/requester_feedback/requester_feedback.h"  // the scope of a verb the host runs for a client
#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <optional>
#include <vector>

namespace coop::kerfur_command {
namespace {

namespace R  = ue_wrap::reflection;
namespace K  = ue_wrap::kerfur;
namespace P  = ue_wrap::profile;
namespace SP = coop::served_player;
namespace sg = ue_wrap::script_gate;

namespace kcw = coop::kerfur_command_wire;
using KR = coop::net::kerfur_command_answer::KerfurRefusal;
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
bool g_saidSendFailed = false;

// Its follow, patrol, hand-back and turn read GetPlayerPawn, its disc insert getMainPlayer.
constexpr uint8_t kOmegaSeams = SP::kPlayerPawn | SP::kMainPlayer;

// A sender's requests run in arrival order from a bounded queue at a bounded rate. The head waits, with its
// token given back, while the host cannot yet run it for the sender: no posed body for it (the seconds between a
// joiner's curtain and its first pose, a mid-join window rather than a verdict), or the Omega's seams and its
// startKill watch still settling -- since the verb's own reads would take the host. A request past the cap, or
// one that can never run (an unknown sender or command, an eid outside the host range), is refused into the
// tally. A get_reports that carries a disc keeps with it the verdict of its arrival (the hand lane showed that disc or
// not). Per-slot state: a leaver's queue, and the discs queued in it, go with its occupant. Game thread.

// A request in a sender's queue: the command, and whether its disc tail passed the host's judge at arrival.
struct Queued {
    kcw::Request req;
    bool tailOk = true;
};

struct Slot {
    coop::net::IntentQueue<Queued> queue;
    bool waitSaid = false;  // the head's wait, said once a streak
    bool preSaid = false;   // a refusal before the queue (full, or an eid outside the host range), answered once a streak
};
coop::roster_ledger::PerSlotState<Slot> g_slots;
coop::net::RefusalTally g_tally{"kerfur_command"};

// Instruments (the kerfur_flood drill reads them against a baseline), per sender.
coop::roster_ledger::PerSlotState<uint32_t> g_runs;
coop::roster_ledger::PerSlotState<uint32_t> g_refused;

// Re-entrancy guard: the host executes a verb by re-invoking actionName via ProcessEvent (HostRunVerb),
// which re-enters the seat's actionName watch. The flag makes TryRecordMenuCommand pass that re-entry through
// instead of recording it (else infinite loop). Game thread; the dispatch is synchronous on the same thread, so
// thread_local suffices.
thread_local bool t_inHostExec = false;

// ---- recorded actions (the seat -> Tick) ---------------------------------------------------------
struct PendingCmd {
    void*   actor = nullptr;
    int32_t internalIdx = -1;
    Command command = Command::Invalid;
    bool    isClient = false;
    std::optional<coop::kerfur_disc_carry::Capture> disc;  // a client's get_reports: the held disc, captured at the press
};
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

// The verb runs inside the requester's feedback scope when a client asked for it: the hint, gloss or sound it makes
// goes to that client, not to the host's screen. The host's own command (slot 0) keeps its feedback.
bool HostRunVerb(void* actor, void* player, const wchar_t* name, uint8_t requesterSlot) {
    std::optional<coop::requester_feedback::Scope> feedback;
    if (requesterSlot != coop::players::kPeerIdHost) feedback.emplace(requesterSlot, 0);
    t_inHostExec = true;
    const bool ran = K::RunActionName(actor, player, name);
    t_inHostExec = false;
    return ran;
}

bool HasBody(uint8_t slot) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    return rp && rp->valid() && rp->HasPose();
}

// Settling, not refused: a refusal is final, and a client's commands are answered instead of waiting (ExecuteHostCommand).
bool Settling() {
    return (!g_killLive && !g_killRefused) || (!SP::SeamsReady(kOmegaSeams) && !SP::SeamsRefused(kOmegaSeams));
}

// The verb runs inside a feedback scope, whose seams stand only once requester_feedback is ready (subsystems::Install
// installs and ticks it for every lane): the head waits for
// them, or runs into the refusal when they never will (RunRequest).
bool CanRunFor(uint8_t slot) {
    return HasBody(slot) && !Settling() &&
           (coop::requester_feedback::Ready() || coop::requester_feedback::Refused());
}

// One refusal into the tally, and into the sender's count when the slot is a client's (1 and up).
void Refuse(uint8_t slot, const char* why) {
    if (slot >= 1 && slot < coop::players::kMaxPeers) ++g_refused[slot];
    g_tally.Note(slot, why, coop::net::RefusalTally::Clock::now());
}

// The requester told why its well-formed command was refused: a client's only, the host's own has nobody to tell.
void Answer(uint8_t slot, uint32_t requestId, KR reason) {
    if (slot == coop::players::kPeerIdHost) return;
    coop::kerfur_command_answer::SendRefused(LoadSession(), slot, requestId, static_cast<uint8_t>(reason));
}

// A refusal made before the queue is answered once a streak (cleared when the sender's next request runs), so a
// flooding sender is told once and not once a packet.
void AnswerOnceAStreak(uint8_t slot, uint32_t requestId, KR reason) {
    Slot& sl = g_slots[slot];
    if (sl.preSaid) return;
    sl.preSaid = true;
    Answer(slot, requestId, reason);
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
// (only take_object and equipment do, which are not relayed), so the host's player stands in. A client's command is
// never run for the host's player: when the host cannot serve the Omega as that client (the startKill watch or a
// seam refused) the command is refused, and the requester is told (Busy). A job refuses any sender's command,
// whichever player the Omega was serving, since the state is the Omega's own.
//
// `disc` is the record a client's get_reports carried, judged at its arrival: the verb then reads the hand of the
// puppet of the sender, which holds a copy of that disc for the verb's span (kerfur_disc_carry). The Omega's own
// gates are read first, since the copy is built only for a verb that will read the hand in the call: one that
// already holds a disc skips the read and runs as the game runs it, and one a deferral gate holds would read the
// hand on a later move, outside the span, so it is refused (Busy) with nothing built. The disc is consumed when the
// verb destroyed the copy, and the requester is told; a copy the verb left is destroyed here, and the client keeps its disc.
void ExecuteHostCommand(ElementId eid, void* actor, Command c, uint8_t requesterSlot, uint32_t requestId = 0,
                        const ue_wrap::save_record::SaveRecord* disc = nullptr) {
    const wchar_t* verb = VerbName(c);
    if (!verb) return;
    if (OnAJob(actor)) {
        UE_LOGI("kerfur_command: verb '%ls' eid=%u from slot %u refused -- the kerfur is on a job (SP parity)",
                verb, eid, requesterSlot);
        Answer(requesterSlot, requestId, KR::Busy);
        return;
    }
    const uint8_t before = SP::ServedSlot(actor);
    if (requesterSlot == coop::players::kPeerIdHost) {
        SP::Unserve(actor);
    } else if (!g_killLive || !SP::Serve(actor, requesterSlot, kOmegaSeams)) {
        if (!g_killLive && !g_saidUnserved) {
            g_saidUnserved = true;
            UE_LOGW("kerfur_command: the Omega's startKill watch is refused -- a client's command is refused");
        }
        UE_LOGI("kerfur_command: verb '%ls' eid=%u from slot %u refused -- the host cannot serve the Omega as that "
                "player", verb, eid, requesterSlot);  // a refused seam is said by served_player
        Answer(requesterSlot, requestId, KR::Busy);
        return;
    }
    // Whom the Omega served before goes back to it when the verb is not run.
    const auto restoreService = [&]() {
        const bool restored = before != coop::players::kPeerIdHost && SP::Serve(actor, before, kOmegaSeams);
        if (!restored) SP::Unserve(actor);
        return restored ? before : coop::players::kPeerIdHost;
    };
    std::optional<coop::kerfur_disc_carry::HandScope> hand;
    if (disc && c == Command::GetReports) {
        const auto gate = coop::kerfur_disc_carry::ReadGate(actor);
        if (gate == coop::kerfur_disc_carry::Gate::Busy) {
            const uint8_t serves = restoreService();
            UE_LOGI("kerfur_command: verb '%ls' eid=%u from slot %u refused -- the Omega's task gate is set, its read "
                    "of the disc would come outside the carry; the kerfur serves slot %u", verb, eid, requesterSlot,
                    static_cast<unsigned>(serves));
            Answer(requesterSlot, requestId, KR::Busy);
            return;
        }
        if (gate == coop::kerfur_disc_carry::Gate::HasDisc) {
            UE_LOGI("kerfur_command: verb '%ls' eid=%u from slot %u runs with no disc carried -- the Omega already "
                    "holds one, and the game skips the read", verb, eid, requesterSlot);
        } else {
            hand.emplace(requesterSlot, *disc);
            if (!hand->ok()) {
                hand.reset();
                const uint8_t serves = restoreService();
                UE_LOGW("kerfur_command: verb '%ls' eid=%u from slot %u not run -- its disc could not be carried; the "
                        "kerfur serves slot %u", verb, eid, requesterSlot, static_cast<unsigned>(serves));
                Answer(requesterSlot, requestId, KR::Unavailable);
                return;
            }
        }
    }
    if (!HostRunVerb(actor, coop::players::Registry::Get().Local(), verb, requesterSlot)) {
        hand.reset();
        UE_LOGW("kerfur_command: verb '%ls' eid=%u did not run -- the kerfur serves slot %u", verb, eid,
                static_cast<unsigned>(restoreService()));
        Answer(requesterSlot, requestId, KR::Unavailable);
        return;
    }
    const bool copyGone = hand && hand->Consumed();
    if (hand) hand->Close();   // the puppet's hand is its own again, and a copy the verb left is gone, before anyone is told
    // The verb latches the disc into the Omega just before it destroys the copy, so a copy that is gone with the Omega
    // holding nothing was removed by something else, and the client keeps its disc.
    bool omegaHolds = false;
    const bool consumed = copyGone && K::ReadHasFloppy(actor, omegaHolds) && omegaHolds;
    if (copyGone && !consumed) {
        UE_LOGW("kerfur_command: slot %u's carried disc copy is gone but the Omega holds no disc -- not consumed, the "
                "client keeps its disc", static_cast<unsigned>(requesterSlot));
        hand.reset();
        Answer(requesterSlot, requestId, KR::Unavailable);
        return;
    }
    hand.reset();
    UE_LOGI("kerfur_command: HOST ran verb '%ls' eid=%u (requester slot %u, serving slot %u)%s", verb, eid,
            requesterSlot, static_cast<unsigned>(requesterSlot),
            consumed ? ", the Omega took the carried disc" : "");
    if (consumed) coop::kerfur_command_answer::SendConsumed(LoadSession(), requesterSlot, requestId);
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
void RunRequest(const Queued& q, uint8_t senderPeerSlot) {
    const kcw::Request& req = q.req;
    const coop::net::KerfurCommandPayload& payload = req.head;
    ++g_runs[senderPeerSlot];
    const auto eid = static_cast<ElementId>(payload.elementId);
    auto* el = coop::element::MirrorManager<coop::element::Npc>::Instance().Get(eid);
    void* actor = el ? el->GetActor() : nullptr;
    const int32_t idx = el ? el->GetInternalIdx() : -1;
    if (!actor || !R::IsLiveByIndex(actor, idx)) {
        RefuseAtServe(senderPeerSlot, "no live kerfur (stale)");
        Answer(senderPeerSlot, payload.requestId, KR::Stale);
        return;
    }
    if (!K::IsKerfurActor(actor)) {
        RefuseAtServe(senderPeerSlot, "targets a non-kerfur element");
        Answer(senderPeerSlot, payload.requestId, KR::NotKerfur);
        return;
    }
    if (K::ReadKill(actor)) {  // the BP's own actionName guard (a murderfur refuses the menu)
        RefuseAtServe(senderPeerSlot, "kill mode (SP parity)");
        Answer(senderPeerSlot, payload.requestId, KR::KillMode);
        return;
    }
    // The requester's body must be within an arm's reach of the Omega, by the shared intent authority, as a container
    // act is (coop/props/container_act_host_put.cpp, container_reach::Judge): one anchor sample, the pose-staleness budget
    // added, and the lane's own reach, which is the game's armLength that the use trace is cast with.
    const auto reach = coop::element::IntentTarget::ForClientIntent(*LoadSession(), senderPeerSlot,
                                                                    coop::props::container_reach::kReachUU)
                           .Authorize(actor);
    if (!reach) {
        RefuseAtServe(senderPeerSlot, coop::element::OutcomeName(reach.outcome));
        Answer(senderPeerSlot, payload.requestId,
               reach.outcome == coop::element::IntentOutcome::OutOfReach ? KR::TooFar : KR::Unavailable);
        return;
    }
    if (coop::requester_feedback::Refused()) {  // no scope can stand: the verb would show its feedback on the host
        RefuseAtServe(senderPeerSlot, "requester_feedback's seams are refused");
        Answer(senderPeerSlot, payload.requestId, KR::Unavailable);
        return;
    }
    // The hand lane did not show that disc, or its class is not loaded here: nothing is run.
    if (req.hasRecord && !q.tailOk) {
        RefuseAtServe(senderPeerSlot, "disc tail not judged ok at arrival");
        Answer(senderPeerSlot, payload.requestId, KR::Unavailable);
        return;
    }
    ExecuteHostCommand(eid, actor, static_cast<Command>(payload.command), senderPeerSlot, payload.requestId,
                       req.hasRecord ? &req.record : nullptr);
}

// Each sender's queue head, once a token allows it: waiting while the host cannot serve the sender, else run.
void ServeQueues() {
    for (uint8_t slot = 1; slot < coop::players::kMaxPeers; ++slot) {
        Slot& sl = g_slots[slot];
        sl.queue.Serve(kBudget, coop::net::NowMs, [&](const Queued& q) {
            const kcw::Request& r = q.req;
            if (!CanRunFor(slot)) {
                if (!sl.waitSaid) {
                    sl.waitSaid = true;
                    UE_LOGI("kerfur_command: slot %u's commands wait: the host has no body for it, the Omega's "
                            "seams or requester_feedback's are still settling", static_cast<unsigned>(slot));
                    coop::kerfur_command_answer::SendWaits(LoadSession(), slot, r.head.requestId);
                }
                return coop::net::Served::Wait;
            }
            sl.waitSaid = false;
            sl.preSaid = false;
            RunRequest(q, slot);
            return coop::net::Served::Done;
        });
    }
}

}  // namespace

Command CommandFromActionName(const std::wstring& name) {
    const auto is = [&](const wchar_t* verb) { return _wcsicmp(name.c_str(), verb) == 0; };
    if (is(L"follow"))           return Command::Follow;
    if (is(L"idle"))             return Command::Idle;
    if (is(L"patrol"))           return Command::Patrol;
    if (is(L"fix_servers"))      return Command::FixServers;
    if (is(L"get_reports"))      return Command::GetReports;
    if (is(L"fix_transformers")) return Command::FixTransformers;
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
    // A client's get_reports reads the hand the host cannot see: the held disc's record goes with it. An engine call,
    // which a script-gate PRE on the game thread may make.
    if (isClient && pc.command == Command::GetReports) pc.disc = coop::kerfur_disc_carry::CaptureHeld();
    g_queue.push_back(std::move(pc));
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

void OnCommandChunk(const coop::net::BlobChunkPayload& chunk, uint8_t senderPeerSlot) {
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
    kcw::Request req;
    const char* why = "";
    switch (kcw::OnChunk(chunk, senderPeerSlot, req, &why)) {
        case kcw::Feed::Incomplete: return;
        case kcw::Feed::Malformed:  Refuse(senderPeerSlot, why); return;
        case kcw::Feed::Ok:         break;
    }
    const coop::net::KerfurCommandPayload& payload = req.head;
    if (payload.command > static_cast<uint8_t>(Command::FixTransformers)) {
        Refuse(senderPeerSlot, "unknown command");
        return;
    }
    // Every legitimate target is a host-range element; an out-of-range id cannot change while the request waits. A
    // well-formed command naming none is answered (Stale), once a streak like a full queue: it was cancelled at the
    // seat, and a sender that keeps naming them is flooding and gets the one answer, not one per packet.
    if (!coop::element::Registry::IsAllowedHostAllocatedEid(static_cast<ElementId>(payload.elementId))) {
        Refuse(senderPeerSlot, "eid outside the host range");
        AnswerOnceAStreak(senderPeerSlot, payload.requestId, KR::Stale);
        return;
    }
    // A disc tail is judged once, at arrival, against what the sender's hand lane showed then; a hostile one is dropped
    // and counted, an honest one the host cannot serve yet keeps its verdict and is answered, and counted, at its turn.
    bool tailOk = true;
    if (req.hasRecord) {
        const auto verdict = coop::kerfur_disc_carry::JudgeTail(req.record, senderPeerSlot, coop::net::NowMs(), &why);
        if (verdict == coop::kerfur_disc_carry::TailVerdict::Malformed) {
            Refuse(senderPeerSlot, why);
            return;
        }
        tailOk = verdict == coop::kerfur_disc_carry::TailVerdict::Ok;
    }
    const uint32_t requestId = payload.requestId;
    Slot& sl = g_slots[senderPeerSlot];
    if (sl.queue.Push(Queued{std::move(req), tailOk}, kMaxPending)) return;
    Refuse(senderPeerSlot, "queue full");
    AnswerOnceAStreak(senderPeerSlot, requestId, KR::QueueFull);
}

void NoteRefused(uint8_t senderPeerSlot, const char* why) { Refuse(senderPeerSlot, why); }

void Tick() {
    auto* s = LoadSession();
    if (!s) return;

    // 1) drain recorded actions (the seat cancelled the local dispatch; we act here).
    std::vector<PendingCmd> ready;
    if (!g_queue.empty()) ready.swap(g_queue);
    for (const PendingCmd& pc : ready) {
        if (!pc.actor || !R::IsLiveByIndex(pc.actor, pc.internalIdx)) continue;  // died before drain
        if (pc.isClient) {
            const ElementId eid = FindWireEidForNpcActor(pc.actor);
            if (eid == kInvalidId) {
                // The seat cancelled this machine's own copy, so the player is told here, with no send.
                UE_LOGW("kerfur_command[client]: command %u on an UNTRACKED kerfur -- no request "
                        "(not adopted yet / rogue local)", static_cast<unsigned>(pc.command));
                coop::kerfur_command_answer::ShowLocal(static_cast<uint8_t>(KR::Unavailable));
                continue;
            }
            kcw::Request req;
            req.head.elementId = static_cast<uint32_t>(eid);
            req.head.command   = static_cast<uint8_t>(pc.command);
            req.head.requestId = coop::kerfur_command_answer::NextRequestId();
            if (pc.disc) {
                req.hasRecord = true;
                req.record = pc.disc->record;
            }
            if (!s->connected() || !kcw::Send(s, req)) {
                if (!g_saidSendFailed) {
                    g_saidSendFailed = true;
                    UE_LOGW("kerfur_command[client]: command %u eid=%u did not go to the host (said once)",
                            static_cast<unsigned>(pc.command), eid);
                }
                coop::kerfur_command_answer::ShowLocal(static_cast<uint8_t>(KR::Unavailable));
                continue;
            }
            // A pledge is made at the send, with the id its answer will name; one that did not go is never made.
            coop::kerfur_command_answer::HoldSent(req.head.requestId,
                                                  pc.disc ? std::optional<coop::kerfur_disc_carry::Pledge>(pc.disc->pledge)
                                                          : std::nullopt);
            UE_LOGI("kerfur_command[client]: sent command=%u eid=%u request=%u%s",
                    static_cast<unsigned>(pc.command), eid, req.head.requestId, pc.disc ? " with the held disc" : "");
        } else {
            // The host's own menu use, of an Omega serving a client: requester = host; hand it back and run it. The
            // eid is for the log only, so an untracked Omega still runs.
            const ElementId eid = coop::npc_sync::GetNpcIdForActor(pc.actor);
            ExecuteHostCommand(eid, pc.actor, pc.command, coop::players::kPeerIdHost);
        }
    }

    // 2) each sender's queued request, once its pace and its body allow; 3) the refusals' summed line once its
    // window has passed, both only on a running host: the queues are the host's, and the roster's bringup resets them
    // off this thread, so no Tick touches them outside a running host session; 4) the startKill watch, until settled.
    if (s->running() && s->role() == coop::net::Role::Host) {
        kcw::Sweep();
        coop::kerfur_command_answer::TickOwed(s);
        ServeQueues();
        g_tally.Flush(coop::net::RefusalTally::Clock::now());
    }
    SettleKillWatch();
}

uint32_t RunsFor(int slot) { return slot >= 1 && slot < coop::players::kMaxPeers ? g_runs[slot] : 0; }
uint32_t RefusedFor(int slot) { return slot >= 1 && slot < coop::players::kMaxPeers ? g_refused[slot] : 0; }

void OnDisconnect() {
    g_queue.clear();
    g_tally.Clear();
    g_saidSendFailed = false;
    kcw::OnDisconnect();
    coop::kerfur_command_answer::OnDisconnect();
}

}  // namespace coop::kerfur_command
