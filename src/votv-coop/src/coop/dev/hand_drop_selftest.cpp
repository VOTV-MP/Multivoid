// coop/dev/hand_drop_selftest.cpp -- see coop/dev/hand_drop_selftest.h.

#include "coop/dev/hand_drop_selftest.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"        // the hand axis a census must not count
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"    // the holder's puppet, for the watcher's census centre
#include "coop/session/net_pump.h"        // HasAnnouncedWorldReady

#include "ue_wrap/actors/prop.h"
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
#include <string>
#include <vector>

namespace coop::dev::hand_drop_selftest {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace P  = ue_wrap::profile;
namespace PR = ue_wrap::prop;

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

// ---- the world census -------------------------------------------------------------------------

struct PropRow {
    void*        actor = nullptr;
    std::wstring key;
    std::wstring cls;
    float        distCm = 0.f;
};

// Every live keyed prop within `radiusCm` of `centre`, sorted by key so both peers print the same
// list in the same order. The hand axis is excluded on both roles: a peer's display mirror is a
// real actor of the held item's class sitting at that peer's hands, so counting it would hide
// exactly the loss this driver measures -- the census would read "still one prop here" while the
// only thing there is the mirror that is about to be destroyed.
std::vector<PropRow> Census(const ue_wrap::FVector& centre, float radiusCm) {
    std::vector<PropRow> out;
    void* handAxis[1 + coop::players::kMaxPeers];
    const size_t handAxisN =
        coop::hand_item::CollectHandAxisActors(handAxis, 1 + coop::players::kMaxPeers);
    const float r2 = radiusCm * radiusCm;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        if (!PR::IsDescendantOfProp(obj)) continue;
        if (!R::IsLive(obj)) continue;
        if (E::IsChildActor(obj)) continue;
        bool isHandAxis = false;
        for (size_t h = 0; h < handAxisN; ++h) if (handAxis[h] == obj) { isHandAxis = true; break; }
        if (isHandAxis) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
        const ue_wrap::FVector loc = E::GetActorLocation(obj);
        const float dx = loc.X - centre.X, dy = loc.Y - centre.Y, dz = loc.Z - centre.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > r2) continue;
        PropRow row;
        row.actor  = obj;
        row.key    = PR::GetKeyString(obj);
        row.cls    = R::ClassNameOf(obj);
        row.distCm = std::sqrt(d2);
        if (row.key.empty() || row.key == L"None") continue;  // keyless props ride other lanes
        out.push_back(std::move(row));
    }
    std::sort(out.begin(), out.end(),
              [](const PropRow& a, const PropRow& b) { return a.key < b.key; });
    return out;
}

// What this census has that the baseline did not, and what the baseline had that it lost. Both
// lists are keys, so the two peers' logs name the same props.
std::wstring DescribeDiff(const std::vector<std::wstring>& baseline,
                          const std::vector<PropRow>& now) {
    auto has = [](const std::vector<std::wstring>& v, const std::wstring& k) {
        return std::find(v.begin(), v.end(), k) != v.end();
    };
    std::vector<std::wstring> nowKeys;
    nowKeys.reserve(now.size());
    for (const PropRow& r : now) nowKeys.push_back(r.key);

    std::wstring added, gone;
    for (const PropRow& r : now)
        if (!has(baseline, r.key)) { if (!added.empty()) added += L", "; added += L"'" + r.key + L"' (" + r.cls + L")"; }
    for (const std::wstring& k : baseline)
        if (!has(nowKeys, k)) { if (!gone.empty()) gone += L", "; gone += L"'" + k + L"'"; }
    std::wstring s = L"gained[" + (added.empty() ? std::wstring(L"none") : added) + L"] lost[" +
                     (gone.empty() ? std::wstring(L"none") : gone) + L"]";
    return s;
}

// ---- the schedule -----------------------------------------------------------------------------

enum class Act { ClearHand, CensusBefore, Pickup, CensusHeld, Drop, CensusAfter };

struct Step {
    int      episode;
    uint64_t atMs;
    bool     holderIsHost;   // the role that acts; the other role censuses the same moment
    Act      act;
};

// Four episodes, alternating which peer holds, each direction twice: one pass of an ordering is an
// anecdote, and the loss this measures was first read out of a single field log. The gaps are wide
// next to the hand lane's own edge-instant announce and the 1 Hz polls around it.
constexpr Step kSteps[] = {
    // Both peers start possessed and holding whatever the save gave them, so the first episode
    // would open with a full hand and no pickup. Each peer empties its own hand first; the prop
    // that lands is inside every census that follows, so it changes no diff.
    {0,  6000, true,  Act::ClearHand},

    {1, 10000, true,  Act::CensusBefore},
    {1, 12000, true,  Act::Pickup},
    {1, 16000, true,  Act::CensusHeld},
    {1, 18000, true,  Act::Drop},
    {1, 23000, true,  Act::CensusAfter},

    {2, 28000, false, Act::CensusBefore},
    {2, 30000, false, Act::Pickup},
    {2, 34000, false, Act::CensusHeld},
    {2, 36000, false, Act::Drop},
    {2, 41000, false, Act::CensusAfter},

    {3, 46000, true,  Act::CensusBefore},
    {3, 48000, true,  Act::Pickup},
    {3, 52000, true,  Act::CensusHeld},
    {3, 54000, true,  Act::Drop},
    {3, 59000, true,  Act::CensusAfter},

    {4, 64000, false, Act::CensusBefore},
    {4, 66000, false, Act::Pickup},
    {4, 70000, false, Act::CensusHeld},
    {4, 72000, false, Act::Drop},
    {4, 77000, false, Act::CensusAfter},
};
constexpr size_t kStepCount = sizeof(kSteps) / sizeof(kSteps[0]);
constexpr uint64_t kVerdictMs = 82000;

uint64_t g_armedAtMs = 0;
size_t   g_next = 0;
bool     g_verdictPrinted = false;

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

// The game's own pickup, with the target handed in directly: `Hold Object` reads `manual` when it
// is valid and only falls back to whatever the player is looking at when it is not, so a driver
// needs no aim and no hit result. The name carries a space, which is a legal FName. It ends in
// addEquip, which writes the hold slot and calls updateHold, so a collected prop IS a hand item --
// and it destroys the world actor on the way, which is why a census taken while it is held reads
// one row shorter on BOTH peers.
//
// `collected` is the verb's own answer and the only honest one: the call returning says the frame
// dispatched, while every refusal inside -- not simulating physics, too heavy, cannot be held --
// leaves collected false and the hand empty. Whether a called verb's body ran is never inferred
// from callability.
bool CallHoldObject(void* player, void* target, bool& collectedOut) {
    collectedOut = false;
    void* cls = R::FindClass(P::name::MainPlayerClass);
    void* fn  = cls ? R::FindFunction(cls, L"Hold Object") : nullptr;
    if (!fn) {
        UE_LOGW("hand_drop_selftest: 'Hold Object' unresolved on %ls -- cannot drive a pickup",
                P::name::MainPlayerClass);
        return false;
    }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    const uint32_t useHold = 0;   // false: take `manual`, not the grab slot
    f.Set(L"useHold", useHold);
    f.Set(L"manual", target);
    if (!ue_wrap::Call(player, f)) return false;
    collectedOut = f.Get<uint32_t>(L"collected") != 0;
    return true;
}

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

// ---- where each role censuses ------------------------------------------------------------------

// The holder censuses around itself; the watcher censuses around the holder's puppet, so both
// peers measure the same region of the same world. Returns false when the watcher has no puppet
// yet, which is a reason printed rather than a census of the wrong place.
bool CensusCentre(bool iAmHolder, bool holderIsHost, ue_wrap::FVector& out) {
    if (iAmHolder) {
        void* p = MainPlayer();
        if (!p) return false;
        out = E::GetActorLocation(p);
        return true;
    }
    const uint8_t holderSlot = holderIsHost ? 0u : 1u;
    coop::RemotePlayer* pup = coop::players::Registry::Get().Puppet(holderSlot);
    if (!pup || !pup->valid()) return false;
    void* actor = pup->GetActor();
    if (!actor) return false;
    out = E::GetActorLocation(actor);
    return true;
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

void RunStep(const Step& st, bool iAmHolder, uint64_t since) {
    if (st.act == Act::ClearHand) {
        // Not a holder/watcher step: every peer empties its own hand.
        void* player = MainPlayer();
        void* holding = HoldingActor(player);
        if (!holding) {
            UE_LOGI("hand_drop_selftest: CLEARHAND (+%llus) -- the hand is already empty",
                    static_cast<unsigned long long>(since / 1000));
            return;
        }
        const bool called = CallDropHolding(player);
        UE_LOGI("hand_drop_selftest: CLEARHAND (+%llus) -- dropped the starting hand item %p "
                "(call=%d, now holding=%p)", static_cast<unsigned long long>(since / 1000),
                holding, called ? 1 : 0, HoldingActor(player));
        return;
    }
    ue_wrap::FVector centre{};
    if (!CensusCentre(iAmHolder, st.holderIsHost, centre)) {
        UE_LOGW("hand_drop_selftest: ep%d %s (+%llus) -- no census centre on this peer "
                "(%s); step SKIPPED", st.episode, ActName(st.act),
                static_cast<unsigned long long>(since / 1000),
                iAmHolder ? "local player not possessed" : "the holder's puppet is not up here");
        return;
    }
    const std::vector<PropRow> rows = Census(centre, kCensusRadiusCm);
    EpisodeResult& ep = g_ep[st.episode];
    const int count = static_cast<int>(rows.size());

    switch (st.act) {
        case Act::CensusBefore:
            ep.before = count;
            ep.beforeKeys.clear();
            for (const PropRow& r : rows) ep.beforeKeys.push_back(r.key);
            break;
        case Act::CensusHeld:   ep.held  = count; break;   // the watcher's inference follows below
        case Act::CensusAfter:  ep.after = count; break;
        default: break;
    }

    UE_LOGI("hand_drop_selftest: ep%d %s (+%llus) role=%s holder=%s -- %d keyed prop(s) within "
            "%.0fcm; vs before: %ls", st.episode, ActName(st.act),
            static_cast<unsigned long long>(since / 1000), iAmHolder ? "HOLDER" : "WATCHER",
            st.holderIsHost ? "HOST" : "CLIENT", count, kCensusRadiusCm,
            DescribeDiff(ep.beforeKeys, rows).c_str());

    if (!iAmHolder && st.act == Act::CensusHeld && ep.targetKey.empty()) {
        // The watcher is told nothing: it works out which prop the holder took by watching one
        // leave its own world. A pickup destroys the world actor on every peer, so exactly one key
        // should be missing here; none means the pickup never crossed, and more than one means
        // something else moved at the same time and the episode cannot name a subject.
        std::vector<std::wstring> gone;
        for (const std::wstring& k : ep.beforeKeys) {
            bool present = false;
            for (const PropRow& r : rows) if (r.key == k) { present = true; break; }
            if (!present) gone.push_back(k);
        }
        if (gone.size() == 1) {
            ep.targetKey = gone[0];
            UE_LOGI("hand_drop_selftest: ep%d WATCHER subject inferred -- key='%ls' left this world "
                    "when the holder picked it up", st.episode, ep.targetKey.c_str());
        } else {
            UE_LOGW("hand_drop_selftest: ep%d WATCHER cannot name a subject -- %zu key(s) left this "
                    "world at the held census (expected exactly one); the pickup either did not "
                    "cross or something else moved with it", st.episode, gone.size());
        }
    }

    if (!iAmHolder) {
        // The watcher's verdict, said the moment it can be said: a prop that went into a hand and
        // came back out is a row restored here. A census that stays short after the drop is the
        // loss -- on this peer the dropped prop exists nowhere, and whatever its wire identity is
        // bound to, it is not an actor in this world.
        if (st.act == Act::CensusAfter && ep.before >= 0) {
            // The prop the holder moved is named by the key it carried before the pickup, so the
            // verdict is about that key and not about a count that two unrelated changes can
            // balance. A key that is gone from this peer after the drop is the loss: the holder
            // has the prop and this peer has nothing bound to it.
            bool stillHere = false;
            for (const PropRow& r : rows) if (r.key == ep.targetKey) { stillHere = true; break; }
            if (ep.targetKey.empty())
                UE_LOGI("hand_drop_selftest: ep%d WATCHER -- the holder named no key this episode; "
                        "%d before, %d after", st.episode, ep.before, ep.after);
            else if (!stillHere)
                // Two readings, and this peer cannot tell them apart from here: the release never
                // happened, or it happened and did not arrive. The holder's own log settles it --
                // its DROP line carries cleared=1 only when the hand actually emptied -- so the
                // verdict names both rather than asserting the interesting one.
                UE_LOGW("hand_drop_selftest: ep%d WATCHER DOES NOT HAVE THE PROP -- key='%ls' is "
                        "not here after the drop (%d before, %d after). Read the holder's DROP "
                        "line: cleared=1 means it was released and this peer lost it; cleared=0 "
                        "means it is still in that hand and there is nothing to have",
                        st.episode, ep.targetKey.c_str(), ep.before, ep.after);
            else
                UE_LOGI("hand_drop_selftest: ep%d WATCHER OK -- key='%ls' is here after the drop "
                        "(%d before, %d after)", st.episode, ep.targetKey.c_str(), ep.before,
                        ep.after);
        }
        return;
    }

    void* player = MainPlayer();
    if (!player) return;

    if (st.act == Act::Pickup) {
        if (HoldingActor(player)) {
            UE_LOGW("hand_drop_selftest: ep%d PICKUP -- the hand is already full; episode cannot fire",
                    st.episode);
            return;
        }
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
            UE_LOGW("hand_drop_selftest: ep%d PICKUP -- no keyed prop within %.0fcm of the holder; "
                    "episode cannot fire", st.episode, kTargetRadiusCm);
            return;
        }
        int tried = 0;
        for (const PropRow* r : byDistance) {
            if (tried >= kMaxPickupAttempts) break;
            ++tried;
            if (!R::IsLive(r->actor)) continue;
            bool collected = false;
            const bool called = CallHoldObject(player, r->actor, collected);
            void* nowHolding = HoldingActor(player);
            UE_LOGI("hand_drop_selftest: ep%d PICKUP try %d key='%ls' cls='%ls' d=%.0fcm -- call=%d "
                    "collected=%d holding=%p", st.episode, tried, r->key.c_str(), r->cls.c_str(),
                    r->distCm, called ? 1 : 0, collected ? 1 : 0, nowHolding);
            if (nowHolding) {
                ep.targetKey   = r->key;
                ep.pickupFired = called;
                ep.pickupHeld  = true;
                UE_LOGI("hand_drop_selftest: ep%d PICKUP OK after %d attempt(s) -- key='%ls' is in "
                        "the hand as actor %p (the world copy is destroyed; a census here reads one "
                        "row shorter until the drop)", st.episode, tried, r->key.c_str(), nowHolding);
                return;
            }
        }
        UE_LOGW("hand_drop_selftest: ep%d PICKUP -- %d candidate(s) tried, the game refused every "
                "one; episode cannot fire", st.episode, tried);
        return;
    }

    if (st.act == Act::Drop) {
        void* wasHolding = HoldingActor(player);
        ep.dropFired   = CallDropHolding(player);
        ep.dropCleared = (HoldingActor(player) == nullptr);
        UE_LOGI("hand_drop_selftest: ep%d DROP key='%ls' -- was holding=%p call=%d cleared=%d",
                st.episode, ep.targetKey.c_str(), wasHolding, ep.dropFired ? 1 : 0,
                ep.dropCleared ? 1 : 0);
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
        // host learns the same fact as that slot going world-ready. Anchoring on this peer's own
        // connect instead puts the two clocks a world-load apart.
        if (isHost ? !s->IsSlotWorldReady(1) : !coop::net_pump::HasAnnouncedWorldReady()) return;
        g_armedAtMs = now;
        UE_LOGI("hand_drop_selftest: ARMED role=%s -- %zu steps, first at +%llus, verdict at +%llus",
                isHost ? "HOST" : "CLIENT", kStepCount,
                static_cast<unsigned long long>(kSteps[0].atMs / 1000),
                static_cast<unsigned long long>(kVerdictMs / 1000));
    }
    const uint64_t since = now - g_armedAtMs;
    while (g_next < kStepCount && since >= kSteps[g_next].atMs) {
        const Step& st = kSteps[g_next];
        ++g_next;
        RunStep(st, isHost == st.holderIsHost, since);
    }
    if (!g_verdictPrinted && since >= kVerdictMs) {
        g_verdictPrinted = true;
        EmitVerdict();
    }
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
    g_armedAtMs = 0;
    g_next = 0;
    g_verdictPrinted = false;
    for (auto& ep : g_ep) ep = EpisodeResult{};
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::dev::hand_drop_selftest
