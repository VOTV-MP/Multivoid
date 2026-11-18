// coop/props/body_push.cpp -- see coop/props/body_push.h.

#include "coop/props/body_push.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/throw_bounds.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/props/prop_hold_claim.h"
#include "coop/props/prop_rest_sync.h"
#include "coop/props/prop_syncer.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/walk_timer.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/world_identity.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace coop::props::body_push {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
using ue_wrap::FRotator;
using ue_wrap::FVector;
using Clock = std::chrono::steady_clock;

constexpr float kTouchCm = 150.f;   // a push's prop, from this player's body
constexpr float kMoveCm = 3.f;      // a move between two reads
constexpr float kTurnDeg = 3.f;     // a turn between two reads
constexpr float kCarryCm = 500.f;   // a driven vehicle this near a moved prop carries it: the vehicle's lane moved it
constexpr size_t kListCap = 64;
constexpr auto kListEvery = std::chrono::seconds(1);
constexpr auto kReadEvery = std::chrono::milliseconds(100);
// The props kept to watch, from this player's body: what a player at the pawn's top speed covers in the one second between two
// list refreshes, and the touch distance on top, so a runner reaches no prop the list did not already hold.
static_assert(kListEvery == std::chrono::seconds(1), "kListCm assumes a one second refresh");
constexpr float kListCm = coop::net::throw_bounds::kPawnTopSpeedCmS + kTouchCm;

struct Watched {
    ue_wrap::CachedObjRef ref;
    FVector loc{};
    FRotator rot{};
    float nearestCm2 = 0.f;   // the squared distance to this player's body when listed: what the cap cuts by
    bool rebase = false;      // a write of this peer's own moved it: the next read re-anchors and is no push (Rebase)
};
std::vector<Watched> g_list;
// The prop classes, found again only when the class set changes, as the food and roomba lanes keep theirs.
std::vector<void*> g_classes;
uint64_t g_classVersion = ~0ull;
Clock::time_point g_nextList{}, g_nextRead{};
uint32_t g_pushes = 0;
uint32_t g_settles = 0;   // the host's settle coasts (Settle)
uint64_t g_cut = 0;   // the refreshes that found more props in reach than the list holds

// Why a move of a prop within this player's reach was not taken as a push: counted per reason and said at the first three
// and each doubling, so a push that opened no claim or row names the test that stopped it.
enum class Skip : uint8_t { NotPushable, Parked, ClaimNotSent, Driven, Capped, NotCoasted, kCount };
constexpr const char* kSkipName[] = {"not a prop this lane pushes (unkeyed, held, a vehicle's or on one)",
                                     "the copy is parked by a stream", "no claim was sent (held off, open or refused)",
                                     "a row already streams it", "the coast cap is reached", "the coast opened no row"};
uint32_t g_skips[static_cast<size_t>(Skip::kCount)] = {};

void NoteSkip(Skip why, void* actor) {
    uint32_t& n = g_skips[static_cast<size_t>(why)];
    ++n;
    if (n <= 3 || (n & (n - 1)) == 0)
        UE_LOGI("body_push: a move of '%ls' %p within reach of this player was not taken as a push: %s (#%u)",
                R::ClassNameOf(actor).c_str(), actor, kSkipName[static_cast<size_t>(why)], n);
}

// This peer's own player's body: the only one a push is read from. A push is the pusher's own contact on the pusher's own peer
// (MTA's push is the local player's contact, CClientGame.cpp:4712-4733); a puppet's contact on the peer that shows it is not
// claimed, so the wrong peer never syncs the prop.
// On the HOST the other players' puppets are read too, for the settle watch only: a prop near any player that moves
// with no hand, row or host body on it is a settle the host streams (Settle below). Never a push of theirs.
struct Body {
    FVector at{};
    bool have = false;
    FVector others[coop::net::kMaxPeers]{};
    int nOthers = 0;
};

void ReadBody(Body& b, bool isHost) {
    void* me = coop::players::Registry::Get().Local();
    b.have = me && E::TryGetRootLocationRaw(me, b.at);
    b.nOthers = 0;
    if (!isHost) return;
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
        void* actor = rp && rp->valid() ? rp->actor() : nullptr;
        if (actor && E::TryGetRootLocationRaw(actor, b.others[b.nOthers])) ++b.nOthers;
    }
}

float Dist2(const FVector& a, const FVector& b) {
    const float x = a.X - b.X, y = a.Y - b.Y, z = a.Z - b.Z;
    return x * x + y * y + z * z;
}

// The squared distance from `at` to this peer's own body; large when it is not read.
float BodyCm2(const Body& b, const FVector& at) { return b.have ? Dist2(at, b.at) : 1.0e18f; }

// The squared distance from `at` to the nearest other player's puppet (the host's settle watch); large when none is read.
float OthersCm2(const Body& b, const FVector& at) {
    float best = 1.0e18f;
    for (int i = 0; i < b.nOthers; ++i) best = std::fmin(best, Dist2(at, b.others[i]));
    return best;
}

// The squared distance from `at` to the nearest body this peer watches round: its own, and on the host every puppet's.
float NearestCm2(const Body& b, const FVector& at) { return std::fmin(BodyCm2(b, at), OthersCm2(b, at)); }

float TurnDeg(const FRotator& a, const FRotator& b) {
    const float p = std::fabs(ue_wrap::NormalizeAxis(a.Pitch - b.Pitch));
    const float y = std::fabs(ue_wrap::NormalizeAxis(a.Yaw - b.Yaw));
    const float r = std::fabs(ue_wrap::NormalizeAxis(a.Roll - b.Roll));
    return std::fmax(p, std::fmax(y, r));
}

struct ListCtx {
    const Body* body;
    std::vector<Watched>* out;
    void* world;
};

void EachProp(void* ctx, void* obj, int32_t) {
    auto* c = static_cast<ListCtx*>(ctx);
    if (ue_wrap::world_identity::WorldOf(obj) != c->world) return;   // another world's, and the class default objects
    FVector loc{};
    FRotator rot{};
    if (!E::TryGetRootPoseRaw(obj, loc, rot)) return;
    const float near2 = NearestCm2(*c->body, loc);
    if (near2 > kListCm * kListCm) return;
    Watched w;
    w.ref.Set(obj);
    w.loc = loc;
    w.rot = rot;
    w.nearestCm2 = near2;
    c->out->push_back(w);
}

void RefreshClasses() {
    const uint64_t version = ue_wrap::object_index::ClassSetVersion();
    if (version == g_classVersion) return;
    g_classVersion = version;
    g_classes.clear();
    ue_wrap::object_index::ForEachClass(
        [](void*, void* cls, void*) {
            if (ue_wrap::prop::IsClassDescendantOfProp(cls)) g_classes.push_back(cls);
        },
        nullptr);
}

struct AtvCtx {
    FVector at;
    bool carried = false;
};

void EachAtv(void* ctx, void* atv, int32_t) {
    auto* c = static_cast<AtvCtx*>(ctx);
    FVector root{};
    FRotator rot{};
    if (c->carried || !ue_wrap::atv::IsDriven(atv) || !ue_wrap::atv::GetRootTransform(atv, root, rot)) return;
    if (Dist2(root, c->at) <= kCarryCm * kCarryCm) c->carried = true;
}

// A prop this lane may push: a keyed prop under the one-holder rule, not an ATV (its own lane), nobody holding it, and
// nothing a vehicle carries. A kerfur's or a hand's carry attaches the prop, which the raw pose read never serves, so it
// never reaches here attached; a prop on a driven ATV's bed moves with it and is the vehicle lane's.
bool Pushable(void* actor, const FVector& at) {
    if (!coop::prop_hold_claim::IsArbitrated(actor)) return false;
    if (ue_wrap::atv::IsAtv(actor) || coop::prop_drive_host::InAHand(actor)) return false;
    AtvCtx atvs{at};
    ue_wrap::atv::ForEachAtv(&EachAtv, &atvs);
    return !atvs.carried;
}

// A prop that moved more down than sideways fell: a hand let go of it, it was born in the air, a shelf gave way. Gravity
// moved it and no body did, and the host's own coasts (a let-go's, a birth's) carry that fall.
bool Fell(const FVector& from, const FVector& to) {
    const float dx = to.X - from.X, dy = to.Y - from.Y, dz = to.Z - from.Z;
    return dz < 0.f && dz * dz > dx * dx + dy * dy;
}

// The body-push drill's host red arm: a push is seen and never streamed by the host. Resolved once.
bool PushOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::body_push_off);
    return off;
}

// The body-push drill's client red arm: this client's push sends no claim, so each body moves its own copy. Resolved once.
bool ClaimOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::body_push_claim_off);
    return off;
}

// The host's settle watch off (VOTVCOOP_SETTLE_OFF=1): every peer's copy of a prop that falls with nothing on it settles by
// its own physics again -- the red arm of the prop accuracy drill. Resolved once.
bool SettleOff() {
    static const bool off = coop::config::ReadEnv("VOTVCOOP_SETTLE_OFF") == "1";
    return off;
}

// The nearest kListCap of the props in reach, as the list: said when the cap cuts, at the first three and each doubling.
void KeepNearest(std::vector<Watched>& fresh, size_t bodies) {
    const size_t cap = kListCap * bodies;
    if (fresh.size() <= cap) return;
    std::nth_element(fresh.begin(), fresh.begin() + static_cast<std::ptrdiff_t>(cap), fresh.end(),
                     [](const Watched& a, const Watched& b) { return a.nearestCm2 < b.nearestCm2; });
    const uint64_t n = ++g_cut;
    if (n <= 3 || (n & (n - 1)) == 0)
        UE_LOGI("body_push: %zu props within %.0f cm of a body, watching the nearest %zu; cut %llu time(s) this session",
                fresh.size(), kListCm, cap, static_cast<unsigned long long>(n));
    fresh.resize(cap);
}

// HOST: a prop near some player moved with no hand, no row and no host body on it -- the plate on a chair a client picked
// up, a can a thrown box knocked, a stack that slid. Every peer's copy used to fall by its own physics and come to rest
// where it would: the plate 55 cm and 110 degrees from the host's. The host's copy is the one truth, so the host streams it
// until it rests (a Settle coast), and every other copy is parked and follows it, its End the resting pose. A move a
// client's own body makes near its puppet is left to that client's claim (a push, not a fall).
void Settle(void* actor, const FVector& at, const Body& body, bool fell) {
    if (!fell && OthersCm2(body, at) <= kTouchCm * kTouchCm) return;
    if (coop::prop_drive_host::IsDriven(actor)) return;   // a row streams it already: a let-go's, a verb's, a client's
    if (!Pushable(actor, at)) return;
    if (SettleOff()) return;
    if (coop::prop_drive_host::CoastCapReached(actor, "settle")) { NoteSkip(Skip::Capped, actor); return; }
    if (!coop::prop_drive_host::Coast(actor, "settle", coop::prop_drive_host::CoastCause::Settle)) {
        NoteSkip(Skip::NotCoasted, actor);
        return;
    }
    ++g_settles;
    if (g_settles <= 3 || (g_settles % 50) == 0)
        UE_LOGI("body_push: '%ls' %p moved with nothing on it (%s) -- the host streams its settle (#%u)",
                R::ClassNameOf(actor).c_str(), actor, fell ? "a fall" : "a slide", g_settles);
}

}  // namespace

void Tick(coop::net::Session& s, bool isHost) {
    UE_ASSERT_GAME_THREAD("body_push::Tick");
    if (!s.connected()) return;
    Body body;
    bool read = false;
    const Clock::time_point now = Clock::now();
    if (now >= g_nextList) {
        g_nextList = now + kListEvery;
        void* world = ue_wrap::world_identity::CurrentWorld();
        if (world) {
            ue_wrap::ScopedWalkTimer walk{"body_push:list"};
            ReadBody(body, isHost);
            read = true;
            RefreshClasses();
            std::vector<Watched> fresh;
            ListCtx ctx{&body, &fresh, world};
            for (void* cls : g_classes) ue_wrap::object_index::ForEachInstance(cls, &EachProp, &ctx);
            KeepNearest(fresh, 1 + static_cast<size_t>(body.nOthers));
            // Keep the last read of a prop already watched, so a move across the refresh is not lost.
            for (Watched& f : fresh)
                for (const Watched& old : g_list)
                    if (old.ref.Raw() == f.ref.Raw()) {
                        f.loc = old.loc;
                        f.rot = old.rot;
                        f.rebase = old.rebase;
                    }
            g_list.swap(fresh);
        }
    }
    // A poll, under the rule's own exception: native code announces a body's push and none of our seams sees it. MTA keys
    // the push on the engine's collision between the vehicle and the local player (the client's CClientGame.cpp:4712-4733).
    // Here, with a box dropped on the pushed prop as a known hit and its body's hit-notify flag
    // read on, ReceiveHit counted 0 on the prop and on both pawns (an event its class does not implement never reaches
    // ProcessEvent: docs/coop-dispatch-visibility.md), prop_C's own hit handler counted 0 (the game binds it to the actor's
    // hit event only while its own distance-sleep flag is set, never beside a player), OnComponentHit is a multicast no seam of
    // ours sees, and NotifyHit is native. The body-push drill's HITS lines carry the counts.
    if (now < g_nextRead) return;
    g_nextRead = now + kReadEvery;
    if (!read) ReadBody(body, isHost);
    for (Watched& w : g_list) {
        void* actor = w.ref.Get();
        FVector loc{};
        FRotator rot{};
        if (!actor || !E::TryGetRootPoseRaw(actor, loc, rot)) continue;
        // This peer's own write put the prop here (an End, a correction snap): the pose is the new anchor, no push.
        if (w.rebase) {
            w.loc = loc;
            w.rot = rot;
            w.rebase = false;
            continue;
        }
        // The move is from an anchor, not from the last read: a prop slid at under 3 cm per read (a heavy one a pawn shoves)
        // would otherwise never be a push. The anchor follows a prop that no body is near and is re-set by each move read.
        const bool moved = Dist2(loc, w.loc) > kMoveCm * kMoveCm || TurnDeg(rot, w.rot) > kTurnDeg;
        const bool fell = moved && Fell(w.loc, loc);
        if (moved || BodyCm2(body, loc) > kTouchCm * kTouchCm) {
            w.loc = loc;
            w.rot = rot;
        }
        if (!moved) continue;
        if (isHost) {
            // Not the host's own body's push (a fall, or a move with the host's body away): a settle the host streams.
            if (fell || BodyCm2(body, loc) > kTouchCm * kTouchCm) {
                Settle(actor, loc, body, fell);
                continue;
            }
            // The host's own body: a prop a row already streams (a let-go's, a verb's, a client's) has its source, and a
            // client's claim for it is judged by prop_syncer_host::OnClaim, which takes a row that only coasts.
            if (coop::prop_drive_host::IsDriven(actor)) { NoteSkip(Skip::Driven, actor); continue; }
            if (!Pushable(actor, loc)) { NoteSkip(Skip::NotPushable, actor); continue; }
            if (PushOff()) continue;
            if (coop::prop_drive_host::CoastCapReached(actor, "body push")) { NoteSkip(Skip::Capped, actor); continue; }
            if (!coop::prop_drive_host::Coast(actor, "body push", coop::prop_drive_host::CoastCause::BodyPush)) {
                NoteSkip(Skip::NotCoasted, actor);
                continue;
            }
            ++g_pushes;
            if (g_pushes == 1 || (g_pushes % 50) == 0)
                UE_LOGI("body_push: a body pushed '%ls' %p -- the host coasts it (#%u)", R::ClassNameOf(actor).c_str(), actor,
                        g_pushes);
        } else {
            // Not this client's own body's push (a fall, or a move with its body away): its copy moved alone, unless a
            // stream parks it. If the host's copy fell too, the host's settle coast parks it; if not, the host's rest is
            // asked for once it rests.
            if (fell || BodyCm2(body, loc) > kTouchCm * kTouchCm) {
                if (!coop::prop_drive_stream::IsParked(actor) && Pushable(actor, loc))
                    coop::prop_rest_sync::NoteLocalMove(actor);
                continue;
            }
            // This client's own player walked into it: the player who bumps an unsynced prop syncs it (MTA's push, the
            // server's CUnoccupiedVehicleSync.cpp:476-509); the host judges the claim.
            if (BodyCm2(body, loc) > kTouchCm * kTouchCm) continue;
            if (!Pushable(actor, loc)) { NoteSkip(Skip::NotPushable, actor); continue; }
            if (coop::prop_drive_stream::IsParked(actor)) { NoteSkip(Skip::Parked, actor); continue; }
            if (ClaimOff()) continue;
            if (!coop::prop_syncer::Claim(actor, coop::net::PropSyncReason::BodyPush)) {
                NoteSkip(Skip::ClaimNotSent, actor);   // held off, open or refused
                coop::prop_rest_sync::NoteLocalMove(actor);   // the copy moved alone: the host's rest is asked for
                continue;
            }
            ++g_pushes;
            if (g_pushes == 1 || (g_pushes % 50) == 0)
                UE_LOGI("body_push: this player pushed '%ls' %p -- this peer syncs it (#%u)", R::ClassNameOf(actor).c_str(),
                        actor, g_pushes);
        }
    }
}

void Rebase(void* actor) {
    for (Watched& w : g_list)
        if (w.ref.Raw() == actor) {
            w.rebase = true;
            return;
        }
}

void OnDisconnect() {
    if (g_pushes || g_settles) UE_LOGI("body_push: session end -- %u pushes taken, %u settles streamed", g_pushes, g_settles);
    g_pushes = 0;
    g_settles = 0;
    g_cut = 0;
    for (uint32_t& n : g_skips) n = 0;
    g_list.clear();
    g_classes.clear();
    g_classVersion = ~0ull;
    g_nextList = g_nextRead = Clock::time_point{};
}

}  // namespace coop::props::body_push
