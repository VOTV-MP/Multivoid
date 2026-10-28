// coop/props/container_act_request.cpp -- see coop/props/container_act_request.h.

#include "coop/props/container_act_request.h"

#include "coop/comms/chat_feed.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/props/container_act_state.h"

#include "l10n/l10n.h"

#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <algorithm>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace coop::props::container_act_request {
namespace {

namespace caw = coop::props::container_act_wire;
using coop::net::ContainerActReason;
using coop::net::ReliableKind;

// A reply that has not come in this long closes the record: an honest wait ends in milliseconds, so the bound covers
// only a transport that refused the reply.
constexpr uint64_t kReplyWaitMs = 10000;

// A refused commit is sent again at most this often.
constexpr uint64_t kCommitRetryMs = 250;

// The one open act.
struct Pending {
    bool open = false;
    uint16_t id = 0;
    uint32_t eid = 0;
    uint8_t family = 0;
    uint64_t sentMs = 0;
};

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
                     kLineNotAsked, kLineCount };
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
    case ContainerActReason::WrongFamily:
    case ContainerActReason::Granted:
        Show(kLineCantNow, l10n::T("That can't be done right now."));
        return;
    }
}

void ShowBadReason() { Show(kLineCantNow, l10n::T("That can't be done right now.")); }
void ShowNotAsked() { Show(kLineNotAsked, l10n::T("The host could not be asked; try again.")); }

// ---- the record and its commits -------------------------------------------------------------------------------

void Close() { g_rec = Pending{}; }

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

void QueueCommit(const caw::Commit& c) {
    QueuedCommit q;
    q.blob = caw::PackCommit(c);
    q.id = c.id;
    q.queuedMs = coop::net::NowMs();
    g_commits.push_back(std::move(q));
    DrainCommits();
}

void SendAbort(uint16_t id, uint32_t eid) {
    caw::Commit c;
    c.id = id;
    c.committed = 0;
    c.eid = eid;
    QueueCommit(c);
    UE_LOGI("container_act: commit id %u eid %u -> aborted", static_cast<unsigned>(id), static_cast<unsigned>(eid));
}

// The reply matched no open record. One for an expired request ends the host's hold: a grant is answered with an
// abort, a refusal reserved nothing.
void NoteStale(const coop::net::ContainerActReplyPayload& p) {
    const auto it = std::find_if(g_expired.begin(), g_expired.end(), [&](const Expired& e) { return e.id == p.id; });
    if (it == g_expired.end()) return;
    if (p.granted == 1 && p.reason == 0) SendAbort(it->id, it->eid);
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
    return true;
}

void Install(coop::net::Session* session) { g_session = session; }

void Tick() {
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
    if (!g_rec.open) return;
    const uint64_t waited = now - g_rec.sentMs;
    if (waited < kReplyWaitMs) return;
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
        ShowBadReason();
        Close();
        return;
    case internal::ReplyAction::Refused:
        ShowRefusal(p.reason);
        Close();
        return;
    case internal::ReplyAction::Granted:
        // No family's replay is built here, so every grant is answered with an abort: the host's hold is ended, not
        // kept. A family that brings its replay switches on g_rec.family above this call.
        SendAbort(g_rec.id, g_rec.eid);
        Close();
        return;
    }
}

void OnDisconnect() {
    Close();
    g_nextId = 0;
    g_nextRequestSeq = 1;
    g_nextCommitSeq = 1;
    g_expired.clear();
    g_commits.clear();
    for (size_t i = 0; i < kLineCount; ++i) g_shown[i] = false;
}

}  // namespace coop::props::container_act_request
