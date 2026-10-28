// coop/interactables/portable_pc_lid.cpp -- see coop/interactables/portable_pc_lid.h.

#include "coop/interactables/portable_pc_lid.h"

#include "coop/element/bind_wait.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/portable_pc.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <set>
#include <vector>

namespace coop::portable_pc_lid {
namespace {

namespace PPC = ue_wrap::portable_pc;
namespace sg  = ue_wrap::script_gate;

std::atomic<coop::net::Session*> g_session{nullptr};

UE_FACT_FN(kRowOpen, "portable_pc_lid", L"prop_portablePc_C", L"open", Declared);
constexpr int kTagLid = 0x4C504344;  // 'LPCD'
bool g_watched = false;  // registered, once a process
bool g_settled = false;  // its names resolved at the gate, live or dead for good

constexpr uint64_t kRetryMs       = 1000;   // a held edge's retry, while any waits
constexpr uint64_t kHeldEdgeTtlMs = 30000;  // how long an edge waits for its PC's name
constexpr uint16_t kLaptopState = static_cast<uint16_t>(coop::net::ReliableKind::LaptopState);

// A local edge on a PC no element names yet: a client's placed PC is named only when the host's echo of
// its spawn comes back. Held by the actor and sent, with the lid as it is then, once the element lane
// names it -- or at once when a line for that PC lands before that, since the line is older than the
// held edge; dropped with its PC, or after kHeldEdgeTtlMs. A handful at most, since a placed PC is named
// within a round trip.
constexpr size_t kHeldCap = 16;
struct HeldEdge { ue_wrap::CachedObjRef pc; uint64_t deadline; };
std::vector<HeldEdge> g_held;

uint64_t g_nextRetry = 0;
bool g_saidUnreadable = false;  // a live PC whose lid does not read: said once a session
std::set<uint32_t> g_wrongClassSaid;  // eids a lid line named that are no portable PC here (said once a session)
Counts g_counts{};  // [dev] this session's authored and applied lines

// The lane's own `open` calls in progress (game thread): a receiver's apply runs the verb a player
// does, so a watch exit inside one sends nothing. A scope in our own frame, unwound on every path.
int g_applyDepth = 0;
struct ApplyScope {
    ApplyScope() { ++g_applyDepth; }
    ~ApplyScope() { --g_applyDepth; }
    ApplyScope(const ApplyScope&) = delete;
    ApplyScope& operator=(const ApplyScope&) = delete;
};

// The lid as each watched body found it, paired to its exit by the gate's own chain.
struct InFlight {
    int   depth;
    void* stack;
    void* actor;
    bool  before;
};
std::vector<InFlight> g_inFlight;

// A body at depth d has only its ancestors below it, so an entry at d or deeper belongs to a body that
// ended without its exit (another watcher's Cancel, or a fault the firewall absorbed, skips every
// exit); it goes, and the stack never outgrows the chain.
void DropFrom(int depth) {
    while (!g_inFlight.empty() && g_inFlight.back().depth >= depth) g_inFlight.pop_back();
}

uint64_t NowMs() {
    return static_cast<uint64_t>(::GetTickCount64());
}

// Outside a session the gate runs only for a dev probe that holds it; the lane takes no part there.
coop::net::Session* RunningSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running()) ? s : nullptr;
}

// A client's line goes to the host. The host's goes to every ready client, and a client's line it
// relays goes to its author as well, tagged with it: each peer applies the lines in the host's order
// and ends where the host ends, even when two peers moved the same lid within one round trip. The
// author's own copy already holds its line unless it moved the lid again since; then its lid replays
// the host's order and ends there too.
void SendOut(coop::net::Session* s, const coop::net::LaptopStatePayload& p, uint8_t origin) {
    if (s->role() == coop::net::Role::Client) {
        s->SendReliableToSlot(0, coop::net::ReliableKind::LaptopState, &p, sizeof(p));
        return;
    }
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
        if (!s->IsSlotReady(slot)) continue;
        s->SendReliableToSlot(slot, coop::net::ReliableKind::LaptopState, &p, sizeof(p), origin);
    }
}

coop::net::LaptopStatePayload LidLine(uint32_t eid, bool opened) {
    coop::net::LaptopStatePayload p{};
    p.op = 6;
    p.isOpened = opened ? 1 : 0;
    p.eid = eid;
    return p;
}

bool IsPc(void* actor) {
    return PPC::IsPortablePc(actor);
}

bool ReadLid(void* actor, bool& opened) {
    return IsPc(actor) && PPC::ReadOpened(actor, opened);
}

// Puts a PC's lid where the line says, through its own `open`, inside the lane's scope. True when it
// moved.
bool Apply(void* actor, bool opened) {
    bool cur = false;
    if (!PPC::ReadOpened(actor, cur) || cur == opened) return false;
    ApplyScope scope;
    if (!PPC::CallOpen(actor, opened)) return false;
    ++g_counts.applied;
    return true;
}

// A local edge on a PC no element names yet, held until one does; a second edge on it refreshes the
// entry, since what goes is the lid as it is when the name lands.
void Hold(void* actor) {
    for (HeldEdge& h : g_held) {
        if (h.pc.Is(actor)) { h.deadline = NowMs() + kHeldEdgeTtlMs; return; }
    }
    if (g_held.size() >= kHeldCap) {
        UE_LOGW("portable_pc_lid: a lid edge on a portable PC no element names yet REFUSED -- %zu edges already "
                "wait for their PC's name", kHeldCap);
        return;
    }
    HeldEdge h;
    h.pc.Set(actor);
    h.deadline = NowMs() + kHeldEdgeTtlMs;
    g_held.push_back(h);
    UE_LOGI("portable_pc_lid: a lid edge on a portable PC no element names yet -- held until it is named");
}

// A held edge for this PC, taken off the list: the caller sends it.
bool TakeHeld(void* actor) {
    for (auto it = g_held.begin(); it != g_held.end(); ++it) {
        if (it->pc.Is(actor)) {
            g_held.erase(it);
            return true;
        }
    }
    return false;
}

sg::Verdict OnOpenPre(const sg::Call& call) {
    DropFrom(call.depth);
    bool before = false;
    if (g_applyDepth == 0 && RunningSession() && ReadLid(call.object, before))
        g_inFlight.push_back({call.depth, call.stack, call.object, before});
    return sg::Verdict::Run;
}

void OnOpenPost(const sg::Call& call) {
    DropFrom(call.depth + 1);
    if (g_inFlight.empty() || g_inFlight.back().depth != call.depth || g_inFlight.back().stack != call.stack)
        return;  // its entry read no lid: the lane's own apply, or outside a session
    const InFlight f = g_inFlight.back();
    g_inFlight.pop_back();
    bool after = false;
    if (!PPC::ReadOpened(f.actor, after) || after == f.before) return;
    coop::net::Session* s = RunningSession();
    if (!s || !s->connected()) return;
    const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(f.actor);
    if (eid == coop::element::kInvalidId) {
        Hold(f.actor);
        return;
    }
    // This edge is newer than any line held for the PC.
    coop::element::bind_wait::Drop(static_cast<uint32_t>(eid), kLaptopState, "a local lid edge is newer");
    SendOut(s, LidLine(static_cast<uint32_t>(eid), after), 0);
    ++g_counts.sent;
    UE_LOGI("portable_pc_lid: local lid edge (eid=%u opened=%u) -- sent",
            static_cast<uint32_t>(eid), static_cast<unsigned>(after));
}

// The held edges whose PC has been named. An edge's send voids the line held for that PC: it was authored
// by a peer that had not seen this peer's edge, and the host, which gets the edge after it, ends at the
// edge too.
void Retry(coop::net::Session* s, uint64_t now) {
    for (auto it = g_held.begin(); it != g_held.end();) {
        void* pc = it->pc.Get();
        const coop::element::ElementId eid =
            pc ? coop::element::Registry::Get().EidForActor(pc) : coop::element::kInvalidId;
        bool opened = false;
        if (!pc) {
            it = g_held.erase(it);  // the PC went before it was named: nothing of it is anywhere else
        } else if (eid != coop::element::kInvalidId && ReadLid(pc, opened)) {
            coop::element::bind_wait::Drop(static_cast<uint32_t>(eid), kLaptopState, "a local lid edge is newer");
            if (s->connected()) {
                SendOut(s, LidLine(static_cast<uint32_t>(eid), opened), 0);
                ++g_counts.sent;
                UE_LOGI("portable_pc_lid: a held lid edge sent once its PC was named (eid=%u opened=%u)",
                        static_cast<uint32_t>(eid), static_cast<unsigned>(opened));
            }
            it = g_held.erase(it);
        } else if (now > it->deadline) {
            UE_LOGW("portable_pc_lid: a held lid edge EXPIRED unsent (its PC was not named within %llu ms)",
                    static_cast<unsigned long long>(kHeldEdgeTtlMs));
            it = g_held.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watched) return;
    g_watched = sg::WatchName(kRowOpen, kTagLid, &OnOpenPre, &OnOpenPost);
    if (!g_watched) UE_LOGW("portable_pc_lid: the lid's watch was refused -- a lid change is not sent");
}

void Tick() {
    coop::net::Session* s = RunningSession();
    if (!s) return;
    if (!g_settled) {
        // Asked until the gate has settled every name watch: a watch refused at registration or dead in a
        // full table never goes live, and asking after that would walk the table every tick.
        sg::ResolvePendingNames();
        if (g_watched && sg::NameWatchLive(kRowOpen, kTagLid)) {
            g_settled = true;
            UE_LOGI("portable_pc_lid: the lid's writer is watched at the script-body gate");
        } else if (!g_watched || sg::PendingNameCount() == 0) {
            g_settled = true;
            UE_LOGW("portable_pc_lid: the lid's writer is NOT watched -- a lid change is not sent");
        }
    }
    if (g_held.empty()) return;
    const uint64_t now = NowMs();
    if (now < g_nextRetry) return;
    g_nextRetry = now + kRetryMs;
    Retry(s, now);
}

namespace {
// A line held in bind_wait runs again once its PC is bound: the whole of OnLid.
void ReplayLid(const uint8_t* bytes, size_t n, int senderSlot) {
    coop::net::LaptopStatePayload p{};
    if (n != sizeof p) return;
    std::memcpy(&p, bytes, sizeof p);
    OnLid(p, static_cast<uint8_t>(senderSlot));
}
}  // namespace

void OnLid(const coop::net::LaptopStatePayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    const bool opened = p.isOpened != 0;
    void* actor = coop::element::LivePropActor(p.eid);
    if (actor && !IsPc(actor)) {
        if (g_wrongClassSaid.insert(p.eid).second)
            UE_LOGW("portable_pc_lid: a lid line for eid=%u (from slot %u) names a prop that is not a portable PC -- "
                    "dropped", p.eid, static_cast<unsigned>(senderSlot));
        return;
    }
    if (!actor) {
        // Held, not relayed: the replay runs OnLid again at the PC's bind and relays the line once, when it
        // applies. Not deferred: no bind is coming for this eid, so there is nothing to wait for.
        if (!coop::element::bind_wait::Hold(p.eid, kLaptopState, senderSlot, &ReplayLid, &p, sizeof p,
                                            /*coalesce=*/true))
            coop::element::bind_wait::NoteNeverBound(kLaptopState, p.eid);
        return;
    }
    bool cur = false;
    if (!ReadLid(actor, cur)) {
        if (!g_saidUnreadable) {
            g_saidUnreadable = true;
            UE_LOGW("portable_pc_lid: a lid line for eid=%u (from slot %u) names a portable PC whose lid does not "
                    "read here -- dropped", p.eid, static_cast<unsigned>(senderSlot));
        }
        return;
    }
    if (TakeHeld(actor)) {
        // This peer's edge on the PC waited for its name and has not gone: whoever sent this line had not
        // seen it, and the host, which gets the edge after the line, ends at the edge. So the edge goes now,
        // with the lid as it is, and the line is not applied.
        if (s->connected()) {
            SendOut(s, LidLine(p.eid, cur), 0);
            ++g_counts.sent;
            UE_LOGI("portable_pc_lid: a held lid edge sent over an older line (eid=%u opened=%u)", p.eid,
                    static_cast<unsigned>(cur));
        }
        return;  // a host sends on the edge, not the line it supersedes
    }
    if (Apply(actor, opened))
        UE_LOGI("portable_pc_lid: wire lid applied (eid=%u opened=%u, from slot %u)", p.eid,
                static_cast<unsigned>(opened), static_cast<unsigned>(senderSlot));
    if (s->role() == coop::net::Role::Host) SendOut(s, p, senderSlot);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    // Only an open lid is a row: `opened` is runtime-only, so every copy the joiner has loaded closed. The
    // PCs are the object index's, not every prop element; the placed ones are those an element names.
    struct Rows { coop::net::Session* s; int slot; int sent; } rows{s, peerSlot, 0};
    PPC::ForEachPc([](void* ctx, void* pc) {
        auto& r = *static_cast<Rows*>(ctx);
        const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(pc);
        bool opened = false;
        if (eid == coop::element::kInvalidId || !PPC::ReadOpened(pc, opened) || !opened) return;
        const coop::net::LaptopStatePayload lp = LidLine(static_cast<uint32_t>(eid), true);
        r.s->SendReliableToSlot(r.slot, coop::net::ReliableKind::LaptopState, &lp, sizeof(lp));
        ++r.sent;
    }, &rows);
    if (rows.sent) UE_LOGI("portable_pc_lid: %d open lid(s) -> slot %d", rows.sent, peerSlot);
}

Counts LineCounts() {
    return g_counts;
}

void OnDisconnect() {
    g_counts = {};
    g_saidUnreadable = false;
    g_wrongClassSaid.clear();
    g_held.clear();
    g_nextRetry = 0;
    g_inFlight.clear();
}

}  // namespace coop::portable_pc_lid
