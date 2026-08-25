// coop/items/coingun_arbiter.cpp -- THE HOST'S HALF OF THE SALE: it decides, prices, mints, and
// consumes.
//
// EXTRACTED from coingun_sync.cpp 2026-08-25, when the A50/A51/items-6-12 work pushed that file to
// 925 LOC (soft cap 800). The cut is by SUBSYSTEM, not by size, and it is the same cut the collect
// lane got a day earlier -- see coingun_internal.h for the shape and why it stays one concept:
//
//   coingun_sync.cpp     -- the CLIENT's half of the sale: the gun verb bracket, the coin capture,
//                           the commit-or-release barrier, the outbound CoinGunSell, and the inbound
//                           CoinGunResult sentence the seller reads.
//   coingun_arbiter.cpp  -- THIS FILE. The host's receiver: range-check, resolve, AUTHORIZE, guard
//                           against replay, price from our own copy, mint through the game's own
//                           verb, and destroy what we paid for.
//   coingun_collect.cpp  -- somebody picks a coin up; the host performs the credit.
//
// WHY THIS IS THE RIGHT LINE. Everything here runs ONLY on the host and ONLY from the trust
// boundary; nothing in coingun_sync.cpp does either. That is also why the security work concentrated
// here: A50 (any peer could name any prop, and the host never destroyed what it paid for) and A51
// (an attacker-rate GUObjectArray walk) were both defects of this receiver, and both were AUTHORED
// by the fix that first made the sold prop nameable. See coop/items/coingun_sync.h for the design,
// docs/security/TRACKER.md's 2026-08-25 block for the rows.
//
// Game thread throughout (the wire receiver runs on the pump).

#include "coop/items/coingun_sync.h"

#include "coingun_internal.h"   // co-located private header (src tree, not include/)

#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"   // A50: the sender's puppet body, to measure its reach
#include "coop/props/prop_echo_suppress.h"   // I-2: mark the key we consume as the arbiter
#include "coop/props/prop_element_tracker.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

namespace coop::coingun_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace I = coop::coingun_sync::internal;

// Resolved by InstallArbiter(), driven from the sale lane's own ~1 Hz-throttled Install.
void* g_libCdo       = nullptr;   // Default__lib_C -- sellObject's context
void* g_sellObjectFn = nullptr;
void* g_sellFn       = nullptr;   // prop_coingun_C::sell -- cached (M-5: was re-resolved per sale)
int32_t g_offPropMesh = -1;       // Aprop_C::StaticMesh (@0x0238 in the dump; resolved by NAME)
ue_wrap::CachedObjRef g_gunRef;   // a live gun instance, world-stamped (C-3: was a walk per sale)

std::atomic<unsigned long long> g_salesRefused{0};

// The artifact's NAME: the save key when the prop has one, the eid otherwise.
//
// BOTH FORMS ARE PREFIXED BY US (item 11, v140). The previous version returned the key VERBATIM and
// justified it with "a save key never starts with '#'" -- which is a fact about SAVE KEYS being
// applied to an ATTACKER-SUPPLIED STRING. A client is free to send key="#5" and collide its artifact
// name with the keyless form for eid 5, which is a consumption-guard collision: one sale poisons the
// guard for an unrelated prop, or launders a replay past it. The generator, not the input, is what
// has to make the namespaces disjoint, so the key form now carries its own prefix and no wire string
// can reach the eid namespace at all.
std::wstring ArtifactName(const std::wstring& key, uint32_t eid) {
    if (!key.empty()) return L"k:" + key;
    return L"#" + std::to_wstring(static_cast<unsigned long>(eid));
}

// THE CONSUMPTION GUARD (C-3, audit 2026-08-24; RE-KEYED AND GIVEN A REAL LIFETIME in v138/B1).
// The artifact must be CONSUMED, or a client can replay one CoinGunSell and mint for the same prop
// forever -- the host removes nothing itself (by design: the client's own PropDestroy does that),
// so nothing else stops a repeat. `order_sync`, this lane's precedent, confirms its commit with an
// OrderCount()+1 edge before charging; this is the equivalent.
//
// TWO v137 DEFECTS FIXED HERE, both measured, both in this comment's predecessor:
//   1. IT WAS KEYED ON THE EID. Once B1 names the artifact by KEY, an eid key collapses onto
//      g_soldEid[0] for exactly the props this lane exists for (a v122 client's own save-loaded
//      keyed prop has eid 0), so the FIRST client sale would poison the guard against EVERY later
//      one -- a free denial of the whole feature. The key is now the artifact NAME: the save key
//      when there is one, "#<eid>" for the keyless families, which is unique in each domain.
//   2. IT CLAIMED TO SELF-CLEAN AND WAS ERASED NOWHERE. The old comment said the map "self-cleans:
//      once that prop dies and the eid is recycled onto a different actor...". `[V]` It was written
//      at :393 and read at :317 and never erased at all, and the recycle it leaned on cannot happen
//      in a real session anyway -- registry.cpp:177 pushes a freed id to the FRONT while Alloc pops
//      the BACK, deferring reuse by ~28k allocations. So the map only ever grew, and a reconnecting
//      client carried a stale sold-set into a new world. It is erased in TWO places now: the ~1 Hz
//      host sweep in Tick() drops entries whose prop has died (a CachedObjRef reads dead as null,
//      including "the world moved on" -- CLAUDE.md 4j), and OnDisconnect clears it whole.
// The value is a world-stamped CachedObjRef, not a raw pointer: the guard compares IDENTITY, and a
// raw pointer to a freed actor can be matched by a recycled allocation.
std::unordered_map<std::wstring, ue_wrap::CachedObjRef> g_sold;

// `[V]` prop_coingun's ubergraph statement [4], read this session off research/bp_reflection:
//     K2Node_Event_player->CALLVIRT arm(1000.0, out arm_start, out arm_end, out arm_rotation)
//     LineTraceSingleForObjects(self, arm_start, arm_end, ...)
// The reach is 1000 uu and the trace originates on the PLAYER, not on the gun -- which is exactly
// why the sender's own puppet is the right thing to measure against.
//
// PRECISION CORRECTION (2026-08-25). The line above used to read "AT THE PLAYER", and `mainPlayer.arm`
// says otherwise: `[V]` `start := GetPlayerCameraManager().K2_GetActorLocation()` and
// `end := start + GetActorForwardVector(<that same camera manager>) * (customLength > 0 ?
// customLength : armLength)`. There is no property called `CameraForwardVector` -- an earlier draft
// of this very paragraph invented one while correcting a different overstatement, which is the same
// failure one size down. The trace
// begins at the PLAYER CAMERA, which follows the player at eye height, not at the actor root that
// `GetActorLocation` reports. The conclusion is unchanged -- the camera is rigidly the sender's own
// body -- but the offset is real and is one of the things `kPoseStalenessUU` below absorbs. This lane
// has already shipped four generations of a comment that overstated what was measured (v140 audit,
// I-3/4/5/6); a comment that is approximately true is the shape that becomes false later.
constexpr float kGunReachUU = 1000.0f;

// The pose-staleness budget. The host's copy of a client's body is behind reality by the one-way
// latency + RemotePlayer::kInterpWindowMs (75 ms) + one send interval, and the native trace starts
// at the player's ARM, which sits above the actor root that GetActorLocation reports. 600 uu covers
// roughly half a second at a sprint plus the eye-height offset. Deliberately generous: this gate
// exists to stop WORLD-WIDE enumeration, not to adjudicate centimetres, and a false refusal costs a
// real player a real sale. Widening it does not re-open A50 -- the arbiter's own destroy does that
// work; this bound only decides how far a peer may reach to spend the world's props.
constexpr float kPoseStalenessUU = 600.0f;

// A50 (2026-08-25). Is the named prop within the SENDER'S OWN reach, measured entirely on the HOST's
// copies of both bodies? Before this existed, `senderSlot` occurred in this file only as a reply
// address and a printf argument: the receiver asked five questions about the ARTIFACT and none about
// the ACTOR, so any peer could name any prop in the world.
//
// MTA does exactly this for a write it did not witness --
// `reference/mtasa-blue/Server/mods/deathmatch/logic/CUnoccupiedVehicleSync.cpp:491`, inside
// `Packet_UnoccupiedVehiclePushSync`: `IsPointNearPoint3D(pVehicle->GetPosition(),
// pPlayer->GetPosition(), radius)` gating an inbound packet before its state change is honoured.
// Same shape, same reason, same place in the flow. (A first draft also cited `:244`; that line is in
// `FindPlayerCloseToVehicle`, the SYNCER-ELECTION path, which validates no write at all and is not a
// packet handler -- audit I-6, 2026-08-25. Wrong citation, dropped.)
//
// FAIL-CLOSED ON THE PUPPET: no live puppet on the host means there is no body to measure a reach
// from, so the answer is no. A joining client whose puppet has not spawned yet is refused and TOLD
// (TooFarAway), which is the principle-8 answer for this lane: the sale is the client's to retry, its
// own prop is gone either way, and inventing a reach for a body we cannot see is the enumeration hole.
//
// *** WHAT THIS DOES NOT DO, AND THE HEADER USED TO CLAIM IT DID (audit CRITICAL C-1, 2026-08-25) ***
// The anchor is the SENDER'S OWN POSITION, and the sender writes it. `[V]` the host applies an
// inbound pose after `ValidatePose` only, which is a static garbage filter -- finite, |xyz| <= 1e6 cm
// (TEN KILOMETRES), a SELF-REPORTED speed bound, angle ranges -- with no delta-vs-time check
// anywhere; and the one place distance is examined, `remote_player.cpp`'s snap limit, ACCEPTS the
// teleport and scales its own threshold off that self-reported speed. So the A50 enumeration survives
// at one extra packet per prop: pose to the prop, sell the prop, repeat.
//
// This gate is therefore a RATE-AND-EFFORT mitigation, not a closure, and it is deliberately kept
// because it is the right architecture (it is MTA's) and because it costs an attacker a visible body
// moving through the world. THE CLOSURE IS A HOST-SIDE MOVEMENT VALIDATOR ON THE POSE LANE -- one
// fix, at the boundary where the unvalidated value actually enters, benefiting every present and
// future lane that asks "was this peer near it", instead of a second compensation bolted on here.
// Tracked as its own row; do not write "A50 closed" until that ships.
bool SenderMayReach(uint8_t senderSlot, void* prop, float& outDist, float& outAllowed) {
    outDist = outAllowed = -1.f;
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(senderSlot);
    void* puppet = (rp && rp->valid()) ? rp->GetActor() : nullptr;
    if (!puppet) return false;

    // CHECKED READS ONLY (audit I-1, 2026-08-25). The first version of this used
    // `E::GetActorLocation`, which returns a default FVector on failure -- and (0,0,0) is the WORLD
    // ORIGIN, an ordinary position -- so a failed read reported the body as standing at the origin
    // and authorized anything else near it. That is a fail-OPEN inside a function whose own comment
    // said FAIL-CLOSED, which is this lane's own recurring defect one more time.
    ue_wrap::FVector body{};
    if (!E::TryGetActorLocation(puppet, body)) return false;

    // The prop's ORIGIN is not its surface, and the native trace stops at whatever it HITS -- so a
    // large prop is legitimately sellable from further away than reach-from-origin. Measure the real
    // bounds instead of inventing a constant fudge for "big things".
    //
    // `GetActorBounds` returning true means THE DISPATCH SUCCEEDED, not that the box is meaningful:
    // `[A]` UE4's AActor::GetActorBounds starts from an empty FBox and expands it per qualifying
    // component, so an actor with no COLLIDING components (carried, physics off, inside a container)
    // yields Origin=(0,0,0) Extent=(0,0,0) -- the world origin again, by a second route. A zero
    // extent is therefore treated as "no bounds", not as "a point-sized prop at the origin".
    ue_wrap::FVector origin{}, extent{};
    float propRadius = 0.f;
    const bool haveBounds = E::GetActorBounds(prop, /*onlyColliding=*/true, origin, extent) &&
                            !(extent.X == 0.f && extent.Y == 0.f && extent.Z == 0.f);
    if (haveBounds) {
        propRadius = std::sqrt(extent.X * extent.X + extent.Y * extent.Y + extent.Z * extent.Z);
    } else if (!E::TryGetActorLocation(prop, origin)) {
        return false;   // we cannot say where it is, so we cannot say the sender could reach it
    }

    const float dx = origin.X - body.X, dy = origin.Y - body.Y, dz = origin.Z - body.Z;
    outDist    = std::sqrt(dx * dx + dy * dy + dz * dz);
    outAllowed = kGunReachUU + propRadius + kPoseStalenessUU;
    return outDist <= outAllowed;
}

void* FindLiveGun() {
    // `[V]` `sell` reads ZERO gun state and positions coins from the SOLD PROP's component, so ANY
    // live instance behaves identically -- there is nothing to prefer.
    // C-3 (audit): "once per sale" is NOT cold enough -- the sale rate is attacker-controlled, and
    // FindObjectByClass is a full GUObjectArray walk. Cache it and walk only on a miss.
    if (void* cached = g_gunRef.Get()) return cached;
    // A51 (v140): the POSITIVE result was cached and the NEGATIVE one was not, so a world with no gun
    // in it re-walked GUObjectArray on every single sale -- and `[V]` prop_coingun is placed in only
    // 3 of 261 maps, so "no gun" is the ordinary world, not the exotic one. Throttle the miss. 250 ms
    // collapses a packet burst (the reliable inbox drains unbounded per tick) to one walk while
    // staying far below the time it takes a human to buy a gun from the laptop and fire it, so a real
    // sale is never refused for want of a re-walk.
    using Clock = std::chrono::steady_clock;
    static Clock::time_point sLastMiss{};
    const Clock::time_point now = Clock::now();
    if (sLastMiss.time_since_epoch().count() != 0 &&
        now - sLastMiss < std::chrono::milliseconds(250)) {
        return nullptr;
    }
    void* found = R::FindObjectByClass(kGunClassName);
    if (found) g_gunRef.Set(found);
    else       sLastMiss = now;
    return found;
}

// ---- the host's ANSWER (v138 B1) ---------------------------------------------------------------
// Every sale gets one, success or refusal. A refusal that says nothing renders on the seller's
// screen as the exact bug the user reported: their prop vanishes (its own destroy is unchanged and
// still lands) and no coins appear. See the header's THE RESULT SENTENCE.
void SendResult(coop::net::Session* s, uint8_t slot, coop::net::CoinGunResultCode code,
                int32_t points) {
    if (!s) return;
    coop::net::CoinGunResultPayload r{};
    r.code   = static_cast<uint8_t>(code);
    r.points = points;
    s->SendReliableToSlot(static_cast<int>(slot), coop::net::ReliableKind::CoinGunResult, &r,
                          sizeof(r));
}

// Refuse + answer + count, in one place so a new refusal path cannot silently skip the sentence.
//
// RETURNS whether this refusal should be LOGGED (item 7, v140). Every refusal path here is reachable
// from the trust boundary at whatever rate a sender likes, and each one carried an unconditional
// UE_LOGW -- so one peer could fill the log faster than anything else in it could be read, which is
// the same denial the A51 walk was, one layer up. The R-1e latch shape from prop_destroy_seam: the
// first five, then every tenth to a hundred, then every hundredth. PER CODE, so a flood of
// no-such-prop cannot hide the first not-sellable.
bool Refuse(coop::net::Session* s, uint8_t slot, coop::net::CoinGunResultCode code) {
    g_salesRefused.fetch_add(1, std::memory_order_relaxed);
    SendResult(s, slot, code, 0);
    // PER SENDER as well as per code (audit MINOR, 2026-08-25): a global counter lets one peer
    // flooding NoSuchProp suppress a DIFFERENT peer's first legitimate refusal -- which turns a
    // noise-control measure into a way to silence somebody else's diagnostics. 8 codes x kMaxPeers
    // slots is 32 counters.
    static uint32_t sSeen[8][coop::players::kMaxPeers] = {};
    const unsigned ci = static_cast<unsigned>(code) < 8u ? static_cast<unsigned>(code) : 0u;
    const unsigned si = slot < coop::players::kMaxPeers ? slot : 0u;
    const uint32_t n = ++sSeen[ci][si];
    return n <= 5 || (n <= 100 && n % 10 == 0) || n % 100 == 0;
}

}  // namespace

void OnReliable(const uint8_t* payload, int len, uint8_t senderSlot) {
    auto* s = I::Session();
    if (!s || s->role() != coop::net::Role::Host) {
        UE_LOGW("coingun[host]: CoinGunSell received off the HOST -- dropping");
        return;
    }
    if (!payload || len < static_cast<int>(sizeof(coop::net::CoinGunSellPayload))) {
        UE_LOGW("coingun[host]: CoinGunSell payload too small (len=%d) -- dropping", len);
        return;
    }
    coop::net::CoinGunSellPayload p{};
    std::memcpy(&p, payload, sizeof(p));

    // --- read the artifact's NAME off the wire ----------------------------------------------------
    // WireKey's contract is that bytes beyond `len` are zero, but a hostile sender is not bound by a
    // contract: clamp the length and stop at the first NUL rather than trusting either.
    std::wstring keyStr;
    {
        const uint8_t klen = (p.key.len <= sizeof(p.key.data))
                                 ? p.key.len
                                 : static_cast<uint8_t>(sizeof(p.key.data));
        for (uint8_t i = 0; i < klen; ++i) {
            const char ch = p.key.data[i];
            if (ch == '\0') break;
            keyStr.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
        }
    }
    const std::wstring artifact = ArtifactName(keyStr, p.elementId);

    // --- eid RANGE trust (item 6, v140) -----------------------------------------------------------
    // The `IsAllowedHostAllocatedEid` idiom is obeyed at 14 other receive sites and was missing here,
    // while the header claimed this receiver "fully range-checks the payload" -- a sentence deleted
    // rather than softened when it was measured false. EITHER range is accepted for the same reason
    // the PropDestroy receiver accepts either (event_dispatch_entity.cpp): a sale REFERENCES an
    // existing shared entity rather than allocating one, and the keyless families can carry an eid
    // allocated by the peer that first expressed them. A genuinely invalid id (0 / kInvalidId / out
    // of both ranges) never reaches the resolver.
    if (p.elementId != 0u && p.elementId != coop::element::kInvalidId &&
        !coop::element::Registry::IsAllowedHostAllocatedEid(p.elementId) &&
        !coop::element::Registry::IsAllowedPeerAllocatedEid(p.elementId)) {
        const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::NoSuchProp);
        if (logIt)
            UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=eid-out-of-range "
                    "(0x%08x is in neither the host nor the peer allocation band). Nothing that names "
                    "an id we could not have issued gets as far as the resolver.",
                    senderSlot, artifact.c_str(), p.elementId);
        return;
    }

    // --- resolve the artifact against OUR OWN world -----------------------------------------------
    // KEY FIRST. This is the whole of B1: v137 resolved the eid alone, and `[V]` a v122 client mints
    // no Element row for its own save-loaded keyed prop, so the eid it sent was 0 for exactly the
    // props a player shoots -- 3 of 3 field sales died right here. The key is the identity that
    // survives, and the eid stays as the keyless fallback exactly as the PropDestroy receiver does.
    //
    // INDEX ONLY (A51, v140). This used to call `ResolveLiveActorByKey`, whose cold fallback is a
    // full `ue_wrap::prop::FindByKeyString` GUObjectArray walk with a key-string read per prop -- and
    // an attacker-chosen key is a GUARANTEED miss, so every hostile packet bought one whole walk, on
    // the host's game thread, at whatever rate the sender likes (the reliable inbox drains with an
    // unbounded `while (TryGetReliable(msg))` per tick, so they compound inside one frame). The
    // mitigation was already written 200 lines above in this same file, applied to the OTHER
    // attacker-steered walk (FindLiveGun): cache, and walk only on a miss. Here the right answer is
    // stronger -- do not walk at all. A key that is not in the maintained index is not a prop this
    // peer can legitimately have shot: the client only KNOWS the key because it mirrored the prop
    // from us, which required us to hold a Prop Element for it, which is precisely what puts it in
    // the index. "Not indexed" is therefore already the NoSuchProp answer this site gives.
    void* prop = nullptr;
    const wchar_t* how = L"key";
    if (!keyStr.empty()) prop = coop::prop_element_tracker::FindLiveActorByKey(keyStr);
    if (!prop && p.elementId != 0u) {
        // The keyless fallback, for an Aprop_C whose Key genuinely reads None. It costs nothing: the
        // 4 bytes sit in alignment padding the payload needs regardless, and LivePropActor is an O(1)
        // Registry lookup, not a walk.
        //
        // (This comment used to assert the fallback is unreachable through the gun "because the
        // keyless families are not Aprop_C descendants", citing ue_wrap/actors/prop.cpp's union
        // predicate. That reasoning was wrong twice over and is corrected here rather than left
        // standing: `[V]` the gun's gate is NOT class descent -- prop_coingun's ubergraph @792 is
        // `BooleanAND(DoesImplementInterface(HitActor, int_player_C), NOT IsChildActor)` and
        // trashBitsPile_C / actorChipPile_C DO implement that interface and DO pass it; they die one
        // statement later at `IsValid(asProp_return)`, because `prop_C::asProp = EX_Self` is the only
        // non-null implementation among all 34 implementors. And our own union predicate can only
        // ever be evidence about our code, never about the game's class tree.)
        prop = coop::element::LivePropActor(static_cast<coop::element::ElementId>(p.elementId));
        how  = L"eid";
    }
    if (!prop) {
        const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::NoSuchProp);
        if (logIt) UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' (key='%ls' eid=%u) -- "
                "REASON=no-such-prop. NEITHER name resolves to a live prop in our world, so the two "
                "peers disagreed about this prop BEFORE anyone fired: a pre-existing stable-ID "
                "divergence this lane exposes, not one it causes. The client's own destroy still "
                "lands, so this degrades to pre-A37 behaviour (item lost, nothing credited) -- it "
                "does not manufacture a NEW loss, and the seller is TOLD.",
                senderSlot, artifact.c_str(), keyStr.empty() ? L"None" : keyStr.c_str(),
                p.elementId);
        return;
    }

    // --- THE AUTHORIZATION GATE (A50, v140) -------------------------------------------------------
    // FIRST question about the ACTOR, and it comes before every remaining question about the
    // artifact so that a peer probing keys learns nothing it did not already have. Everything above
    // this line is identity resolution; everything below it spends the world's props.
    {
        float dist = -1.f, allowed = -1.f;
        if (!SenderMayReach(senderSlot, prop, dist, allowed)) {
            const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::TooFarAway);
            if (logIt) UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=too-far-away "
                    "(dist=%.0f allowed=%.0f; -1 for both means the sender has no live puppet here, "
                    "so there is no body to measure a reach from and we refuse rather than assume "
                    "one). The gun `[V]` traces arm(1000.0) from the sender's own camera, so a prop outside that "
                    "reach was not shot -- naming it is enumeration, not a sale.",
                    senderSlot, artifact.c_str(), dist, allowed);
            return;
        }
    }

    // THE CONSUMPTION GUARD -- has this exact artifact already been minted for? (see g_sold)
    {
        auto it = g_sold.find(artifact);
        if (it != g_sold.end() && it->second.Get() == prop) {
            const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::AlreadySold);
            if (logIt) UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=already-sold. This prop "
                    "was already minted for and its destroy has not yet removed it. A replayed or "
                    "duplicated sale mints nothing.", senderSlot, artifact.c_str());
            return;
        }
    }

    void* gun = FindLiveGun();
    if (!gun) {
        const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::NoGun);
        if (logIt) UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=no-live-coingun. Nothing in "
                "our world can execute `sell` (the sender's hand mirror may have been stowed within "
                "the RTT). We mint nothing rather than inventing an instance -- `[V]` a CDO executor "
                "would mint ZERO coins anyway, since EX_Self is the WorldContextObject of every "
                "deferred spawn inside `sell`.", senderSlot, artifact.c_str());
        return;
    }

    // --- price it from OUR OWN copy ---------------------------------------------------------------
    if (!g_sellObjectFn || !g_libCdo) {
        const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::HostInternal);
        if (logIt) UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=sellObject-unresolved",
                senderSlot, artifact.c_str());
        return;
    }
    const std::wstring propNameStr = ue_wrap::prop::GetPropNameString(prop);
    const R::FName propName = ue_wrap::fname_utils::StringToFName(propNameStr);
    ue_wrap::ParamFrame f(g_sellObjectFn);
    if (!f.valid()) {
        const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::HostInternal);
        if (logIt) UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=sellObject-frame-invalid",
                senderSlot, artifact.c_str());
        return;
    }
    f.Set<R::FName>(L"object", propName);
    f.Set<bool>(L"onlyShop", true);              // the GUN passes TRUE -- match it exactly
    f.Set<void*>(L"objectToSell", prop);
    f.Set<void*>(L"__WorldContext", prop);
    if (!ue_wrap::Call(g_libCdo, f)) {
        const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::HostInternal);
        if (logIt) UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=sellObject-dispatch-failed",
                senderSlot, artifact.c_str());
        return;
    }
    const int32_t points = f.Get<int32_t>(L"Points");
    const bool    sold   = f.Get<bool>(L"sold");
    if (!sold) {
        // NEAR-UNREACHABLE BY CONSTRUCTION, and kept fail-closed anyway. `[V]` the refusal gate is
        // `row.price <= 1 && onlyShop` over `list_props` -- pure cooked TABLE data, byte-identical on
        // both peers -- so a client whose own `sell` refused never destroyed the prop, never entered
        // our destroy seam and never authored a sale at all. Reaching here means the two peers'
        // tables or prop NAMES disagree, which is a version/identity fault worth seeing, not a
        // routine outcome. (Refusal by price is very much routine in SP: measured over the shipped
        // list_props, every loose-garbage row -- ntrash*, g3_bag_*, garbBin2, garbContainer,
        // trashClump -- prices at 1 and is refused, so shooting garbage does nothing on ANY peer.)
        const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::NotSellable);
        if (logIt) UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=not-sellable (sellObject said "
                "sold=0 for name='%ls'). Our prop is left UNMUTATED; the client's destroy still "
                "lands, exactly as today. If this fires, the peers disagree about this prop's name "
                "or about list_props itself.", senderSlot, artifact.c_str(), propNameStr.c_str());
        return;
    }

    // --- mint through the game's own verb ---------------------------------------------------------
    // `comp` is the SOLD PROP's static mesh -- `[V]` `sell` does DynamicCast<UStaticMeshComponent>(comp)
    // and `rnd(c, comp)` derives the spawn location from it. Aprop_C::StaticMesh (prop.hpp:9).
    if (g_offPropMesh < 0) g_offPropMesh = R::FindPropertyOffset(R::ClassOf(prop), L"StaticMesh");
    void* meshComp = (g_offPropMesh >= 0)
        ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(prop) + g_offPropMesh)
        : nullptr;
    if (!g_sellFn) g_sellFn = R::FindFunction(R::ClassOf(gun), L"sell");   // M-5: cache, not per sale
    void* sellFn = g_sellFn;
    if (!sellFn) {
        const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::HostInternal);
        if (logIt) UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=sell-unresolved", senderSlot,
                artifact.c_str());
        return;
    }
    ue_wrap::ParamFrame sf(sellFn);
    if (!sf.valid()) {
        const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::HostInternal);
        if (logIt) UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=sell-frame-invalid",
                senderSlot, artifact.c_str());
        return;
    }
    sf.Set<int32_t>(L"Index", points);           // `[V]` `Index` IS the price
    sf.Set<void*>(L"comp", meshComp);
    const bool ok = ue_wrap::Call(gun, sf);
    if (!ok) {
        // The dispatch itself failed, so nothing was minted -- do NOT consume the artifact, and do
        // NOT tell the seller it sold. v137 stamped the sold-set unconditionally right here.
        const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::HostInternal);
        if (logIt) UE_LOGE("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=sell-dispatch-failed "
                "(gun=%p comp=%p price=%d). No coins were minted and the artifact is NOT consumed.",
                senderSlot, artifact.c_str(), gun, meshComp, points);
        return;
    }
    g_sold[artifact].Set(prop);      // consume the artifact (see THE CONSUMPTION GUARD)
    SendResult(s, senderSlot, coop::net::CoinGunResultCode::Sold, points);

    // --- THE ARBITER CONSUMES WHAT IT PAID FOR (A50, v140) ----------------------------------------
    // `[V]` the sold prop's destroy lives in the GUN's ubergraph at [26] (`HitActor->K2_DestroyActor`,
    // right after [24] `self->sell(...)`), NOT inside `sell` -- so calling `sell` alone mints coins
    // and leaves the prop standing. Until v140 this half of the transaction was delegated to the
    // client's own PropDestroy arriving behind us on the same lane, and the success log below said so
    // out loud: a cost paid on the counterparty's honour is not a transaction. An attacker simply
    // omitted the destroy.
    //
    // We run it HERE, in the native order (sell, then destroy), on the host's own copy. Nothing new
    // has to broadcast it: E::DestroyActor dispatches Actor.K2_DestroyActor, which is the exact
    // UFunction prop_destroy_seam Func-patches, so DestroySeamBody sees this destroy and sends the
    // ordinary PropDestroy to every peer -- one owner, one mechanism. (SendSaleForDyingProp inside
    // that seam is a no-op for us: it returns immediately off a Client role, and we are the host.)
    // The seller's own PropDestroy then arrives at a host that no longer resolves the prop and lands
    // as the steady-state no-op OnDestroyImpl_ already implements for an echo.
    //
    // WHAT THE CONSUMPTION GUARD ACTUALLY COVERS NOW, corrected (audit MINOR, 2026-08-25). It is NOT
    // what stops a same-frame replay: both resolvers reject a PendingKill actor (IsLiveByIndex in the
    // key index; LiveActorOfType in the collect lane), so a second packet naming this prop resolves
    // nothing and refuses with NoSuchProp before the guard is consulted. The guard compares through
    // CachedObjRef::Get(), which is itself liveness-validated, so once this destroy lands it is
    // structurally silent. Its real remaining job is the FAILED-destroy case -- we minted, the destroy
    // did not take, the prop is still resolvable -- which is exactly the case nothing else covers.
    //
    // I-2 (audit 2026-08-25): mark the key BEFORE destroying it. Our destroy evicts the key from the
    // maintained index, so the seller's own PropDestroy arriving behind us would miss the index and
    // pay a full FindByKeyString GUObjectArray walk to rediscover that the prop is gone -- converting
    // what used to be an O(1) index hit into a guaranteed cold scan on every arbiter-performed sale,
    // which is the very cost A51 had just removed from this lane. The mark is the fact only we know.
    if (!keyStr.empty()) coop::prop_echo_suppress::MarkArbiterConsumedKey(keyStr);
    E::DestroyActor(prop);

    // The coins the mint spawns are EX_CallMath BeginDeferreds from the gun's bytecode, so
    // npc_world_enum's source-gated Func thunk catches them and drains to
    // world_actor_sync::HostEnrollExSpawn -- that is what allocates each eid and broadcasts
    // WorldActorSpawn. Nothing here has to enroll anything.
    UE_LOGI("coingun[host]: SOLD slot=%u artifact='%ls' (resolved by %ls) name='%ls' price=%d -> "
            "minted via gun=%p comp=%p, and WE destroyed the prop ourselves (v140 A50: the arbiter "
            "performs the whole transaction; the destroy rides the ordinary K2_DestroyActor seam out "
            "to every peer). Coins are HOST-OWNED; whoever's body trips one credits the host.",
            senderSlot, artifact.c_str(), how, propNameStr.c_str(), points, gun, meshComp);
}

namespace internal {

void InstallArbiter() {
    // Called from the sale lane's Install, INSIDE its ~1 Hz resolve throttle -- every resolve here is
    // a linear GUObjectArray walk with a name render per entry, and the caller already pays for the
    // throttle (C-4, audit 2026-08-24).
    if (!g_libCdo)        g_libCdo        = R::FindClassDefaultObject(L"lib_C");
    if (!g_sellObjectFn && g_libCdo)
        g_sellObjectFn = R::FindFunction(R::ClassOf(g_libCdo), L"sellObject");
}

bool ArbiterResolved() { return g_sellObjectFn != nullptr; }

void OnDisconnectArbiter() {
    UE_LOGI("coingun[arbiter]: SESSION SUMMARY -- salesRefused=%llu soldSet=%zu",
            g_salesRefused.load(std::memory_order_relaxed), g_sold.size());
    // Every WORLD-scoped thing goes; the resolved UClass / UFunction / CDO pointers are NOT
    // world-scoped and stay (CLAUDE.md 4j). v137 had no OnDisconnect at all, so a reconnecting peer
    // carried a stale sold-set into a new world.
    g_sold.clear();
    g_gunRef = ue_wrap::CachedObjRef{};
}

void SweepSoldSet() {
    // Erase consumed artifacts whose prop has died. This is what gives the consumption guard a real
    // lifetime -- v137's comment CLAIMED the map self-cleaned while the map was erased nowhere at
    // all, so it only ever grew. A CachedObjRef reads dead as null, including "the world moved on"
    // (CLAUDE.md 4j), so this is exactly the liveness test the guard itself uses. The map holds only
    // props that were sold and have not died yet, which -- now that the arbiter destroys them itself
    // (A50) -- is normally zero or one entry.
    if (g_sold.empty()) return;
    size_t dropped = 0;
    for (auto it = g_sold.begin(); it != g_sold.end();) {
        if (it->second.Get() == nullptr) { it = g_sold.erase(it); ++dropped; }
        else                             { ++it; }
    }
    if (dropped)
        UE_LOGI("coingun[sold-set]: swept %zu consumed artifact(s) whose prop is gone (%zu still "
                "live)", dropped, g_sold.size());
}

}  // namespace internal
}  // namespace coop::coingun_sync
