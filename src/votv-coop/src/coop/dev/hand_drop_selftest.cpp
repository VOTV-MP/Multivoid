// coop/dev/hand_drop_selftest.cpp -- see coop/dev/hand_drop_selftest.h.

#include "coop/dev/hand_drop_selftest.h"

#include "coop/comms/chat_sync.h"
#include "coop/config/config.h"
#include "coop/dev/drill_chat_pace.h"
#include "coop/dev/prop_act_drill.h"      // the act drill's handshake reads: the other peer's line, OwnJoinOver
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"    // the holder's puppet, for the watcher's census centre
#include "coop/props/prop_act_request.h"  // dev::PendingId: a client's pickup of a shared prop is asked of the host
#include "coop/props/prop_key_index.h"    // Find: the world prop a key names
#include "coop/text/utf8_codec.h"

#include "hand_drop_census.h"  // co-located private header (src tree, not include/)

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

namespace coop::dev::hand_drop_selftest {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace P  = ue_wrap::profile;

using coop::dev::hand_drop_census::PropRow;
using coop::dev::hand_drop_census::Census;
using coop::dev::hand_drop_census::DescribeDiff;
using coop::dev::hand_drop_census::g_unreadable;

std::atomic<coop::net::Session*> g_session{nullptr};

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::hand_drop_selftest);
    return s;
}

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The region both peers census. Wide enough to still hold a prop the drop THREW -- the game's drop
// ends in a camera-aimed impulse, and at 6 m the released prop left the region on the peer that
// dropped it, which reads as a loss on both logs and is really the instrument's edge. Narrow
// enough that the two peers, centred on the holder and on the holder's puppet, agree on the set.
constexpr float kCensusRadiusCm = 2500.f;
// The pickup target is taken from much closer, so the prop that moves is the one a player at that
// spot would have reached for.
constexpr float kTargetRadiusCm = 400.f;
// How many refusals to walk past before calling the episode unfirable. Bounded, because each
// attempt dispatches a verb with side effects of its own on a frozen prop.
constexpr int kMaxPickupAttempts = 10;

// ---- the chain --------------------------------------------------------------------------------

enum class Act { ClearHand, CensusBefore, Pickup, CensusHeld, Drop, CensusAfter };

// Four episodes, alternating which peer holds, each direction twice: one pass of an ordering is an
// anecdote, and the loss this measures was first read out of a single field log. Both peers walk the
// same chain, and a step advances on this peer's own state AND a line the other peer said in the lobby
// chat (the act drill's handshake, prop_act_drill.h), so no step waits on a clock. Every wait has a
// failure bound that FAILs the chain; it never passes one.
constexpr int      kEpisodes     = 4;
constexpr uint64_t kCheckEveryMs = 250;
constexpr uint64_t kBoundMs      = 60000;
// A prop that moved less than this between two reads is at rest, kRestChecks reads in a row, as
// remover_drill and sack_drill read theirs; the reads are one actor's, never a census.
constexpr float    kRestMoveCm   = 1.f;
constexpr int      kRestChecks   = 3;

bool HolderIsHost(int episode) { return (episode % 2) == 1; }   // the host holds in the odd episodes
const char* RoleWord(bool host) { return host ? "host" : "client"; }

enum class Stage : uint8_t {
    Clear, ClearRest, ClearPeer,         // episode 0: each peer empties its own hand
    Before, BeforePeer,                  // both take a census; the holder waits for the watcher's
    Pickup, PickupWait, HeldPeer,        // the holder: take, then wait for the watcher's held line
    DropRest,                            // the holder: the dropped prop at rest
    Picked, DroppedWait,                 // the watcher: the key leaves, then comes back at rest
    AfterPeer,                           // both: the other peer's after line
    Done, Failed
};

uint64_t g_armedAtMs = 0, g_nextCheckMs = 0, g_nextSayMs = 0;
uint64_t g_stageMs = 0;   // when the stage began; the failure bound counts from it
Stage    g_stage   = Stage::Clear;
int      g_episode = 0;
std::deque<std::string> g_out;   // lines this peer owes the lobby chat

// Reads of one actor's place, a check apart: at rest once it moved under kRestMoveCm for kRestChecks reads in a row.
struct RestWatch {
    ue_wrap::CachedObjRef actor;
    ue_wrap::FVector      last{};
    int                   reads = 0;

    void Reset() { actor.Reset(); reads = 0; }

    bool Step(void* a) {
        ue_wrap::FVector at{};
        if (!a || !E::TryGetActorLocation(a, at)) { reads = 0; return false; }
        if (!actor.Is(a)) { actor.Set(a); last = at; reads = 0; return false; }
        const float dx = at.X - last.X, dy = at.Y - last.Y, dz = at.Z - last.Z;
        reads = (std::sqrt(dx * dx + dy * dy + dz * dz) < kRestMoveCm) ? reads + 1 : 0;
        last  = at;
        return reads >= kRestChecks;
    }
};
RestWatch             g_rest;
ue_wrap::CachedObjRef g_dropped;   // the ex-hand actor a drop released

struct EpisodeResult {
    bool         pickupFired = false;
    bool         pickupHeld  = false;   // holding_actor really became the target
    bool         dropFired   = false;
    bool         dropCleared = false;
    std::wstring targetKey;
    int          before = -1, held = -1, after = -1;   // this peer's own census counts
    // The keys the region held before the pickup. A count says a region changed; the diff against
    // this says WHICH prop did, which is the only form in which a 45-row census is readable at all
    // -- and a prop that leaves under one key and comes back under another reads as a count that
    // balances while the identity did not survive.
    std::vector<std::wstring> beforeKeys;
    // The actors each before key named -- a duplicate key names several -- so an unread row later
    // counts as that prop only when it is one of them, never another actor under the same key.
    std::vector<std::pair<std::wstring, ue_wrap::CachedObjRef>> beforeActors;

    bool BeforeActorIs(const std::wstring& key, void* actor) const {
        for (const auto& b : beforeActors)
            if (b.first == key && b.second.Is(actor)) return true;
        return false;
    }
};
EpisodeResult g_ep[5];   // 1-based

// ---- the verbs --------------------------------------------------------------------------------

void* MainPlayer() {
    void* p = coop::players::Registry::Get().Local();
    return (p && R::IsLive(p)) ? p : nullptr;
}

void* HoldingActor(void* player) {
    E::MainPlayerGrabState gs{};
    if (!player || !E::ReadMainPlayerGrabState(player, gs)) return nullptr;
    return gs.holdingActor;
}

// The pickup in progress on the holder, one candidate per act. The game's own pickup is E::CallMainPlayerHoldObject
// with the target handed in directly: `Hold Object` reads `manual` when it is valid and only falls back to whatever
// the player is looking at when it is not, so a driver needs no aim and no hit result. It ends in addEquip, which
// writes the hold slot and calls updateHold, so a collected prop IS a hand item -- and it destroys the world actor on
// the way, which is why a census taken while it is held reads one row shorter on BOTH peers.
//
// `collected` is the verb's own answer and the only honest one for a call that ran: the call returning says the frame
// dispatched, while every refusal inside -- not simulating physics, too heavy, cannot be held -- leaves collected
// false and the hand empty. A client's pickup of a shared prop does not run at the call: it is cancelled and asked
// of the host, and the hand says what the act came to once no request is open.
struct PickCandidate {
    ue_wrap::CachedObjRef actor;
    std::wstring          key;
    std::wstring          cls;
    float                 distCm = 0.f;
};
struct PickupRun {
    bool                       active = false;
    int                        episode = 0;
    std::vector<PickCandidate> list;      // nearest first, at most kMaxPickupAttempts
    size_t                     next = 0;  // the candidate the next act tries
    bool                       asked = false;   // the act of list[next - 1] is open at the host
};
PickupRun g_pick;

// ---- where each role censuses ------------------------------------------------------------------
// The game's own release. Three were measured before this one was settled on, and what each does
// is worth keeping, because the choice decides WHERE the released prop is when the peers exchange
// it:
//   simulateDrop(place=false) parks the released actor in `droppedItem` and puts nothing in the
//     world -- measured, no prop within 6 m on either peer, and three keyed destroys on the wire;
//   simulateDrop(place=true) enters the placement MODE rather than releasing, so the hand
//     stayed full on two of four episodes and the watcher read a still-held prop as lost;
//   throwHoldingProp drops, teleports the released actor to the camera and throws it -- the one
//     that reliably empties the hand and leaves a prop in the world.
// The throw is why the census radius is metres: the released prop travels before it settles.
bool CallDropHolding(void* player) {
    void* cls = R::FindClass(P::name::MainPlayerClass);
    void* fn  = cls ? R::FindFunction(cls, L"throwHoldingProp") : nullptr;
    if (!fn) {
        UE_LOGW("hand_drop_selftest: 'throwHoldingProp' unresolved -- cannot drive a drop");
        return false;
    }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    return ue_wrap::Call(player, f);
}

// The holder censuses around itself; the watcher censuses around the holder's puppet, so both
// peers measure the same region of the same world. Returns false when the watcher has no puppet
// yet or a location cannot be read, which is a reason printed rather than a census of the wrong place.
bool CensusCentre(bool iAmHolder, bool holderIsHost, ue_wrap::FVector& out) {
    void* actor = nullptr;
    if (iAmHolder) {
        actor = MainPlayer();
    } else {
        const uint8_t holderSlot = holderIsHost ? 0u : 1u;
        coop::RemotePlayer* pup = coop::players::Registry::Get().Puppet(holderSlot);
        if (!pup || !pup->valid()) return false;
        actor = pup->GetActor();
    }
    if (!actor) return false;
    // Like every census read: an actor whose read failed is not read again.
    for (const ue_wrap::CachedObjRef& u : g_unreadable)
        if (u.Is(actor)) return false;
    if (E::TryGetActorLocation(actor, out)) return true;
    g_unreadable.emplace_back();
    g_unreadable.back().Set(actor);
    return false;
}

const char* ActName(Act a) {
    switch (a) {
        case Act::ClearHand:    return "CLEARHAND";
        case Act::CensusBefore: return "BEFORE";
        case Act::Pickup:       return "PICKUP";
        case Act::CensusHeld:   return "HELD";
        case Act::Drop:         return "DROP";
        case Act::CensusAfter:  return "AFTER";
    }
    return "?";
}

// One census, logged, plus the watcher's reads of it (the inferred subject at the held census, the verdict at the
// after census). False, with nothing logged, when this peer has no census centre yet. `rowsOut` takes the rows, for
// the pickup's candidate list.
bool RunCensus(Act act, int episode, bool holderIsHost, bool iAmHolder, uint64_t since,
               std::vector<PropRow>* rowsOut = nullptr) {
    ue_wrap::FVector centre{};
    if (!CensusCentre(iAmHolder, holderIsHost, centre)) return false;
    std::vector<PropRow> unread;
    const std::vector<PropRow> rows = Census(centre, kCensusRadiusCm, unread);
    if (!unread.empty())
        UE_LOGW("hand_drop_selftest: ep%d %s -- %zu keyed prop(s) could not be read (first key='%ls'); this "
                "census cannot say where they are", episode, ActName(act), unread.size(),
                unread[0].key.c_str());
    EpisodeResult& ep = g_ep[episode];
    const int count = static_cast<int>(rows.size());

    switch (act) {
        case Act::CensusBefore:
            ep.before = count;
            ep.beforeKeys.clear();
            ep.beforeActors.clear();
            for (const PropRow& r : rows) {
                ep.beforeKeys.push_back(r.key);
                ep.beforeActors.emplace_back(r.key, ue_wrap::CachedObjRef{});
                ep.beforeActors.back().second.Set(r.actor);
            }
            break;
        case Act::CensusHeld:   ep.held  = count; break;   // the watcher's inference follows below
        case Act::CensusAfter:  ep.after = count; break;
        default: break;
    }

    UE_LOGI("hand_drop_selftest: ep%d %s (+%llus) role=%s holder=%s -- %d keyed prop(s) within "
            "%.0fcm; vs before: %ls", episode, ActName(act),
            static_cast<unsigned long long>(since / 1000), iAmHolder ? "HOLDER" : "WATCHER",
            holderIsHost ? "HOST" : "CLIENT", count, kCensusRadiusCm,
            DescribeDiff(ep.beforeKeys, rows).c_str());

    if (!iAmHolder && act == Act::CensusHeld && ep.targetKey.empty()) {
        // The watcher is told nothing: it works out which prop the holder took by watching one
        // leave its own world. A pickup destroys the world actor on every peer, so exactly one key
        // should be missing here; none means the pickup never crossed, and more than one means
        // something else moved at the same time and the episode cannot name a subject. A row the
        // census could not read is still that prop in this world, and so did not leave, when it is
        // the very actor the before census saw under the key.
        std::vector<std::wstring> gone;
        for (const std::wstring& k : ep.beforeKeys) {
            bool present = false;
            for (const PropRow& r : rows) if (r.key == k) { present = true; break; }
            for (const PropRow& u : unread)
                if (!present && u.key == k && ep.BeforeActorIs(k, u.actor)) { present = true; break; }
            if (!present) gone.push_back(k);
        }
        if (gone.size() == 1) {
            ep.targetKey = gone[0];
            UE_LOGI("hand_drop_selftest: ep%d WATCHER subject inferred -- key='%ls' left this world "
                    "when the holder picked it up", episode, ep.targetKey.c_str());
        } else {
            UE_LOGW("hand_drop_selftest: ep%d WATCHER cannot name a subject -- %zu key(s) left this "
                    "world at the held census (expected exactly one); the pickup either did not "
                    "cross or something else moved with it", episode, gone.size());
        }
    }

    if (!iAmHolder) {
        // The watcher's verdict, said the moment it can be said: a prop that went into a hand and
        // came back out is a row restored here. A census that stays short after the drop is the
        // loss -- on this peer the dropped prop exists nowhere, and whatever its wire identity is
        // bound to, it is not an actor in this world.
        if (act == Act::CensusAfter && ep.before >= 0) {
            // The prop the holder moved is named by the key it carried before the pickup, so the
            // verdict is about that key and not about a count that two unrelated changes can
            // balance. A key that is gone from this peer after the drop is the loss: the holder
            // has the prop and this peer has nothing bound to it.
            bool stillHere = false, targetUnread = false;
            for (const PropRow& r : rows) if (r.key == ep.targetKey) { stillHere = true; break; }
            for (const PropRow& u : unread) if (u.key == ep.targetKey) { targetUnread = true; break; }
            if (targetUnread)
                UE_LOGW("hand_drop_selftest: ep%d WATCHER cannot judge -- key='%ls' could not be read after the "
                        "drop", episode, ep.targetKey.c_str());
            else if (ep.targetKey.empty())
                UE_LOGI("hand_drop_selftest: ep%d WATCHER -- the holder named no key this episode; "
                        "%d before, %d after", episode, ep.before, ep.after);
            else if (!stillHere)
                // Two readings, and this peer cannot tell them apart from here: the release never
                // happened, or it happened and did not arrive. The holder's own log settles it --
                // its DROP line carries cleared=1 only when the hand actually emptied -- so the
                // verdict names both rather than asserting the interesting one.
                UE_LOGW("hand_drop_selftest: ep%d WATCHER DOES NOT HAVE THE PROP -- key='%ls' is "
                        "not here after the drop (%d before, %d after). Read the holder's DROP "
                        "line: cleared=1 means it was released and this peer lost it; cleared=0 "
                        "means it is still in that hand and there is nothing to have",
                        episode, ep.targetKey.c_str(), ep.before, ep.after);
            else
                UE_LOGI("hand_drop_selftest: ep%d WATCHER OK -- key='%ls' is here after the drop "
                        "(%d before, %d after)", episode, ep.targetKey.c_str(), ep.before,
                        ep.after);
        }
    }
    if (rowsOut) *rowsOut = rows;
    return true;
}

// The pickup's candidates: the keyed props near the holder, nearest first, handed to PickupTick one act each. Empty
// when the run is armed, else why the episode cannot fire.
std::string StartPickup(const std::vector<PropRow>& rows, int episode) {
    void* player = MainPlayer();
    if (!player) return "the local player is not possessed";
    if (HoldingActor(player)) return "the hand is already full";
    // Nearest first, and KEEP GOING while the game refuses: most of what stands around a base
    // is scenery the pickup declines (not simulating physics, too heavy, canHold off), and a
    // driver that gave up on the nearest one would report "the hand lane does nothing" when
    // what it found was a crate. Each attempt says which class and what the verb answered.
    std::vector<const PropRow*> byDistance;
    for (const PropRow& r : rows)
        if (r.distCm <= kTargetRadiusCm) byDistance.push_back(&r);
    std::sort(byDistance.begin(), byDistance.end(),
              [](const PropRow* a, const PropRow* b) { return a->distCm < b->distCm; });
    if (byDistance.empty()) {
        char why[96];
        std::snprintf(why, sizeof(why), "no keyed prop within %.0fcm of the holder", kTargetRadiusCm);
        return why;
    }
    g_pick = PickupRun{};
    g_pick.active  = true;
    g_pick.episode = episode;
    for (const PropRow* r : byDistance) {
        if (g_pick.list.size() >= static_cast<size_t>(kMaxPickupAttempts)) break;
        g_pick.list.emplace_back();
        PickCandidate& c = g_pick.list.back();
        c.actor.Set(r->actor);
        c.key    = r->key;
        c.cls    = r->cls;
        c.distCm = r->distCm;
    }
    return {};
}

// The act of the candidate just tried has ended (it ran at the call, or the host's answer is in): the hand says
// whether it was taken. A refusal leaves the run for the next candidate.
void PickupSettle(void* player, bool called, bool collected) {
    EpisodeResult& ep = g_ep[g_pick.episode];
    const size_t tried = g_pick.next;
    const PickCandidate& c = g_pick.list[tried - 1];
    void* nowHolding = HoldingActor(player);
    UE_LOGI("hand_drop_selftest: ep%d PICKUP try %zu key='%ls' cls='%ls' d=%.0fcm -- call=%d "
            "collected=%d holding=%p", g_pick.episode, tried, c.key.c_str(), c.cls.c_str(),
            c.distCm, called ? 1 : 0, collected ? 1 : 0, nowHolding);
    if (!nowHolding) return;
    ep.targetKey   = c.key;
    ep.pickupFired = called;
    ep.pickupHeld  = true;
    UE_LOGI("hand_drop_selftest: ep%d PICKUP OK after %zu attempt(s) -- key='%ls' is in "
            "the hand as actor %p (the world copy is destroyed; a census here reads one "
            "row shorter until the drop)", g_pick.episode, tried, c.key.c_str(), nowHolding);
    g_pick.active = false;
}

// One act of the pickup in progress per tick: while a request is open nothing else is called; when it ends, or when
// the call ran at once, the hand is read and a refusal moves to the next candidate. The list is the attempt bound.
void PickupTick() {
    if (!g_pick.active) return;
    void* player = MainPlayer();
    if (!player) return;
    if (g_pick.asked) {
        if (coop::prop_act_request::dev::PendingId() != 0) return;
        g_pick.asked = false;
        PickupSettle(player, true, false);
        return;
    }
    if (g_pick.next >= g_pick.list.size()) {
        UE_LOGW("hand_drop_selftest: ep%d PICKUP -- %zu candidate(s) tried, the game refused every "
                "one; episode cannot fire", g_pick.episode, g_pick.next);
        g_pick.active = false;
        return;
    }
    void* target = g_pick.list[g_pick.next++].actor.Get();
    if (!target) return;   // died since the census: the try is spent
    bool collected = false;
    const bool called = E::CallMainPlayerHoldObject(player, target, collected);
    if (called && !collected && coop::prop_act_request::dev::PendingId() != 0) {
        g_pick.asked = true;
        return;
    }
    PickupSettle(player, called, collected);
}

// ---- the handshake ------------------------------------------------------------------------------
// The lines the two peers say to each other in the lobby chat. A client says its lines one per drill_chat_pace.h's
// spacing, since the host refuses a faster one for good; the host's go out at once.

namespace PAD = coop::dev::prop_act_drill::detail;

void Say(const std::string& line) { g_out.push_back(line); }

void PumpSay(bool isHost, uint64_t now) {
    if (g_out.empty() || now < g_nextSayMs) return;
    coop::chat_sync::QueueSend(g_out.front());
    g_out.pop_front();
    g_nextSayMs = isHost ? now : now + coop::dev::kTalkEveryMs;
}

// The text after `head` in the first line the OTHER peer said: the host reads its lobby record for the client's slot,
// a client its chat feed (history included -- the lines a joiner is seeded with are this session's).
bool OtherSaid(bool isHost, const std::string& head, std::string* rest = nullptr) {
    return isHost ? PAD::ClientSaid(PAD::kClientSlot, head, rest) : PAD::HostSaid(head, rest);
}

std::string EpHead(int episode) { return std::string("hand-drop ep") + std::to_string(episode) + " "; }

// ---- the failure line ---------------------------------------------------------------------------

void Fail(int episode, const char* step, const std::string& why, bool bound) {
    UE_LOGW("hand_drop_selftest: FAIL ep%d %s -- %s%s", episode, step, why.c_str(), bound ? " within 60 s" : "");
    g_stage = Stage::Failed;
}

// A new stage starts its bound and its rest reads over.
void Enter(Stage st, uint64_t now) {
    g_stage   = st;
    g_stageMs = now;
    g_rest.Reset();
}

// One check of the chain on this peer: at most one stage change.
void Advance(bool isHost, uint64_t now) {
    if (g_stage == Stage::Done || g_stage == Stage::Failed) return;
    const uint64_t since = now - g_armedAtMs;
    const int n = g_episode;
    const bool holderIsHost = HolderIsHost(n);
    const bool iAmHolder = (isHost == holderIsHost);
    const std::string me   = RoleWord(isHost);
    const std::string peer = RoleWord(!isHost);
    EpisodeResult& ep = g_ep[n];
    // Waiting on `what` past the bound is the FAIL; before it, nothing.
    const auto waitOn = [&](const char* step, const std::string& what) {
        if (now - g_stageMs > kBoundMs) Fail(n, step, what + " not seen", true);
    };
    const std::string head = EpHead(n);

    switch (g_stage) {
    case Stage::Clear: {
        // Both peers start possessed and holding whatever the save gave them, so the first episode
        // would open with a full hand and no pickup. Each peer empties its own hand first; the prop
        // that lands is inside every census that follows, so it changes no diff.
        void* player = MainPlayer();
        if (!player) { waitOn("clear", "the local player"); return; }
        void* holding = HoldingActor(player);
        if (!holding) {
            UE_LOGI("hand_drop_selftest: CLEARHAND (+%llus) -- the hand is already empty",
                    static_cast<unsigned long long>(since / 1000));
            Say("hand-drop clear " + me + " done");
            Enter(Stage::ClearPeer, now);
            return;
        }
        const bool called = CallDropHolding(player);
        UE_LOGI("hand_drop_selftest: CLEARHAND (+%llus) -- dropped the starting hand item %p "
                "(call=%d, now holding=%p)", static_cast<unsigned long long>(since / 1000),
                holding, called ? 1 : 0, HoldingActor(player));
        g_dropped.Set(holding);
        Enter(Stage::ClearRest, now);
        return;
    }
    case Stage::ClearRest: {
        // A prop that is gone has nothing left to settle.
        void* a = g_dropped.Get();
        if (a && !g_rest.Step(a)) { waitOn("clear", "the dropped prop at rest"); return; }
        Say("hand-drop clear " + me + " done");
        Enter(Stage::ClearPeer, now);
        return;
    }
    case Stage::ClearPeer:
        if (!OtherSaid(isHost, "hand-drop clear " + peer + " done")) {
            waitOn("clear", "the line 'clear " + peer + " done'");
            return;
        }
        g_episode = 1;
        Enter(Stage::Before, now);
        return;
    case Stage::Before:
        if (!RunCensus(Act::CensusBefore, n, holderIsHost, iAmHolder, since)) {
            waitOn("before", iAmHolder ? "this peer's census centre (the local player)"
                                       : "this peer's census centre (the holder's puppet)");
            return;
        }
        Say(head + "before " + me);
        Enter(iAmHolder ? Stage::BeforePeer : Stage::Picked, now);
        return;
    case Stage::BeforePeer:
        if (!OtherSaid(isHost, head + "before " + peer)) { waitOn("before", "the line 'before " + peer + "'"); return; }
        Enter(Stage::Pickup, now);
        return;
    case Stage::Pickup: {
        std::vector<PropRow> rows;
        if (!RunCensus(Act::Pickup, n, holderIsHost, iAmHolder, since, &rows)) {
            waitOn("pickup", "this peer's census centre (the local player)");
            return;
        }
        const std::string why = StartPickup(rows, n);
        if (!why.empty()) { Fail(n, "pickup", why + "; episode cannot fire", false); return; }
        Enter(Stage::PickupWait, now);
        return;
    }
    case Stage::PickupWait:
        if (g_pick.active) { waitOn("pickup", "the end of the pickup"); return; }
        if (!ep.pickupHeld) {
            Fail(n, "pickup", "the game refused every candidate; episode cannot fire", false);
            return;
        }
        Say(head + "picked key=" + coop::text::ToUtf8(ep.targetKey));
        if (!RunCensus(Act::CensusHeld, n, holderIsHost, iAmHolder, since)) {
            Fail(n, "held", "no census centre on the holder", false);
            return;
        }
        Enter(Stage::HeldPeer, now);
        return;
    case Stage::HeldPeer: {
        if (!OtherSaid(isHost, head + "held " + peer)) { waitOn("held", "the line 'held " + peer + "'"); return; }
        void* player = MainPlayer();
        if (!player) { Fail(n, "drop", "the local player is gone", false); return; }
        void* wasHolding = HoldingActor(player);
        ep.dropFired   = CallDropHolding(player);
        ep.dropCleared = (HoldingActor(player) == nullptr);
        UE_LOGI("hand_drop_selftest: ep%d DROP key='%ls' -- was holding=%p call=%d cleared=%d",
                n, ep.targetKey.c_str(), wasHolding, ep.dropFired ? 1 : 0, ep.dropCleared ? 1 : 0);
        if (!ep.dropCleared) {
            Fail(n, "drop", "the hand did not empty (call=" + std::to_string(ep.dropFired ? 1 : 0) + ")", false);
            return;
        }
        g_dropped.Set(wasHolding);
        Enter(Stage::DropRest, now);
        return;
    }
    case Stage::DropRest: {
        // The released actor itself; when the game released a different one, the world prop the key names.
        void* a = g_dropped.Get();
        if (!a) a = coop::prop_key_index::Find(ep.targetKey);
        if (!a || !g_rest.Step(a)) { waitOn("drop", "the dropped prop at rest"); return; }
        Say(head + "dropped key=" + coop::text::ToUtf8(ep.targetKey));
        if (!RunCensus(Act::CensusAfter, n, holderIsHost, iAmHolder, since)) {
            Fail(n, "after", "no census centre on the holder", false);
            return;
        }
        Say(head + "after " + me);
        Enter(Stage::AfterPeer, now);
        return;
    }
    case Stage::Picked: {
        std::string key;
        if (!OtherSaid(isHost, head + "picked key=", &key)) { waitOn("held", "the line 'picked key='"); return; }
        std::wstring wkey;
        if (!coop::text::FromUtf8Strict(key.data(), key.size(), &wkey)) {
            Fail(n, "held", "the picked line carries an ill-formed key", false);
            return;
        }
        // This world's copy under that key, among the actors the before census saw, must be gone.
        for (const auto& b : ep.beforeActors)
            if (b.first == wkey && b.second.Alive()) { waitOn("held", "the world copy of the key leaving"); return; }
        if (!RunCensus(Act::CensusHeld, n, holderIsHost, iAmHolder, since)) {
            waitOn("held", "this peer's census centre (the holder's puppet)");
            return;
        }
        Say(head + "held " + me);
        Enter(Stage::DroppedWait, now);
        return;
    }
    case Stage::DroppedWait: {
        std::string key;
        std::wstring wkey;
        const char* what = nullptr;
        void* a = nullptr;
        if (!OtherSaid(isHost, head + "dropped key=", &key)) what = "the line 'dropped key='";
        else if (!coop::text::FromUtf8Strict(key.data(), key.size(), &wkey)) what = "a well-formed key in the dropped line";
        else if (!(a = coop::prop_key_index::Find(wkey))) what = "a live actor under the key";
        else if (!g_rest.Step(a)) what = "that actor at rest";
        if (what) {
            if (now - g_stageMs > kBoundMs) {
                // The census is the verdict (a key that did not come back reads as the loss), so it is taken
                // before the FAIL.
                RunCensus(Act::CensusAfter, n, holderIsHost, iAmHolder, since);
                Fail(n, "after", std::string(what) + " not seen", true);
            }
            return;
        }
        if (!RunCensus(Act::CensusAfter, n, holderIsHost, iAmHolder, since)) {
            waitOn("after", "this peer's census centre (the holder's puppet)");
            return;
        }
        Say(head + "after " + me);
        Enter(Stage::AfterPeer, now);
        return;
    }
    case Stage::AfterPeer:
        if (!OtherSaid(isHost, head + "after " + peer)) { waitOn("after", "the line 'after " + peer + "'"); return; }
        if (n >= kEpisodes) {
            Enter(Stage::Done, now);
            EmitVerdict();
            return;
        }
        ++g_episode;
        Enter(Stage::Before, now);
        return;
    case Stage::Done:
    case Stage::Failed:
        return;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    if (!Enabled()) return;
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!Enabled()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    const bool isHost = s->role() == coop::net::Role::Host;
    const uint64_t now = NowMs();
    if (!g_armedAtMs) {
        // The shared origin, the disc driver's: only a CLIENT announces ClientWorldReady, and the
        // host learns the same fact as that slot going world-ready. A client also waits for its join
        // to be over, the cover down, since its first line to the host is a chat line.
        if (isHost ? !s->IsSlotWorldReady(1) : !coop::dev::prop_act_drill::detail::OwnJoinOver()) return;
        g_armedAtMs = now;
        g_stageMs   = now;
        UE_LOGI("hand_drop_selftest: ARMED role=%s -- %d episodes, each step on what both peers show",
                isHost ? "HOST" : "CLIENT", kEpisodes);
    }
    PickupTick();
    if (now < g_nextCheckMs) return;
    g_nextCheckMs = now + kCheckEveryMs;
    PumpSay(isHost, now);
    Advance(isHost, now);
}

void EmitVerdict() {
    if (!Enabled() || !g_armedAtMs) return;
    for (int e = 1; e <= 4; ++e) {   // episode 0 is the hand-clearing setup, not an episode
        const EpisodeResult& ep = g_ep[e];
        UE_LOGW("hand_drop_selftest: VERDICT ep%d key='%ls' pickup(call=%d held=%d) "
                "drop(call=%d cleared=%d) census before=%d held=%d after=%d",
                e, ep.targetKey.c_str(), ep.pickupFired ? 1 : 0, ep.pickupHeld ? 1 : 0,
                ep.dropFired ? 1 : 0, ep.dropCleared ? 1 : 0, ep.before, ep.held, ep.after);
    }
}

void OnDisconnect() {
    g_armedAtMs = g_stageMs = g_nextCheckMs = g_nextSayMs = 0;
    g_stage   = Stage::Clear;
    g_episode = 0;
    g_out.clear();
    g_rest.Reset();
    g_dropped.Reset();
    for (auto& ep : g_ep) ep = EpisodeResult{};
    g_pick = PickupRun{};
    g_unreadable.clear();
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::dev::hand_drop_selftest
