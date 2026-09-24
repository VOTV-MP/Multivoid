// coop/props/trash_morph_gate.cpp -- see coop/props/trash_morph_gate.h.

#include "coop/props/trash_morph_gate.h"

#include "coop/net/session.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"        // ChipPileClass / GarbageClumpClass
#include "ue_wrap/core/reflection.h"    // FindDispatchFunction
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"   // the three verb names

#include <atomic>
#include <chrono>

namespace coop::trash_morph_gate {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;
namespace sg = ue_wrap::script_gate;

// The session the role gate reads. The verdicts carry no session of their own, so they read this
// cached pointer; Install re-stores it on every call and OnDisconnect clears it.
std::atomic<coop::net::Session*> g_session{nullptr};

// Registration is process-wide (a UFunction outlives a session), so the watches latch once all
// three are on; the role gate below is what turns the refusals on and off. A partial set never
// latches, or a verb that failed to resolve would stay ungated for the life of the process behind
// one warning.
bool g_installed = false;
std::chrono::steady_clock::time_point g_lastResolveAt{};

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

// Resolve one verb on `cls` and watch its body. FindDispatchFunction rather than FindFunction: it
// climbs and names the declaring class, and the answer here is expected to BE the base, which is
// what makes one watch cover every variant under it. An EXACT watch rather than a watch by name,
// because `playerGrabbed` is an interface event dozens of unrelated prop classes implement and a
// name watch would judge every one of them.
bool Register(void* cls, const wchar_t* fnName, int tag, sg::PreFn pre, const char* what) {
    if (!cls) return false;
    void* declarer = nullptr;
    void* fn = R::FindDispatchFunction(cls, fnName, &declarer);
    if (!fn) {
        UE_LOGW("trash_morph_gate: '%ls' not found on the trash class -- a client can still author "
                "%s locally", fnName, what);
        return false;
    }
    if (!sg::Watch(fn, tag, pre, nullptr)) {
        UE_LOGW("trash_morph_gate: the script-body gate refused the watch on '%ls' (not installed, "
                "native, or the table is full)", fnName);
        return false;
    }
    UE_LOGI("trash_morph_gate: watching %s at the script-body gate -- '%ls' declared on '%ls'",
            what, fnName, R::ToString(R::NameOf(declarer)).c_str());
    return true;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);  // re-cache every call (reconnect)
    if (g_installed) return;
    // FindClass does not memoise a miss, so an unresolved class walks the whole object array on
    // every attempt; this runs from the per-tick install pump, so the retry is throttled the way
    // the sibling trash lane throttles its own.
    const auto now = std::chrono::steady_clock::now();
    if (g_lastResolveAt.time_since_epoch().count() != 0 && (now - g_lastResolveAt) < std::chrono::seconds(2))
        return;
    g_lastResolveAt = now;
    void* pileCls  = ue_wrap::prop::ChipPileClass();
    void* clumpCls = ue_wrap::prop::GarbageClumpClass();
    if (!pileCls || !clumpCls) return;  // classes not loaded yet -> retry on a later tick
    int n = 0;
    n += Register(pileCls,  P::name::PileToClumpFn,       kTagToClump,      &OnToClump,       "a pile's morph to a clump") ? 1 : 0;
    n += Register(pileCls,  P::name::PilePlayerGrabbedFn, kTagPlayerGrab,   &OnPlayerGrabbed, "a pile's grab morph") ? 1 : 0;
    n += Register(clumpCls, P::name::ClumpContactFn,      kTagClumpContact, &OnClumpContact,  "a clump's re-pile") ? 1 : 0;
    if (n < 3) return;  // partial: retry rather than latch with a verb nobody is watching
    g_installed = true;
    UE_LOGI("trash_morph_gate: all 3 trash morph verbs watched -- on a client every pile-clump "
            "transition is the host's, delivered as PropConvert");
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
