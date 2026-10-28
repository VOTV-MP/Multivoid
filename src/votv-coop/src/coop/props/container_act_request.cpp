// coop/props/container_act_request.cpp -- see coop/props/container_act_request.h.

#include "coop/props/container_act_request.h"
#include "coop/props/container_act_request_internal.h"

#include "coop/comms/chat_feed.h"
#include "coop/element/registry.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/props/container_act_state.h"
#include "coop/props/container_birth.h"
#include "coop/props/container_contents_io.h"
#include "coop/props/container_contents_sync.h"
#include "coop/props/container_slice_wire.h"
#include "coop/session/net_pump.h"

#include "l10n/l10n.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace coop::props::container_act_request {

using namespace detail;

namespace {

namespace caw = coop::props::container_act_wire;
namespace ci  = ue_wrap::container_inventory;
namespace cio = coop::props::container_contents_io;
namespace csw = coop::props::container_slice_wire;
namespace cv  = ue_wrap::container_view;
namespace EL  = coop::element;
namespace SR  = ue_wrap::save_record;
namespace sg  = ue_wrap::script_gate;
using coop::net::ContainerActFamily;
using coop::net::ContainerActReason;
using coop::net::ReliableKind;

// The take family's watch: the slot's own pressButton, entered by the player's click or by the take hotkey.
UE_FACT_FN(kRowTake, "container_act", L"uicomp_playerInvContainerSlot_C", L"pressButton", Declared);
constexpr int kTagTake = 0x43414331;   // "CAC1"

// A refused commit is sent again at most this often.
constexpr uint64_t kCommitRetryMs = 250;

// A grant older than the host's hold deadline less this is not replayed. The host restores the record by itself at its
// deadline, so a replay after that makes the item twice. The age is counted from the reply's receipt here, which
// underestimates the grant's age by at most the one-way latency; the margin covers that. The replay runs inline at the
// receipt today, so the guard fires only for a replay held on the client (the drill's hold, or any future deferral):
// that is why it exists.
constexpr uint64_t kGrantMarginMs = 5000;

// The replay's apply latch, for the length of one synchronous verb call: the verb watches ask it whether a verb is the
// replay's.
struct Latch {
    bool on = false;
};
Latch g_latch;

uint32_t g_takeDoneUnread = 0;   // takes committed done although the press's count could not be read back
uint32_t g_staleGrantAborts = 0; // grants aborted for their age instead of replayed
uint8_t g_lastRefusal = 0;       // the reason of the refusal of the open or last ask, 0 when none
bool g_holdReplay = false;       // dev (dev:: below): a take's or a put's grant parks its replay
bool g_doubleAbort = false;      // dev: a take's abort commit is sent twice
dev::Take g_lastTake;            // the last take's commit as it was sent; the drills read it
uint32_t g_acts[3] = {};   // done, aborted, refused: how the client's acts ended (dev::ActsDone ...)
dev::Put g_lastPut;        // the last put act's stand, as the put seats noted it; the drills read it

// A request whose wait ran out, kept until the host answers it, so a grant that still arrives is answered with an
// abort and the host's escrow is always ended. Dropped once the host's own deadline has passed: a grant that late finds
// no hold.
struct Expired {
    uint16_t id = 0;
    uint32_t eid = 0;
    uint64_t atMs = 0;
};

// A commit the transport refused stays at the head and is sent again, so the host's hold is always ended. The order of
// the queue is the order the host must see.
struct QueuedCommit {
    std::vector<uint8_t> blob;
    uint16_t id = 0;
    uint64_t queuedMs = 0;
    uint64_t nextTryMs = 0;
    bool refusedLogged = false;
    bool lateLogged = false;
};

coop::net::Session* g_session = nullptr;
Pending g_rec;
uint16_t g_nextId = 0;   // the last request id; 0 is never an id
uint32_t g_nextRequestSeq = 1;
uint32_t g_nextCommitSeq = 1;
std::vector<Expired> g_expired;
std::deque<QueuedCommit> g_commits;

// ---- the lines ------------------------------------------------------------------------------------------------

// A line is shown at most once per kind per window.
enum Line : size_t { kLineGone, kLineReach, kLineNoSpace, kLineLocked, kLineRefused, kLineQueueFull, kLineCantNow,
                     kLineNotAsked, kLineNotReady, kLineLapsed, kLineBusy,
                     kLineEmptyFirst, kLineCount };
constexpr uint64_t kShowEveryMs = 5000;
uint64_t g_shownAt[kLineCount] = {};
bool g_shown[kLineCount] = {};

void Show(Line which, const char* line) {
    const uint64_t now = coop::net::NowMs();
    if (g_shown[which] && now - g_shownAt[which] < kShowEveryMs) return;
    g_shown[which] = true;
    g_shownAt[which] = now;
    coop::chat_feed::Push(std::string(line), coop::chat_feed::Keep::Transient);
}

// The reply refused the open act: when it is a put, dev::LastPut says so.
void NoteRefusedPut() {
    if (g_rec.open && static_cast<ContainerActFamily>(g_rec.family) == ContainerActFamily::PutRecord &&
        g_lastPut.id == g_rec.id)
        g_lastPut.outcome = dev::PutOutcome::Refused;
}

}  // namespace

namespace detail {

Pending& Rec() { return g_rec; }

Latched::Latched() { g_latch.on = true; }
Latched::~Latched() { g_latch = Latch{}; }

void ShowRefusal(uint8_t reason) {
    switch (static_cast<ContainerActReason>(reason)) {
    case ContainerActReason::Gone:
        Show(kLineGone, l10n::T("That item is gone."));
        return;
    case ContainerActReason::Reach:
        Show(kLineReach, l10n::T("That container is too far away."));
        return;
    case ContainerActReason::NoSpace:
        Show(kLineNoSpace, l10n::T("Not enough space."));
        return;
    case ContainerActReason::Locked:
        Show(kLineLocked, l10n::T("That container is locked."));
        return;
    case ContainerActReason::Refused:
        Show(kLineRefused, l10n::T("That can't go in there."));
        return;
    case ContainerActReason::QueueFull:
        Show(kLineQueueFull, l10n::T("Too many actions at once, try again."));
        return;
    case ContainerActReason::Lapsed:
        Show(kLineLapsed, l10n::T("Try again in a moment."));
        return;
    case ContainerActReason::WrongFamily:
    case ContainerActReason::Granted:
        Show(kLineCantNow, l10n::T("That can't be done right now."));
        return;
    }
}

void ShowBadReason() { Show(kLineCantNow, l10n::T("That can't be done right now.")); }
void ShowNotAsked() { Show(kLineNotAsked, l10n::T("The host could not be asked; try again.")); }
void ShowNotReady() { Show(kLineNotReady, l10n::T("Not ready yet, try again in a moment.")); }
coop::net::Session* LinkedSession() { return g_session && g_session->connected() ? g_session : nullptr; }

void ShowBusy() { Show(kLineBusy, l10n::T("Another player is using that item.")); }
void ShowEmptyFirst() { Show(kLineEmptyFirst, l10n::T("Empty it before putting it in a shared container.")); }

Target SharedTarget(void* container, uint32_t& eid) {
    eid = EL::kInvalidId;
    if (!container || !ci::IsContainer(container)) return Target::Own;
    void* const inv = ci::InventoryOf(container);
    if (!inv || !ci::IsWorldInventory(inv)) return Target::Own;
    eid = EL::Registry::Get().EidForActor(container);
    if (eid == EL::kInvalidId) {
        // A host container the client has not bound yet would make the item twice if acted on natively; one in an
        // announced world with no element is the client's own, and nothing ships for it.
        return coop::net_pump::IsInAnnouncedWorld(container) ? Target::Own : Target::NotReady;
    }
    // A container the client threw: its contents are the thrower's own until its birth slice goes.
    return coop::props::container_birth::IsBirthSlice(eid) ? Target::Own : Target::Shared;
}

// The host's own player beside a client's hold (the pocket's HostPre, prop_act_request.cpp): a put is cancelled on a
// container any client holds anything on, since the restore of an escrow could overflow it; a take is cancelled on
// one a client holds reserved. Everything else runs natively. MTA never refuses its server body for a client's seat
// claim (CGame.cpp:3340 refuses only the second client); here the host player is a participant on the same container,
// whose restore of an escrow could overflow it.
sg::Verdict HostPre(void* container, bool anyHold) {
    if (!container || !ci::IsContainer(container)) return sg::Verdict::Run;
    const EL::ElementId eid = EL::Registry::Get().EidForActor(container);
    if (eid == EL::kInvalidId) return sg::Verdict::Run;
    const bool busy = anyHold ? container_act_state::Live().HoldOn(eid)
                              : container_act_state::Live().ReservedByOther(eid, 0);
    if (!busy) return sg::Verdict::Run;
    ShowBusy();
    return sg::Verdict::Cancel;
}

void Close() { g_rec = Pending{}; }

}  // namespace detail

namespace {

// ---- the record and its commits -------------------------------------------------------------------------------

void DrainCommits() {
    if (!g_session) return;
    const uint64_t now = coop::net::NowMs();
    while (!g_commits.empty()) {
        QueuedCommit& head = g_commits.front();
        if (now < head.nextTryMs) return;
        if (!head.lateLogged && now - head.queuedMs > container_act_state::kHoldDeadlineMs) {
            head.lateLogged = true;
            UE_LOGW("container_act: commit id %u is older than the host's hold deadline -- the host has already ended "
                    "that hold by itself", static_cast<unsigned>(head.id));
        }
        // A retry goes under a fresh seq: the receiver's half-assembly of a refused attempt is swept, not merged.
        if (!coop::blob_chunks::SendBlob(g_session, ReliableKind::ContainerActCommit, g_nextCommitSeq++, head.blob)) {
            if (!head.refusedLogged) {
                head.refusedLogged = true;
                UE_LOGW("container_act: commit id %u refused by the transport -- kept, sent again",
                        static_cast<unsigned>(head.id));
            }
            head.nextTryMs = now + kCommitRetryMs;
            return;
        }
        g_commits.pop_front();
    }
}

}  // namespace

namespace detail {

void NotePut(const Pending& p, dev::PutOutcome outcome, uint16_t index, uint64_t replayHash, const float (&floats)[3]) {
    const dev::Put was = g_lastPut;
    g_lastPut = dev::Put{p.id, static_cast<uint8_t>(p.route + 1), outcome, index, p.hash, replayHash};
    float(&dst)[3] = outcome == dev::PutOutcome::Asked ? g_lastPut.askFloats : g_lastPut.replayFloats;
    std::memcpy(dst, floats, sizeof(dst));
    if (outcome != dev::PutOutcome::Asked && was.id == p.id)
        std::memcpy(g_lastPut.askFloats, was.askFloats, sizeof(was.askFloats));
}

bool QueueCommit(const caw::Commit& c) {
    QueuedCommit q;
    q.blob = caw::PackCommit(c);   // packed once, here
    if (q.blob.empty()) return false;
    ++g_acts[c.committed ? 0 : 1];
    q.id = c.id;
    q.queuedMs = coop::net::NowMs();
    g_commits.push_back(std::move(q));
    DrainCommits();
    return true;
}

void SendCommit(uint16_t id, uint32_t eid, uint8_t committed) {
    caw::Commit c;
    c.id = id;
    c.committed = committed;
    c.eid = eid;
    QueueCommit(c);
    UE_LOGI("container_act: commit id %u eid %u -> %s", static_cast<unsigned>(id), static_cast<unsigned>(eid),
            committed ? "done" : "aborted");
}

}  // namespace detail

namespace {

// The reply matched no open record. One for an expired request ends the host's hold: a grant is answered with an
// abort, a refusal reserved nothing.
void NoteStale(const coop::net::ContainerActReplyPayload& p) {
    const auto it = std::find_if(g_expired.begin(), g_expired.end(), [&](const Expired& e) { return e.id == p.id; });
    if (it == g_expired.end()) return;
    if (p.granted == 1 && p.reason == 0) SendCommit(it->id, it->eid, 0);
    g_expired.erase(it);
}

}  // namespace

namespace internal {

ReplyAction DecideReply(bool pending, uint16_t pendingId, const coop::net::ContainerActReplyPayload& p) {
    if (!pending || p.id != pendingId) return ReplyAction::Stale;
    if (p.granted == 1 && p.reason == 0) return ReplyAction::Granted;
    if (p.granted == 0 && p.reason >= 1 && p.reason < coop::net::kContainerActReasons) return ReplyAction::Refused;
    return ReplyAction::BadReason;
}

const char* ReplyActionName(ReplyAction a) {
    switch (a) {
    case ReplyAction::Stale:     return "stale";
    case ReplyAction::Refused:   return "refused";
    case ReplyAction::BadReason: return "bad-reason";
    case ReplyAction::Granted:   return "granted";
    }
    return "";
}

TakeCheck CheckTake(bool grantFresh, bool slotLive, bool screenShowsContainer, bool sameId, bool hashMatches) {
    return grantFresh && slotLive && screenShowsContainer && sameId && hashMatches ? TakeCheck::Replay : TakeCheck::Abort;
}

uint8_t TakeCommitted(bool pressed, bool afterRead, size_t countBefore, size_t countAfter) {
    if (!pressed) return 0;
    if (!afterRead) return 1;
    return countAfter < countBefore ? 1 : 0;
}

}  // namespace internal

bool Ask(caw::Request r) {
    UE_ASSERT_GAME_THREAD("container_act_request::Ask");
    // A second press while one is pending is the player's double press: cancelled with nothing sent and no line.
    if (g_rec.open || !g_session) return false;
    uint16_t id = ++g_nextId;
    if (id == 0) id = ++g_nextId;
    r.id = id;
    const std::vector<uint8_t> blob = caw::PackRequest(r);
    if (blob.empty()) {   // a record past the byte cap: the host would refuse it, so it is not sent and nothing opens
        UE_LOGW("container_act: request eid %u is not sendable (record past the byte cap)",
                static_cast<unsigned>(r.eid));
        ShowBadReason();
        return false;
    }
    if (!coop::blob_chunks::SendBlob(g_session, ReliableKind::ContainerActRequest, g_nextRequestSeq++, blob)) {
        ShowNotAsked();
        return false;
    }
    g_rec.open = true;
    g_rec.id = id;
    g_rec.eid = r.eid;
    g_rec.family = r.family;
    g_rec.sentMs = coop::net::NowMs();
    g_lastRefusal = 0;   // per press: a refusal read after this ask is this ask's
    return true;
}

namespace {

// A client's click on an inventory-screen slot of a shared container: cancelled, and asked of the host. The slot is
// kept for the replay. Everything that is not such a click runs natively.
// Unlike the pocket's gate there is no mirror-prop rule: the container's element id and its world inventory are the
// test, and one latch serves the one pending act (prop_act_request.cpp:352-360 is the shape it follows).
sg::Verdict PreTake(const sg::Call& c) {
    if (g_latch.on) return sg::Verdict::Run;
    if (!g_session || !g_session->connected()) return sg::Verdict::Run;
    cv::Slot s{};
    if (!cv::ReadSlot(c.object, s)) return sg::Verdict::Run;
    if (g_session->role() == coop::net::Role::Host) return HostPre(s.container, /*anyHold=*/false);
    // The player's own pane, a container this lane does not share and one the client threw take natively.
    EL::ElementId eid = EL::kInvalidId;
    switch (SharedTarget(s.container, eid)) {
    case Target::Own:
        return sg::Verdict::Run;
    case Target::NotReady:
        ShowNotReady();
        return sg::Verdict::Cancel;
    case Target::Shared:
        break;
    }
    void* const inv = ci::InventoryOf(s.container);
    std::vector<SR::SaveRecord> recs;
    if (!cio::ReadRecords(inv, recs, /*neuterNested=*/true) || s.id < 0 || static_cast<size_t>(s.id) >= recs.size()) {
        ShowRefusal(static_cast<uint8_t>(ContainerActReason::Gone));
        return sg::Verdict::Cancel;
    }
    caw::Request r;
    r.family = static_cast<uint8_t>(ContainerActFamily::Take);
    r.eid = eid;
    r.index = static_cast<uint16_t>(s.id);
    r.hash = csw::RecordHash(recs[static_cast<size_t>(s.id)]);
    if (Ask(r)) {
        g_rec.slot.Set(c.object);
        g_rec.container.Set(s.container);
        g_rec.index = r.index;
        g_rec.hash = r.hash;
    }
    // A refused ask has shown its own line, and an open act shows nothing.
    return sg::Verdict::Cancel;
}

// The take's commit goes out: the id, what it said, and how many takes have said it this session.
void SendTakeCommit(uint16_t id, uint32_t eid, uint8_t committed) {
    g_lastTake = dev::Take{id, committed, g_lastTake.count + 1};
    SendCommit(id, eid, committed);
    if (g_doubleAbort && !committed) SendCommit(id, eid, committed);
}

// The granted take: the same slot is pressed again, under the latch, if it still names the record the host granted;
// the commit then says whether the record left the container.
void ReplayTake() {
    cv::Slot s{};
    void* const slot = g_rec.slot.Get();
    const bool slotLive = slot && cv::ReadSlot(slot, s);
    void* const container = g_rec.container.Get();
    void* const inv = container ? ci::InventoryOf(container) : nullptr;
    std::vector<SR::SaveRecord> recs;
    const bool read = inv && cio::ReadRecords(inv, recs, /*neuterNested=*/true);
    const bool hashMatches = read && g_rec.index < recs.size() && csw::RecordHash(recs[g_rec.index]) == g_rec.hash;
    const bool shows = container && cv::Viewed() == container;
    const bool sameId = slotLive && s.id >= 0 && static_cast<uint32_t>(s.id) == g_rec.index;
    const uint64_t grantAge = coop::net::NowMs() - g_rec.grantedMs;
    const bool grantFresh = grantAge < container_act_state::kHoldDeadlineMs - kGrantMarginMs;
    if (internal::CheckTake(grantFresh, slotLive, shows, sameId, hashMatches) == internal::TakeCheck::Abort) {
        if (!grantFresh) {
            ++g_staleGrantAborts;
            UE_LOGW("container_act: take id=%u eid=%u index=%u grant %llu ms old -- not replayed past the host's hold, "
                    "aborted", static_cast<unsigned>(g_rec.id), static_cast<unsigned>(g_rec.eid),
                    static_cast<unsigned>(g_rec.index), static_cast<unsigned long long>(grantAge));
        } else {
            UE_LOGI("container_act: take id=%u eid=%u index=%u not replayed (the slot, the screen or the record "
                    "changed) -> aborted", static_cast<unsigned>(g_rec.id), static_cast<unsigned>(g_rec.eid),
                    static_cast<unsigned>(g_rec.index));
        }
        SendTakeCommit(g_rec.id, g_rec.eid, 0);
        ShowBadReason();   // the click is lost, never the record: the escrow restores it; the player clicks again
        return;
    }
    const size_t countBefore = recs.size();
    bool pressed = false;
    {
        Latched latch;
        pressed = cv::PressSlot(slot);
    }
    // A replay on a shared mirror changes it under the latch, where no verb watch marks it, so the host's next truth for
    // this container must apply even when it equals the last one applied. Only a press that dispatched can have changed it.
    if (pressed) coop::props::container_contents_sync::ForgetApplied(g_rec.eid);
    std::vector<SR::SaveRecord> after;
    void* const invAfter = g_rec.container.Get() ? ci::InventoryOf(g_rec.container.Get()) : nullptr;
    const bool afterRead = invAfter && cio::ReadRecords(invAfter, after, /*neuterNested=*/true);
    const uint8_t done = internal::TakeCommitted(pressed, afterRead, countBefore, after.size());
    if (done && pressed && afterRead && after.size() + 1 < countBefore) {
        UE_LOGW("container_act: take id=%u eid=%u index=%u the count fell %zu -> %zu, by more than the one record",
                static_cast<unsigned>(g_rec.id), static_cast<unsigned>(g_rec.eid), static_cast<unsigned>(g_rec.index),
                countBefore, after.size());
    }
    if (done && pressed && !afterRead) {
        ++g_takeDoneUnread;
        UE_LOGW("container_act: take id=%u eid=%u widget=%u index=%u done UNREAD (%ls): the press ran and the count did "
                "not read back", static_cast<unsigned>(g_rec.id), static_cast<unsigned>(g_rec.eid),
                static_cast<unsigned>(g_rec.slot.Idx()), static_cast<unsigned>(g_rec.index),
                recs[g_rec.index].className.c_str());
    }
    SendTakeCommit(g_rec.id, g_rec.eid, done);
    UE_LOGI("container_act: take id=%u eid=%u index=%u replayed -> %s", static_cast<unsigned>(g_rec.id),
            static_cast<unsigned>(g_rec.eid), static_cast<unsigned>(g_rec.index), done ? "done" : "aborted");
}

enum class WatchState : uint8_t { Pending, Registered, Refused, Settled };
struct Watch {
    const ue_wrap::facts::Function& row;
    int tag;
    sg::PreFn pre;
    WatchState state;
};
std::vector<Watch> g_watches;   // the take's, then the put seats': filled by the first Install
bool g_watchesSettled = false;

// Says once per watch whether the gate took it. The names resolve on the game thread, so a tick drives them.
void SettleWatches() {
    if (g_watchesSettled || g_watches.empty()) return;
    bool registered = false;
    for (const Watch& w : g_watches) registered = registered || w.state == WatchState::Registered;
    if (registered) sg::ResolvePendingNames();
    bool open = false;
    for (Watch& w : g_watches) {
        if (w.state == WatchState::Settled) continue;
        if (w.state == WatchState::Pending) {
            open = true;
        } else if (w.state == WatchState::Registered && sg::NameWatchLive(w.row, w.tag)) {
            w.state = WatchState::Settled;
            UE_LOGI("container_act: watch %ls.%ls live", w.row.Class(), w.row.Name());
        } else if (w.state == WatchState::Refused || sg::NameWatchSettled(w.row, w.tag)) {
            w.state = WatchState::Settled;
            UE_LOGE("container_act: the %ls.%ls watch did not register -- a client's act on a shared container runs "
                    "unasked", w.row.Class(), w.row.Name());
        } else {
            open = true;
        }
    }
    g_watchesSettled = !open;
}

}  // namespace

bool InReplay() { return g_latch.on; }

void Install(coop::net::Session* session) {
    g_session = session;
    if (g_watches.empty()) {
        g_watches.push_back(Watch{kRowTake, kTagTake, &PreTake, WatchState::Pending});
        size_t puts = 0;
        const WatchRow* const rows = PutWatches(puts);
        for (size_t i = 0; i < puts; ++i)
            g_watches.push_back(Watch{rows[i].row, rows[i].tag, rows[i].pre, WatchState::Pending});
    }
    // The gate refuses a watch it cannot keep for the process; a refused one is said dead at the settle.
    for (Watch& w : g_watches) {
        if (w.state != WatchState::Pending) continue;
        w.state = sg::WatchName(w.row, w.tag, w.pre, nullptr) ? WatchState::Registered : WatchState::Refused;
    }
}

void Tick() {
    SettleWatches();
    if (!g_rec.open && g_commits.empty() && g_expired.empty()) return;
    UE_ASSERT_GAME_THREAD("container_act_request::Tick");
    DrainCommits();
    const uint64_t now = coop::net::NowMs();
    for (auto it = g_expired.begin(); it != g_expired.end();) {
        if (now - it->atMs <= container_act_state::kHoldDeadlineMs) {
            ++it;
            continue;
        }
        UE_LOGI("container_act: expired request id %u eid %u dropped, the host's hold deadline has passed",
                static_cast<unsigned>(it->id), static_cast<unsigned>(it->eid));
        it = g_expired.erase(it);
    }
    if (!g_rec.open || g_rec.replayHeld) return;   // a parked replay is a drill's to release
    const uint64_t waited = now - g_rec.sentMs;
    if (waited < container_act_state::kReplyWaitMs) return;
    UE_LOGI("container_act: request id %u eid %u expired after %llu ms", static_cast<unsigned>(g_rec.id),
            static_cast<unsigned>(g_rec.eid), static_cast<unsigned long long>(waited));
    g_expired.push_back(Expired{g_rec.id, g_rec.eid, now});
    ShowNotAsked();
    Close();
}

void OnReply(const coop::net::ContainerActReplyPayload& p) {
    UE_ASSERT_GAME_THREAD("container_act_request::OnReply");
    const internal::ReplyAction action = internal::DecideReply(g_rec.open, g_rec.id, p);
    const uint64_t after = g_rec.open ? coop::net::NowMs() - g_rec.sentMs : 0;
    UE_LOGI("container_act: reply id %u -> %s after %llu ms", static_cast<unsigned>(p.id),
            internal::ReplyActionName(action), static_cast<unsigned long long>(after));
    switch (action) {
    case internal::ReplyAction::Stale:
        NoteStale(p);
        return;
    case internal::ReplyAction::BadReason:
        ++g_acts[2];
        NoteRefusedPut();
        ShowBadReason();
        Close();
        return;
    case internal::ReplyAction::Refused:
        g_lastRefusal = p.reason;
        ++g_acts[2];
        NoteRefusedPut();
        ShowRefusal(p.reason);
        Close();
        return;
    case internal::ReplyAction::Granted:
        // The take and the put-record families replay; any other family has no replay yet, so its grant is answered
        // with an abort: the host's hold is ended, not kept.
        switch (static_cast<ContainerActFamily>(g_rec.family)) {
        case ContainerActFamily::Take:
            g_rec.grantedMs = coop::net::NowMs();
            if (g_holdReplay) {
                g_rec.replayHeld = true;
                return;
            }
            ReplayTake();
            break;
        case ContainerActFamily::PutRecord:
            if (g_holdReplay) {
                g_rec.replayHeld = true;
                return;
            }
            ReplayPut();
            break;
        default:
            SendCommit(g_rec.id, g_rec.eid, 0);
            break;
        }
        Close();
        return;
    }
}

void OnDisconnect() {
    Close();
    g_latch = Latch{};
    // Said once at the session's end: each one trusted that a press took a record it could not count.
    if (dev::TakeDoneUnread())
        UE_LOGW("container_act: this session committed %u take(s) done UNREAD", dev::TakeDoneUnread());
    g_takeDoneUnread = 0;
    g_staleGrantAborts = 0;
    g_lastRefusal = 0;
    g_holdReplay = false;
    g_doubleAbort = false;
    g_lastTake = dev::Take{};
    std::memset(g_acts, 0, sizeof(g_acts));
    g_lastPut = dev::Put{};
    ResetPutLines();
    g_nextId = 0;
    g_nextRequestSeq = 1;
    g_nextCommitSeq = 1;
    g_expired.clear();
    g_commits.clear();
    for (size_t i = 0; i < kLineCount; ++i) g_shown[i] = false;
}

namespace dev {

uint32_t TakeDoneUnread() { return g_takeDoneUnread; }
uint32_t ActsDone() { return g_acts[0]; }
uint32_t ActsAborted() { return g_acts[1]; }
uint32_t ActsRefused() { return g_acts[2]; }

uint16_t PendingId() { return g_rec.open ? g_rec.id : 0; }

uint32_t StaleGrantAborts() { return g_staleGrantAborts; }

uint8_t LastRefusal() { return g_lastRefusal; }

void HoldReplay(bool on) { g_holdReplay = on; }

bool ReplayParked() { return g_rec.open && g_rec.replayHeld; }

void ReleaseReplay() {
    if (!ReplayParked()) return;
    g_rec.replayHeld = false;
    if (static_cast<ContainerActFamily>(g_rec.family) == ContainerActFamily::PutRecord) ReplayPut();
    else ReplayTake();
    Close();
}

void DoubleAbort(bool on) { g_doubleAbort = on; }

Take LastTake() { return g_lastTake; }

Put LastPut() { return g_lastPut; }

const char* PutOutcomeName(PutOutcome o) {
    switch (o) {
    case PutOutcome::None:    return "none";
    case PutOutcome::Asked:   return "asked";
    case PutOutcome::Done:    return "done";
    case PutOutcome::Aborted: return "aborted";
    case PutOutcome::Refused: return "refused";
    }
    return "none";
}

}  // namespace dev

}  // namespace coop::props::container_act_request
