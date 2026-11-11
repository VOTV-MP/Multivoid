// coop/props/container_act_request.cpp -- see coop/props/container_act_request.h.

#include "coop/props/container_act_request.h"
#include "coop/props/container_act_request_internal.h"

#include "coop/comms/chat_feed.h"
#include "coop/element/registry.h"
#include "coop/interactables/atv_trunk_sync.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/props/container_act_state.h"
#include "coop/props/container_birth.h"
#include "coop/props/container_contents_io.h"
#include "coop/props/container_slice_wire.h"
#include "coop/props/container_target.h"
#include "coop/session/net_pump.h"

#include "l10n/l10n.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/actors/inventory_item.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/drone.h"
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
namespace ct  = coop::props::container_target;
namespace cv  = ue_wrap::container_view;
namespace D   = ue_wrap::drone;
namespace EL  = coop::element;
namespace II  = ue_wrap::inventory_item;
namespace R   = ue_wrap::reflection;
namespace sg  = ue_wrap::script_gate;
using coop::net::ContainerActFamily;
using coop::net::ContainerActReason;
using coop::net::ReliableKind;

// The take's watch: the component's removeInventoryItem, which the inventory screen calls on the component an entry
// leaves, for every drag out of a container, a mass selection and a drop into the world alike.
UE_FACT_FN(kRowTake, "container_act", L"inventoryComponent_C", L"removeInventoryItem", Declared);
constexpr int kTagTake = 0x43414331;   // "CAC1"

// A refused commit is sent again at most this often.
constexpr uint64_t kCommitRetryMs = 250;

// The replay's apply latch, for the length of one synchronous verb call: the verb watches ask it whether a verb is the
// replay's.
struct Latch {
    bool on = false;
};
Latch g_latch;

constexpr size_t kMaxTakes = 64;
// A drag's puts waiting at once: a mass selection dropped on the canvas asks one per entry.
constexpr size_t kMaxPuts = 8;

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
std::vector<Pending> g_puts;
std::vector<Take> g_takes;
uint16_t g_nextId = 0;   // the last request id; 0 is never an id
uint32_t g_nextRequestSeq = 1;
uint32_t g_nextCommitSeq = 1;
std::vector<Expired> g_expired;
std::deque<QueuedCommit> g_commits;

// A merge's source entry: the removal of it that follows the merge is the merge's own.
struct MergeNote {
    bool open = false;
    bool asked = false;
    ue_wrap::CachedObjRef inventory;
    uint64_t hash = 0;
};
MergeNote g_mergeNote;

// ---- the lines ------------------------------------------------------------------------------------------------

// A line is shown at most once per kind per window.
enum Line : size_t { kLineGone, kLineReach, kLineNoSpace, kLineLocked, kLineRefused, kLineQueueFull, kLineCantNow,
                     kLineNotAsked, kLineNotReady, kLineLapsed, kLineBusy,
                     kLineCount };
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

}  // namespace

namespace detail {

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

Target SharedTargetOfInventory(void* inventory, uint32_t& eid) {
    eid = EL::kInvalidId;
    if (!inventory) return Target::Own;
    if (D::IsCargo(inventory)) {
        eid = ct::kDroneCargoEid;
        return Target::Shared;
    }
    if (const uint32_t trunk = coop::atv_trunk_sync::EidOfInventory(inventory)) {
        eid = trunk;
        return Target::Shared;
    }
    return ci::IsWorldInventory(inventory) ? SharedTarget(ci::OwnerOf(inventory), eid) : Target::Own;
}

// The host's own put beside a client's hold (the pocket's HostPre, prop_act_request.cpp): a put is cancelled on a
// container any client holds anything on, since the restore of an escrow could overflow it. Everything else runs
// natively. MTA never refuses its server body for a client's seat claim (CGame.cpp:3340 refuses only the second
// client); here the host player is a participant on the same container, whose restore of an escrow could overflow it.
sg::Verdict HostPre(void* container) {
    if (!container || !ci::IsContainer(container)) return sg::Verdict::Run;
    const EL::ElementId eid = EL::Registry::Get().EidForActor(container);
    if (eid == EL::kInvalidId || !container_act_state::Live().HoldOn(eid)) return sg::Verdict::Run;
    ShowBusy();
    return sg::Verdict::Cancel;
}

void NoteMergeSource(void* inventory, uint64_t recordHash, bool asked) {
    g_mergeNote.open = true;
    g_mergeNote.asked = asked;
    g_mergeNote.inventory.Set(inventory);
    g_mergeNote.hash = recordHash;
}

MergeSource ConsumeMergeSource(void* inventory, uint64_t recordHash) {
    if (!g_mergeNote.open || g_mergeNote.hash != recordHash || !g_mergeNote.inventory.Is(inventory))
        return MergeSource::None;
    const MergeSource which = g_mergeNote.asked ? MergeSource::Asked : MergeSource::NotAsked;
    g_mergeNote = MergeNote{};
    return which;
}

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

bool QueueCommit(const caw::Commit& c) {
    QueuedCommit q;
    q.blob = caw::PackCommit(c);   // packed once, here
    if (q.blob.empty()) return false;
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

}  // namespace internal

namespace {

// Sends `r` under a fresh id (ids skip 0), written into it and into `id`. False, with the player's line shown, when the
// record cannot be packed or the transport refused the send.
bool SendRequest(caw::Request& r, uint16_t& id) {
    if (!g_session) return false;
    id = ++g_nextId;
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
    return true;
}

}  // namespace

namespace detail {

bool Ask(caw::Request r, Pending p) {
    UE_ASSERT_GAME_THREAD("container_act_request::Ask");
    if (p.kind == PutKind::HoldAndRun) {
        // A second press while one is pending is the player's double press: cancelled with nothing sent and no line.
        for (const Pending& open : g_puts)
            if (open.kind == PutKind::HoldAndRun) return false;
    } else if (g_puts.size() >= kMaxPuts) {
        ShowRefusal(static_cast<uint8_t>(ContainerActReason::QueueFull));
        return false;
    }
    if (!SendRequest(r, p.id)) return false;
    p.eid = r.eid;
    p.sentMs = coop::net::NowMs();
    g_puts.push_back(std::move(p));
    return true;
}

bool AskTake(caw::Request r, Take t) {
    UE_ASSERT_GAME_THREAD("container_act_request::AskTake");
    if (g_takes.size() >= kMaxTakes) {
        ShowRefusal(static_cast<uint8_t>(ContainerActReason::QueueFull));
        return false;
    }
    if (!SendRequest(r, t.id)) return false;   // the send showed its own line
    t.eid = r.eid;
    t.family = r.family;
    t.sentMs = coop::net::NowMs();
    g_takes.push_back(std::move(t));
    // The mirror changes with the call that follows; the host's next publication or truth is applied whatever the last
    // one was.
    ct::ForgetApplied(r.eid);
    return true;
}

void AttachCopies() {
    for (Take& t : g_takes) {
        if (!t.awaitCopy) continue;
        t.awaitCopy = false;
        t.copy.Set(cv::CopyOf(t.part, t.from.Get()));
    }
}

}  // namespace detail

namespace {

// The player's bag gives the copy back: the drag filed it there before it took the entry out of the container, and the
// entry stays in the container. Nothing to do for an entry the drag did not copy (a craft that consumed it, a drop into
// the world).
void GiveBack(const Take& t) {
    void* const copy = t.copy.Alive() ? t.copy.Raw() : nullptr;
    if (copy && cv::Discard(copy))
        UE_LOGI("container_act: take id=%u eid=%u refused -- the copy is out of the bag again",
                static_cast<unsigned>(t.id), static_cast<unsigned>(t.eid));
}

// A client's drag out of a shared inventory (a world container, the drone's cargo or an ATV's trunk). The screen files the entry's copy
// in the player's bag, then calls removeInventoryItem on the inventory's component; the call runs here at once, so the
// drag goes on without a wait, and the entry's address goes to the host in the same step. The host takes the entry out
// into escrow and grants, or refuses, and the answer decides whether the move stands (OnReply). A take that cannot be
// asked is not left to run: the copy is taken out of the bag again and the call cancelled. Everything that is not a take
// out of a shared inventory runs natively, and so does the removal of a merge's source, which the merge's request covers.
sg::Verdict PreTake(const sg::Call& c) {
    if (!g_session || !g_session->connected() || g_session->role() == coop::net::Role::Host) return sg::Verdict::Run;
    void* const inv = c.object;
    if (!inv || !(D::IsCargo(inv) || ci::IsWorldInventory(inv) || coop::atv_trunk_sync::EidOfInventory(inv)))
        return sg::Verdict::Run;
    static R::ParamOffset sFind{L"itemToFind"};
    static R::ParamOffset sIndex{L"indexToRemove"};
    static R::ParamOffset sBar{L"slotbar"};
    const int32_t findOff = sFind.Of(c.function);
    const int32_t indexOff = sIndex.Of(c.function);
    const int32_t barOff = sBar.Of(c.function);
    std::vector<II::Item> recs;
    if (findOff < 0 || indexOff < 0 || barOff < 0 || !c.locals || !cio::ReadRecords(inv, recs)) {
        static bool sSaid = false;
        if (!sSaid) {
            sSaid = true;
            UE_LOGW("container_act: the take seat could not read removeInventoryItem's parameters or the inventory's "
                    "records -- takes from a shared inventory run unasked");
        }
        return sg::Verdict::Run;
    }
    if (c.locals[barOff] != 0) return sg::Verdict::Run;   // a hotbar slot's array, not an inventory's entries
    // The entry the call blanks, found as the body finds it: the index it is given, else the first entry equal to the one
    // it is given.
    int32_t index = 0;
    std::memcpy(&index, c.locals + indexOff, sizeof(index));
    if (index < 0) {
        const uint8_t* find = sg::OutParamPtr(c, findOff);
        if (!find) find = c.locals + findOff;
        II::Item want;
        if (!II::Read(find, want)) return sg::Verdict::Run;
        const uint64_t wantHash = csw::RecordHash(want);
        for (size_t i = 0; i < recs.size() && index < 0; ++i)
            if (csw::RecordHash(recs[i]) == wantHash) index = static_cast<int32_t>(i);
    }
    if (index < 0 || static_cast<size_t>(index) >= recs.size() || II::IsEmpty(recs[static_cast<size_t>(index)]))
        return sg::Verdict::Run;
    const II::Item& taken = recs[static_cast<size_t>(index)];

    uint32_t eid = EL::kInvalidId;
    const Target target = SharedTargetOfInventory(inv, eid);
    if (target == Target::Own) return sg::Verdict::Run;
    switch (ConsumeMergeSource(inv, csw::RecordHash(taken))) {
    case MergeSource::Asked:    return sg::Verdict::Run;
    case MergeSource::NotAsked: return sg::Verdict::Cancel;
    case MergeSource::None:     break;
    }
    Take t;
    t.copy.Set(cv::CopyOf(taken, inv));
    if (target == Target::NotReady) {
        GiveBack(t);
        ShowNotReady();
        return sg::Verdict::Cancel;
    }
    caw::Request r;
    r.family = static_cast<uint8_t>(ContainerActFamily::Take);
    r.eid = eid;
    r.index = static_cast<uint16_t>(index);
    r.hash = csw::RecordHash(taken);
    if (!AskTake(r, std::move(t))) {   // the ask showed its own line
        Take back;
        back.copy.Set(cv::CopyOf(taken, inv));
        GiveBack(back);
        return sg::Verdict::Cancel;
    }
    return sg::Verdict::Run;
}

// The host's answer to a take that has already run here. A grant ends the host's escrow with the commit; a refusal
// gives the entry back.
void TakeReply(const Take& t, internal::ReplyAction action, const coop::net::ContainerActReplyPayload& p) {
    if (action == internal::ReplyAction::Granted) {
        // A merge is one write on the host and holds nothing, so there is no escrow to end.
        if (t.family != static_cast<uint8_t>(ContainerActFamily::MergeStack)) SendCommit(t.id, t.eid, 1);
        return;
    }
    GiveBack(t);
    ct::ForgetApplied(t.eid);
    if (action == internal::ReplyAction::Refused) ShowRefusal(p.reason);
    else ShowBadReason();
}

enum class WatchState : uint8_t { Pending, Registered, Refused, Settled };
struct Watch {
    const ue_wrap::facts::Function& row;
    int tag;
    sg::PreFn pre;
    sg::PostFn post;
    WatchState state;
};
std::vector<Watch> g_watches;   // the take's, then the put seat's: filled by the first Install
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
        g_watches.push_back(Watch{kRowTake, kTagTake, &PreTake, nullptr, WatchState::Pending});
        for (const WatchRow* w : {&PutWatch(), &DragWatch()})
            g_watches.push_back(Watch{w->row, w->tag, w->pre, w->post, WatchState::Pending});
        size_t n = 0;
        const WatchRow* stack = StackWatches(n);
        for (size_t i = 0; i < n; ++i)
            g_watches.push_back(Watch{stack[i].row, stack[i].tag, stack[i].pre, stack[i].post, WatchState::Pending});
    }
    // The gate refuses a watch it cannot keep for the process; a refused one is said dead at the settle.
    for (Watch& w : g_watches) {
        if (w.state != WatchState::Pending) continue;
        w.state = sg::WatchName(w.row, w.tag, w.pre, w.post) ? WatchState::Registered : WatchState::Refused;
    }
}

void Tick() {
    SettleWatches();
    g_mergeNote.open = false;   // a merge's removal follows it in the same call, never a later tick
    if (g_puts.empty() && g_takes.empty() && g_commits.empty() && g_expired.empty()) return;
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
    // A take whose wait ran out is given back, and kept as expired so that a grant still to come is answered with an abort
    // (a merge holds nothing on the host, so there is nothing to end).
    for (auto it = g_takes.begin(); it != g_takes.end();) {
        const uint64_t waited = now - it->sentMs;
        if (waited < container_act_state::kReplyWaitMs) {
            ++it;
            continue;
        }
        UE_LOGI("container_act: take id %u eid %u expired after %llu ms", static_cast<unsigned>(it->id),
                static_cast<unsigned>(it->eid), static_cast<unsigned long long>(waited));
        GiveBack(*it);
        if (it->family != static_cast<uint8_t>(ContainerActFamily::MergeStack))
            g_expired.push_back(Expired{it->id, it->eid, now});
        ct::ForgetApplied(it->eid);
        ShowNotAsked();
        it = g_takes.erase(it);
    }
    // A put whose wait ran out has changed nothing here: the entry is still where it was.
    for (auto it = g_puts.begin(); it != g_puts.end();) {
        const uint64_t waited = now - it->sentMs;
        if (waited < container_act_state::kReplyWaitMs) {
            ++it;
            continue;
        }
        UE_LOGI("container_act: request id %u eid %u expired after %llu ms", static_cast<unsigned>(it->id),
                static_cast<unsigned>(it->eid), static_cast<unsigned long long>(waited));
        g_expired.push_back(Expired{it->id, it->eid, now});
        ct::ForgetApplied(it->eid);
        ShowNotAsked();
        it = g_puts.erase(it);
    }
}

void OnReply(const coop::net::ContainerActReplyPayload& p) {
    UE_ASSERT_GAME_THREAD("container_act_request::OnReply");
    const auto take = std::find_if(g_takes.begin(), g_takes.end(), [&](const Take& t) { return t.id == p.id; });
    if (take != g_takes.end()) {
        const internal::ReplyAction action = internal::DecideReply(true, take->id, p);
        UE_LOGI("container_act: reply id %u -> %s after %llu ms", static_cast<unsigned>(p.id),
                internal::ReplyActionName(action),
                static_cast<unsigned long long>(coop::net::NowMs() - take->sentMs));
        TakeReply(*take, action, p);
        g_takes.erase(take);
        return;
    }
    const auto put = std::find_if(g_puts.begin(), g_puts.end(), [&](const Pending& o) { return o.id == p.id; });
    const internal::ReplyAction action = internal::DecideReply(put != g_puts.end(), p.id, p);
    const uint64_t after = put != g_puts.end() ? coop::net::NowMs() - put->sentMs : 0;
    UE_LOGI("container_act: reply id %u -> %s after %llu ms", static_cast<unsigned>(p.id),
            internal::ReplyActionName(action), static_cast<unsigned long long>(after));
    if (action == internal::ReplyAction::Stale) {
        NoteStale(p);
        return;
    }
    const Pending open = std::move(*put);
    g_puts.erase(put);
    switch (action) {
    case internal::ReplyAction::Stale:
        return;
    case internal::ReplyAction::BadReason:
        ct::ForgetApplied(open.eid);
        ShowBadReason();
        return;
    case internal::ReplyAction::Refused:
        // Only a Gone reason brings the host's truth; the call is harmless for the others. With the applied
        // hash forgotten, a truth identical to the last publication still applies, so a ghost converges.
        ct::ForgetApplied(open.eid);
        ShowRefusal(p.reason);
        return;
    case internal::ReplyAction::Granted:
        if (open.kind == PutKind::HoldAndRun) ReplayPut(open);
        else ReplayDrag(open);
        return;
    }
}

void OnDisconnect() {
    g_puts.clear();
    g_takes.clear();
    g_mergeNote = MergeNote{};
    g_latch = Latch{};
    ResetPutLines();
    g_nextId = 0;
    g_nextRequestSeq = 1;
    g_nextCommitSeq = 1;
    g_expired.clear();
    g_commits.clear();
    for (size_t i = 0; i < kLineCount; ++i) g_shown[i] = false;
}

}  // namespace coop::props::container_act_request
