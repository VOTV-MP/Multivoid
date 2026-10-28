// coop/props/container_act_host.cpp -- see coop/props/container_act_host.h.

#include "coop/props/container_act_host.h"

#include "coop/net/blob_chunks.h"
#include "coop/net/intent_queue.h"
#include "coop/net/net_clock.h"
#include "coop/net/refusal_tally.h"
#include "coop/net/session.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/container_act_wire.h"
#include "coop/props/container_contents_sync.h"
#include "coop/props/container_record_edit.h"
#include "coop/props/container_slice_wire.h"

#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>

namespace coop::props::container_act_host {
namespace {

namespace cas = coop::props::container_act_state;
namespace cre = coop::props::container_record_edit;
namespace caw = coop::props::container_act_wire;
namespace cr  = coop::props::container_reach;
namespace SR  = ue_wrap::save_record;
using coop::net::ContainerActFamily;
using coop::net::ContainerActReason;
using coop::net::ReliableKind;

// Past this many queued requests a sender is refused with QueueFull. Past it for a sender whose world is not ready the
// request is only counted: nothing is sent to a link that cannot take it.
constexpr size_t kCap = 2;
constexpr coop::net::IntentBudget kBudget{4.0f, 2.0f};
constexpr uint64_t kSecondMs = 1000;
constexpr uint64_t kOwedErrorEveryMs = 60000;
constexpr std::chrono::seconds kAssemblyTtl{10};

// A request with the time it was queued, so a head that waits too long can be ended.
struct Queued {
    caw::Request req;
    uint64_t queuedMs = 0;
    // What the head was last held on (WaitCause's text); the queue hands the head out const, so the serving writes it.
    mutable const char* lastWait = nullptr;
};

struct Slot {
    coop::net::IntentQueue<Queued> queue;
    std::unordered_map<uint32_t, uint64_t> truthSentMs;   // per container: when the host's truth last went to this slot
    uint64_t truthAnyMs = 0;                              // any container: when the last one went (0: none yet)
};

// A restore that could not write when it was tried: the hold, and when its next ERROR is due.
struct Owed {
    cas::Hold hold;
    uint64_t nextErrorMs;
};

coop::net::Session* g_session = nullptr;
coop::roster_ledger::PerSlotState<Slot> g_slots;
uint32_t g_tally[static_cast<int>(Tally::kCount)] = {};
coop::net::RefusalTally g_refusals{"container_act"};
coop::blob_chunks::Assembler g_requestAsm;
coop::blob_chunks::Assembler g_commitAsm;
std::vector<Owed> g_restoreOwed;
uint64_t g_nextSecondMs = 0;

bool InRoster(int slot) { return slot >= 1 && slot < coop::net::kMaxPeers; }
bool IsHostRole() { return g_session && g_session->role() == coop::net::Role::Host; }
bool IsHost() { return g_session && g_session->running() && g_session->role() == coop::net::Role::Host; }
coop::net::RefusalTally::Clock::time_point Now() { return coop::net::RefusalTally::Clock::now(); }
void Bump(Tally t) { ++g_tally[static_cast<int>(t)]; }

const char* FamilyName(uint8_t family) {
    switch (static_cast<ContainerActFamily>(family)) {
        case ContainerActFamily::Take:      return "take";
        case ContainerActFamily::PutRecord: return "put-record";
        case ContainerActFamily::PutProp:   return "put-prop";
    }
    return "unknown";
}

// MTA's vehicle entry reserves the seat at the request, confirms it, and commits with a re-check (CGame.cpp:3148-3153,
// 3419-3445). Ours holds the record itself, because a take shifts every later index.
//
// Put a hold's record back where it was. Done: said. NoElement: the container is gone and the record with it. Any other
// result is a write that cannot happen now: the hold is owed and retried, never dropped while the session runs.
void Restore(cas::Hold h, const char* why) {
    const cre::EditResult r = cre::InsertAt(h.eid, h.index, h.record, cre::Source::Host);
    if (r == cre::EditResult::Done) {
        UE_LOGI("container_act: slot=%u id=%u eid=%u %s, restored", static_cast<unsigned>(h.slot),
                static_cast<unsigned>(h.id), static_cast<unsigned>(h.eid), why);
        return;
    }
    if (r == cre::EditResult::NoElement) {
        Bump(Tally::TargetGone);
        UE_LOGI("container_act: slot=%u id=%u eid=%u %s, the container is gone and the record with it",
                static_cast<unsigned>(h.slot), static_cast<unsigned>(h.id), static_cast<unsigned>(h.eid), why);
        return;
    }
    Bump(Tally::RestoreOwed);
    UE_LOGE("container_act: slot=%u id=%u eid=%u %s -- the restore of a %ls at index %u failed (%s); owed, retried "
            "every second", static_cast<unsigned>(h.slot), static_cast<unsigned>(h.id), static_cast<unsigned>(h.eid),
            why, h.record.className.c_str(), static_cast<unsigned>(h.index), cre::EditResultName(r));
    g_restoreOwed.push_back(Owed{std::move(h), coop::net::NowMs() + kOwedErrorEveryMs});
}

// Try every owed restore once; an ERROR for one still owed once a minute.
void RetryOwed(uint64_t now) {
    for (size_t i = 0; i < g_restoreOwed.size();) {
        Owed& o = g_restoreOwed[i];
        const cas::Hold& h = o.hold;
        const cre::EditResult r = cre::InsertAt(h.eid, h.index, h.record, cre::Source::Host);
        if (r == cre::EditResult::Done || r == cre::EditResult::NoElement) {
            if (r == cre::EditResult::NoElement) Bump(Tally::TargetGone);
            UE_LOGI("container_act: slot=%u id=%u eid=%u owed restore %s", static_cast<unsigned>(h.slot),
                    static_cast<unsigned>(h.id), static_cast<unsigned>(h.eid),
                    r == cre::EditResult::Done ? "done" : "dropped, the container is gone");
            g_restoreOwed.erase(g_restoreOwed.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (now >= o.nextErrorMs) {
            o.nextErrorMs = now + kOwedErrorEveryMs;
            UE_LOGE("container_act: eid=%u index=%u: a %ls is still owed back to the container (%s)",
                    static_cast<unsigned>(h.eid), static_cast<unsigned>(h.index), h.record.className.c_str(),
                    cre::EditResultName(r));
        }
        ++i;
    }
}

bool Answer(uint8_t slot, const coop::net::ContainerActReplyPayload& reply) {
    return g_session->SendReliableToSlot(slot, ReliableKind::ContainerActReply, &reply, sizeof(reply));
}

coop::net::ContainerActReplyPayload ReplyOf(internal::Verdict v, uint16_t id) {
    switch (v) {
        case internal::Verdict::Grant:       return {id, 1, static_cast<uint8_t>(ContainerActReason::Granted)};
        case internal::Verdict::Reach:       return {id, 0, static_cast<uint8_t>(ContainerActReason::Reach)};
        case internal::Verdict::WrongFamily: return {id, 0, static_cast<uint8_t>(ContainerActReason::WrongFamily)};
        case internal::Verdict::Gone:
        case internal::Verdict::Wait:        break;
    }
    return {id, 0, static_cast<uint8_t>(ContainerActReason::Gone)};
}

// The host's truth of a container to a slot whose view it just refused: at most one per kTruthEveryMs per container and
// one per kTruthSlotEveryMs per slot across containers, so a forged address buys no more than one container blob per
// interval and a flood of distinct addresses no more than the slot's floor.
void SendTruth(uint8_t slot, uint32_t eid) {
    Slot& s = g_slots[slot];
    const uint64_t now = coop::net::NowMs();
    if (!internal::SlotTruthDue(s.truthAnyMs, now)) return;
    const auto it = s.truthSentMs.find(eid);
    if (it != s.truthSentMs.end() && !internal::TruthDue(it->second, now)) return;
    s.truthSentMs[eid] = now;
    s.truthAnyMs = now;
    coop::props::container_contents_sync::SendTruthToSlot(eid, slot);
}

// Reads Decide's inputs in Decide's order and stops at the first that decides, so a request that must wait reads only
// what the wait needs. The record lookup comes last: it reads and hashes the whole container.
void ReadInputs(uint8_t slot, const caw::Request& q, internal::Inputs& in, uint16_t& foundIndex) {
    in.worldReady = g_session->IsSlotWorldReady(slot);
    if (!in.worldReady) return;
    in.served = q.family == static_cast<uint8_t>(ContainerActFamily::Take);
    if (!in.served) return;
    in.holdsWait = cas::Live().WaitsOnHolds(q.family != static_cast<uint8_t>(ContainerActFamily::Take), q.eid, slot);
    if (in.holdsWait) return;
    in.reach = cr::Judge(*g_session, slot, q.eid).reach;
    if (in.reach == cr::Reach::NoBody) return;
    switch (cre::Probe(q.eid)) {
        case cre::EditResult::Done:     in.container = internal::Container::Ready; break;
        case cre::EditResult::NotReady: in.container = internal::Container::NotReady; break;
        default:                        in.container = internal::Container::Gone; break;
    }
    if (in.container != internal::Container::Ready || in.reach == cr::Reach::Unreachable) return;
    // Past Probe the only failure of the read is Unreadable, a count over the cap: permanent, so the record is gone.
    std::vector<SR::SaveRecord> recs;
    if (cre::ReadRecords(q.eid, recs, /*neuterNested=*/true) != cre::EditResult::Done) return;
    // The honest address is the record at the named index: hash that one alone, and the rest only on a mismatch.
    if (q.index < recs.size() && coop::props::container_slice_wire::RecordHash(recs[q.index]) == q.hash) {
        in.found = true;
        foundIndex = q.index;
        return;
    }
    std::vector<uint64_t> hashes;
    hashes.reserve(recs.size());
    for (const SR::SaveRecord& r : recs) hashes.push_back(coop::props::container_slice_wire::RecordHash(r));
    const std::optional<size_t> at = internal::FindRecord(hashes, q.index, q.hash);
    if (!at) return;
    in.found = true;
    foundIndex = static_cast<uint16_t>(*at);
}

// What the head waits on, for the line that ends the wait.
const char* WaitCause(const internal::Inputs& in) {
    if (!in.worldReady) return "the sender's world is not ready";
    if (in.holdsWait) return "a hold stands in its way";
    if (in.reach == cr::Reach::NoBody) return "the sender's body has no pose";
    return "the container is not ready";
}

// A head that cannot be judged yet stays at the front, until kWaitBoundMs after it was queued; then it is answered
// Gone. A client asks only from a world it plays in, so the not-ready wait is a race measured in frames, never a
// world load.
coop::net::Served WaitOrEnd(uint8_t slot, const Queued& item, const internal::Inputs& in) {
    const uint64_t now = coop::net::NowMs();
    if (!internal::WaitExpired(item.queuedMs, now)) {
        item.lastWait = WaitCause(in);
        return coop::net::Served::Wait;
    }
    Bump(Tally::WaitExpired);
    Answer(slot, ReplyOf(internal::Verdict::Gone, item.req.id));
    g_refusals.Note(slot, "wait expired", Now());
    UE_LOGI("container_act: slot=%u id=%u eid=%u %s -> wait expired after %llu ms: %s", static_cast<unsigned>(slot),
            static_cast<unsigned>(item.req.id), static_cast<unsigned>(item.req.eid), FamilyName(item.req.family),
            static_cast<unsigned long long>(now - item.queuedMs), WaitCause(in));
    return coop::net::Served::Done;
}

// A head served after it waited this long says what it waited on.
constexpr uint64_t kWaitedLineMs = 1000;

void NoteWaited(uint8_t slot, const Queued& item) {
    const uint64_t waited = coop::net::NowMs() - item.queuedMs;
    if (waited < kWaitedLineMs) return;
    UE_LOGI("container_act: slot=%u id=%u eid=%u waited %llu ms (last wait: %s)", static_cast<unsigned>(slot),
            static_cast<unsigned>(item.req.id), static_cast<unsigned>(item.req.eid),
            static_cast<unsigned long long>(waited), item.lastWait ? item.lastWait : "none");
}

// One queued request, judged.
coop::net::Served ServeHead(uint8_t slot, const Queued& item) {
    const caw::Request& q = item.req;
    internal::Inputs in;
    uint16_t foundIndex = 0;
    ReadInputs(slot, q, in, foundIndex);

    internal::Verdict v = internal::Decide(in);
    if (v == internal::Verdict::Wait) return WaitOrEnd(slot, item, in);

    if (v == internal::Verdict::Grant) {
        SR::SaveRecord taken;
        switch (cre::RemoveAt(q.eid, foundIndex, q.hash, taken)) {
            case cre::EditResult::Done:     break;
            case cre::EditResult::NotReady: return WaitOrEnd(slot, item, in);
            default:                        v = internal::Verdict::Gone; break;   // moved under the lookup
        }
        if (v == internal::Verdict::Grant) {
            cas::Hold h;
            h.kind = cas::HoldKind::Escrow;
            h.slot = slot;
            h.id = q.id;
            h.eid = q.eid;
            h.index = foundIndex;
            h.record = std::move(taken);
            h.grantedMs = coop::net::NowMs();
            if (!cas::Live().Open(h)) {   // cannot happen past holdsWait; the record must not be lost if it does
                Bump(Tally::GrantUnsent);
                NoteWaited(slot, item);
                Restore(std::move(h), "hold not opened");
                Answer(slot, ReplyOf(internal::Verdict::Gone, q.id));   // so the client does not wait out its 10 s
                return coop::net::Served::Done;
            }
        }
    }

    NoteWaited(slot, item);
    const bool sent = Answer(slot, ReplyOf(v, q.id));
    if (v == internal::Verdict::Grant && !sent) {
        Bump(Tally::GrantUnsent);
        if (std::optional<cas::Hold> h = cas::Live().Close(slot, q.id)) Restore(std::move(*h), "grant not sent");
    } else if (v != internal::Verdict::Grant) {
        g_refusals.Note(slot, internal::VerdictName(v), Now());
    }
    if (v == internal::Verdict::Gone && in.container == internal::Container::Ready) SendTruth(slot, q.eid);
    UE_LOGI("container_act: slot=%u id=%u eid=%u %s -> %s", static_cast<unsigned>(slot), static_cast<unsigned>(q.id),
            static_cast<unsigned>(q.eid), FamilyName(q.family),
            v == internal::Verdict::Grant && !sent ? "grant-unsent" : internal::VerdictName(v));
    return coop::net::Served::Done;
}

}  // namespace

namespace internal {

Verdict Decide(const Inputs& in) {
    if (!in.worldReady) return Verdict::Wait;
    if (!in.served) return Verdict::WrongFamily;
    if (in.holdsWait) return Verdict::Wait;
    if (in.reach == cr::Reach::NoBody) return Verdict::Wait;
    if (in.container == Container::Gone) return Verdict::Gone;
    if (in.container == Container::NotReady) return Verdict::Wait;
    if (in.reach == cr::Reach::Unreachable) return Verdict::Reach;
    if (!in.found) return Verdict::Gone;
    return Verdict::Grant;
}

bool CommitMatches(const cas::Hold* h, uint32_t commitEid, bool hasRecord) {
    return h && h->kind == cas::HoldKind::Escrow && h->eid == commitEid && !hasRecord;
}

CommitStep DecideCommit(bool matched, uint8_t committed) {
    if (!matched) return CommitStep::BadCommit;
    return committed == 1 ? CommitStep::Done : CommitStep::Abort;
}

std::optional<size_t> FindRecord(const std::vector<uint64_t>& hashes, uint16_t index, uint64_t hash) {
    if (index < hashes.size() && hashes[index] == hash) return index;
    const auto it = std::find(hashes.begin(), hashes.end(), hash);
    if (it == hashes.end()) return std::nullopt;
    return static_cast<size_t>(it - hashes.begin());
}

bool TruthDue(uint64_t lastSentMs, uint64_t nowMs) { return nowMs >= lastSentMs + kTruthEveryMs; }

bool SlotTruthDue(uint64_t lastAnyMs, uint64_t nowMs) {
    return lastAnyMs == 0 || nowMs >= lastAnyMs + kTruthSlotEveryMs;
}

bool WaitExpired(uint64_t queuedMs, uint64_t nowMs) { return nowMs >= queuedMs + kWaitBoundMs; }

const char* VerdictName(Verdict v) {
    switch (v) {
        case Verdict::Wait:        return "wait";
        case Verdict::Gone:        return "gone";
        case Verdict::Reach:       return "reach";
        case Verdict::WrongFamily: return "wrong-family";
        case Verdict::Grant:       return "grant";
    }
    return "?";
}

const char* CommitName(CommitStep s) {
    switch (s) {
        case CommitStep::BadCommit: return "bad";
        case CommitStep::Done:      return "done";
        case CommitStep::Abort:     return "abort";
    }
    return "?";
}

}  // namespace internal

void Install(coop::net::Session* session) { g_session = session; }

void Tick() {
    if (!IsHost()) return;
    UE_ASSERT_GAME_THREAD("container_act_host::Tick");
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        g_slots[slot].queue.Serve(kBudget, coop::net::NowMs,
                                  [&](const Queued& q) { return ServeHead(slot, q); });
    }
    g_refusals.Flush(Now());

    if (cas::Live().Count() == 0 && g_restoreOwed.empty() && g_requestAsm.Idle() && g_commitAsm.Idle()) return;
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextSecondMs) return;
    g_nextSecondMs = now + kSecondMs;

    if (!g_requestAsm.Idle()) g_requestAsm.Sweep(std::chrono::steady_clock::now(), kAssemblyTtl);
    if (!g_commitAsm.Idle()) g_commitAsm.Sweep(std::chrono::steady_clock::now(), kAssemblyTtl);

    // The owed restores first, so one owed this tick is not tried twice in its first second.
    RetryOwed(now);
    // MTA's reservation ends only by the client (CGame.cpp:3419-3445). Ours also ends by itself after 30 s, because the
    // effect runs on the client and the host cannot see it.
    for (cas::Hold& h : cas::Live().TakeExpired(now)) Restore(std::move(h), "hold expired after 30 s");
    // A container destroyed under a hold takes the record with it.
    for (const uint32_t eid : cas::Live().HeldEids()) {
        if (cre::Probe(eid) != cre::EditResult::NoElement) continue;
        for (const cas::Hold& h : cas::Live().TakeForEid(eid)) {
            Bump(Tally::TargetGone);
            UE_LOGI("container_act: slot=%u id=%u eid=%u the container is gone under its hold",
                    static_cast<unsigned>(h.slot), static_cast<unsigned>(h.id), static_cast<unsigned>(h.eid));
        }
    }
}

void OnRequestChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    if (!IsHost()) return;
    UE_ASSERT_GAME_THREAD("container_act_host::OnRequestChunk");
    if (!InRoster(senderSlot)) return;   // the dispatcher counted it
    std::vector<uint8_t> blob;
    if (!g_requestAsm.OnChunk(p, senderSlot, blob)) return;   // incomplete

    caw::Request req;
    const char* why = "";
    switch (caw::ParseRequest(blob, req, &why)) {
        case caw::Parse::Malformed:
            Bump(Tally::Malformed);
            g_refusals.Note(senderSlot, why, Now());
            return;
        case caw::Parse::UnknownFamily:
            // Answered at once and never queued; a sender whose world is not ready is counted and not answered.
            if (g_session->IsSlotWorldReady(senderSlot)) {
                Answer(senderSlot, ReplyOf(internal::Verdict::WrongFamily, req.id));
                g_refusals.Note(senderSlot, internal::VerdictName(internal::Verdict::WrongFamily), Now());
            } else {
                Bump(Tally::UnknownFamilyUnready);
            }
            return;
        case caw::Parse::Ok:
            break;
    }
    const uint16_t id = req.id;
    if (!g_slots[senderSlot].queue.Push(Queued{std::move(req), coop::net::NowMs()}, kCap)) {
        if (!g_session->IsSlotWorldReady(senderSlot)) {
            Bump(Tally::QueueFullUnready);
            return;
        }
        Answer(senderSlot, coop::net::ContainerActReplyPayload{id, 0,
                                                              static_cast<uint8_t>(ContainerActReason::QueueFull)});
        g_refusals.Note(senderSlot, "queue full", Now());
    }
}

void OnCommitChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    if (!IsHost()) return;
    UE_ASSERT_GAME_THREAD("container_act_host::OnCommitChunk");
    if (!InRoster(senderSlot)) return;
    std::vector<uint8_t> blob;
    if (!g_commitAsm.OnChunk(p, senderSlot, blob)) return;   // incomplete

    caw::Commit c;
    const char* why = "";
    if (caw::ParseCommit(blob, c, &why) != caw::Parse::Ok) {
        Bump(Tally::Malformed);
        g_refusals.Note(senderSlot, why, Now());
        return;
    }
    // No act re-judge here: the commit only names the hold it ends. A Reservation never matches in this package.
    const bool matched = internal::CommitMatches(cas::Live().Match(senderSlot, c.id), c.eid, c.hasRecord);
    const internal::CommitStep step = internal::DecideCommit(matched, c.committed);
    UE_LOGI("container_act: slot=%u id=%u eid=%u commit -> %s", static_cast<unsigned>(senderSlot),
            static_cast<unsigned>(c.id), static_cast<unsigned>(c.eid), internal::CommitName(step));
    if (step == internal::CommitStep::BadCommit) {
        // An unmatched commit owns nothing; a hold named with a wrong eid stays open for the real commit.
        Bump(Tally::BadCommit);
        g_refusals.Note(senderSlot, "bad commit", Now());
        return;
    }
    std::optional<cas::Hold> closed = cas::Live().Close(senderSlot, c.id);
    if (step == internal::CommitStep::Abort && closed) Restore(std::move(*closed), "aborted");
}

// A departed sender's holds end with it.
void OnPeerGone(uint8_t slot) {
    if (!IsHostRole() || !InRoster(slot)) return;
    g_requestAsm.ClearSlot(slot);
    g_commitAsm.ClearSlot(slot);
    for (cas::Hold& h : cas::Live().TakeForSlot(slot)) Restore(std::move(h), "the sender left");
}

void OnDisconnect() {
    // Owed restores first, then the holds: a restore that fails here is owed and not tried twice.
    RetryOwed(coop::net::NowMs());
    for (cas::Hold& h : cas::Live().TakeAll()) Restore(std::move(h), "the link ended");
    // A host whose last client left while the session lives on keeps what is owed and retries it each second; at a stop
    // no Tick follows, so what is still owed is lost, and said.
    if (!(g_session && g_session->running() && IsHostRole())) {
        for (const Owed& o : g_restoreOwed) {
            UE_LOGE("container_act: LOST a %ls owed to eid %u at index %u: the session ended before the container "
                    "took it back", o.hold.record.className.c_str(), static_cast<unsigned>(o.hold.eid),
                    static_cast<unsigned>(o.hold.index));
        }
        g_restoreOwed.clear();
    }
    std::string tallies;   // the session's refusals and losses, said once at its end
    for (int i = 0; i < static_cast<int>(Tally::kCount); ++i) {
        const Tally t = static_cast<Tally>(i);
        if (!TallyOf(t)) continue;
        tallies += ' ';
        tallies += TallyName(t);
        tallies += '=';
        tallies += std::to_string(TallyOf(t));
    }
    if (!tallies.empty()) UE_LOGI("container_act: this session:%s", tallies.c_str());
    g_requestAsm.Clear();
    g_commitAsm.Clear();
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) g_slots[slot] = Slot{};
    std::fill(std::begin(g_tally), std::end(g_tally), 0u);
    g_refusals.Clear();
}

void NoteBadSender() { Bump(Tally::BadSender); }

void NoteShortPayload(uint8_t senderSlot) {
    Bump(Tally::ShortPayload);
    g_refusals.Note(senderSlot, "payload too short", Now());
}

const char* TallyName(Tally t) {
    static const char* const kNames[] = {"target_gone", "bad_commit", "grant_unsent", "queue_full_unready", "bad_sender",
                                         "short_payload", "malformed", "unknown_family_unready", "restore_owed",
                                         "wait_expired"};
    static_assert(std::size(kNames) == static_cast<size_t>(Tally::kCount), "a name for every tally");
    return t < Tally::kCount ? kNames[static_cast<int>(t)] : "";
}

uint32_t TallyOf(Tally t) { return t < Tally::kCount ? g_tally[static_cast<int>(t)] : 0; }

}  // namespace coop::props::container_act_host
