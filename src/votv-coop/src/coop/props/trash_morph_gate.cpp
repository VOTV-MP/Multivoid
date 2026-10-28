// coop/props/trash_morph_gate.cpp -- see coop/props/trash_morph_gate.h.

#include "coop/props/trash_morph_gate.h"

#include "coop/net/session.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>

namespace coop::trash_morph_gate {
namespace {

namespace sg = ue_wrap::script_gate;

// The three trash morph verbs a client refuses. `toClump` is the pile's morph verb and covers every caller
// of it -- the collision component's overlap handler and the arir follower alike; `playerGrabbed` spawns
// the carried clump inline instead of calling it, so it needs its own watch; the clump's re-pile is inline
// in its ubergraph, so the contact handler that enters there is the verb. Each is declared on its own
// class with no subclass overriding it, so a Declared row covers every variant under the class.
UE_FACT_FN(kRowToClump, "trash_morph_gate", L"actorChipPile_C", L"toClump", Declared);
UE_FACT_FN(kRowPlayerGrabbed, "trash_morph_gate", L"actorChipPile_C", L"playerGrabbed", Declared);
UE_FACT_FN(kRowClumpContact, "trash_morph_gate", L"prop_garbageClump_C",
           L"BndEvt__prop_garbageClump_StaticMesh_K2Node_ComponentBoundEvent_0_ComponentHitSignature__DelegateSignature",
           Declared);

// The session the role gate reads. The verdicts carry no session of their own, so they read this
// cached pointer; Install re-stores it at every session start and OnDisconnect clears it.
std::atomic<coop::net::Session*> g_session{nullptr};

// Registration is process-wide (a name watch matches every incarnation of its class), so the watches
// are registered once, and the flag is set only when all three took: a partial set is retried by the
// next Install. The role gate below is what turns the refusals on and off.
std::atomic<bool> g_watched{false};

// Set by Tick, game thread only: the three watches settled (live, or dead for good). Process-wide like
// the registration, so a reconnect does not repeat the check.
bool g_live = false;

// The watch tags, distinct so the gate's tables keep them apart.
constexpr int kTagToClump      = 1;
constexpr int kTagPlayerGrab   = 2;
constexpr int kTagClumpContact = 3;

// True while this peer must not author a trash transition: a live session in which we are not the
// host. Single-player and the host both answer false, and every verb then runs exactly as the
// game wrote it.
bool MustNotAuthor() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->connected() && s->role() != coop::net::Role::Host;
}

// What each verb has been seen doing. A gate that never says it fired cannot be told apart from
// one whose verb never dispatches, and the two call for opposite fixes. Game-thread serial (the
// gate skips and counts any off-thread body), so plain ints.
struct VerbTally { const char* what; int seen = 0; int refused = 0; };
VerbTally g_toClump{"a pile's morph to a clump"};
VerbTally g_playerGrab{"a pile's grab morph"};
VerbTally g_clumpContact{"a clump's re-pile"};

// Each of these spawns the successor and destroys the actor the body runs on, which on a client
// destroys the mirror the host is driving and leaves an actor no element id owns. Refusing the
// body leaves the mirror where the host put it, and the host's own PropConvert performs the
// transition.
sg::Verdict Judge(VerbTally& t, const sg::Call& call) {
    ++t.seen;
    const bool refuse = MustNotAuthor();
    if (refuse) ++t.refused;
    // The first three only. A clump at rest dispatches its contact handler about eighty times a
    // second for as long as it lies there, so anything periodic here is a line a second forever;
    // the session tally carries the count instead.
    if (t.seen <= 3)
        UE_LOGI("trash_morph_gate: %s #%d on %p -- %s",
                t.what, t.seen, call.object, refuse ? "REFUSED" : "allowed (this peer authors it)");
    return refuse ? sg::Verdict::Cancel : sg::Verdict::Run;
}

sg::Verdict OnToClump(const sg::Call& call)       { return Judge(g_toClump, call); }
sg::Verdict OnPlayerGrabbed(const sg::Call& call) { return Judge(g_playerGrab, call); }
sg::Verdict OnClumpContact(const sg::Call& call)  { return Judge(g_clumpContact, call); }

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);  // re-cache at every session start
    if (g_watched.load(std::memory_order_acquire)) return;
    // Name watches, registered at session start before the trash classes need to be loaded: a watch
    // matches by the owning class's name once its names are resolved, whether or not the class has
    // loaded yet, so a class that loads later is judged from its first instance. A Declared row, not
    // an exact watch on a resolved function: `playerGrabbed` is an interface event dozens of
    // unrelated prop classes implement, and the row's class keeps a name watch from judging them.
    struct Row { const ue_wrap::facts::Function* row; int tag; sg::PreFn pre; };
    const Row rows[] = {
        {&kRowToClump,       kTagToClump,      &OnToClump},
        {&kRowPlayerGrabbed, kTagPlayerGrab,   &OnPlayerGrabbed},
        {&kRowClumpContact,  kTagClumpContact, &OnClumpContact},
    };
    int watched = 0;
    for (const Row& r : rows) {
        if (sg::WatchName(*r.row, r.tag, r.pre, nullptr)) {
            ++watched;
        } else {
            UE_LOGW("trash_morph_gate: the watch on %ls::%ls did not register -- a client can still author "
                    "that transition locally", r.row->Class(), r.row->Name());
        }
    }
    // Install runs on the TimelineThread, where ResolvePendingNames does nothing; WatchName posted one
    // resolve to the game thread, and Tick drives the rest until all three watches are live.
    if (watched != 3) {
        UE_LOGW("trash_morph_gate: only %d/3 trash morph verbs registered -- the next session start retries", watched);
        return;
    }
    g_watched.store(true, std::memory_order_release);
    UE_LOGI("trash_morph_gate: 3/3 trash morph verbs registered by class and name -- live once their names "
            "resolve (Tick says when)");
}

void Tick() {
    if (g_live || !g_watched.load(std::memory_order_acquire)) return;
    sg::ResolvePendingNames();
    const bool live = sg::NameWatchLive(kRowToClump, kTagToClump) &&
                      sg::NameWatchLive(kRowPlayerGrabbed, kTagPlayerGrab) &&
                      sg::NameWatchLive(kRowClumpContact, kTagClumpContact);
    if (!live) {
        // A watch dead for good (its class declares no such function) never goes live: say so once, stop.
        if (!(sg::NameWatchSettled(kRowToClump, kTagToClump) && sg::NameWatchSettled(kRowPlayerGrabbed, kTagPlayerGrab) &&
              sg::NameWatchSettled(kRowClumpContact, kTagClumpContact)))
            return;
        UE_LOGW("trash_morph_gate: the three watches settled and not all are live -- a client can still "
                "author a trash transition locally");
        g_live = true;
        return;
    }
    g_live = true;
    UE_LOGI("trash_morph_gate: 3/3 trash morph verbs live -- on a client every pile-clump transition is "
            "the host's, delivered as PropConvert");
}

void OnSessionStart() {
    g_toClump.seen = g_toClump.refused = 0;
    g_playerGrab.seen = g_playerGrab.refused = 0;
    g_clumpContact.seen = g_clumpContact.refused = 0;
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
    // The session's tally, once. Zero SEEN on a verb means the seam never dispatched in this
    // session, which is a fact about the game, not about the gate.
    UE_LOGI("trash_morph_gate: session tally -- to-clump %d/%d refused, grab %d/%d, re-pile %d/%d",
            g_toClump.refused, g_toClump.seen, g_playerGrab.refused, g_playerGrab.seen,
            g_clumpContact.refused, g_clumpContact.seen);
}

}  // namespace coop::trash_morph_gate
