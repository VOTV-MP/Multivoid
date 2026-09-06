// coop/items/coingun_arbiter.cpp -- the host's half of a coin-gun sale: range-check, resolve,
// authorise, guard against replay, price from our own copy, mint through the game's own verb,
// and destroy what was paid for. The client's half (the gun verb bracket, the coin capture, the
// barrier, the outbound sale and the result the seller reads) is coingun_sync.cpp; the pickup
// is coingun_collect.cpp. Everything here runs on the host, from the trust boundary, on the
// game thread.

#include "coop/items/coingun_sync.h"

#include "coingun_internal.h"   // co-located private header (src tree, not include/)

#include "coop/element/intent_authority.h"   // may this sender name it
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"   // the sender's puppet body, its reach
#include "coop/props/prop_echo_suppress.h"   // MarkArbiterConsumedKey
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

// Resolved by InstallArbiter, driven from the sale lane's own 1 Hz-throttled Install.
void* g_libCdo       = nullptr;   // Default__lib_C -- sellObject's context
void* g_sellObjectFn = nullptr;
void* g_sellFn       = nullptr;   // prop_coingun_C::sell
int32_t g_offPropMesh = -1;       // Aprop_C::StaticMesh, resolved by name
ue_wrap::CachedObjRef g_gunRef;   // a live gun instance, world-stamped

std::atomic<unsigned long long> g_salesRefused{0};

// The artifact's name: the save key when the prop has one, the eid otherwise. Both forms carry
// a prefix minted here: a client could send a key that collides with the keyless form for some
// eid and poison the consumption guard for an unrelated prop, so the generator, not the input,
// keeps the namespaces disjoint.
std::wstring ArtifactName(const std::wstring& key, uint32_t eid) {
    if (!key.empty()) return L"k:" + key;
    return L"#" + std::to_wstring(static_cast<unsigned long>(eid));
}

// The consumption guard. The artifact must be consumed, or one CoinGunSell could be replayed to
// mint for the same prop again and again; nothing else stops a repeat. Keyed by the artifact
// name, not the eid (a client's own save-loaded keyed prop has eid 0, so an eid key would
// collapse every such sale onto one entry). Valued by a world-stamped CachedObjRef, so the
// guard compares identity and an entry reads dead once the prop or its world is gone; erased
// by the host's sweep and cleared whole on disconnect.
std::unordered_map<std::wstring, ue_wrap::CachedObjRef> g_sold;

// The gun's reach: its trace is arm(1000), which starts at the player camera manager's location
// (eye height, not the actor root) and runs along the camera forward, so the sender's own body
// is the right thing to measure against; the camera offset is one of the things the pose
// staleness budget absorbs.
constexpr float kGunReachUU = 1000.0f;



void* FindLiveGun() {
    // `sell` reads no gun state and positions the coins from the sold prop's component, so any
    // live instance behaves identically. Cached and walked only on a miss: the sale rate is
    // attacker-controlled, and FindObjectByClass is a full walk.
    if (void* cached = g_gunRef.Get()) return cached;
    // The miss is throttled too: a world with no gun (the ordinary world; the gun is placed in
    // three maps) would otherwise re-walk on every packet. 250 ms collapses a burst to one walk and
    // stays far below the time a person needs to buy a gun and fire it.
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

// The host's answer. Every sale gets one, success or refusal: a silent refusal renders on the
// seller's screen as their prop vanishing (its own destroy still lands) with no coins.
void SendResult(coop::net::Session* s, uint8_t slot, coop::net::CoinGunResultCode code,
                int32_t points) {
    if (!s) return;
    coop::net::CoinGunResultPayload r{};
    r.code   = static_cast<uint8_t>(code);
    r.points = points;
    s->SendReliableToSlot(static_cast<int>(slot), coop::net::ReliableKind::CoinGunResult, &r,
                          sizeof(r));
}

// Refuse, answer and count in one place, so a new refusal path cannot skip the sentence.
// Returns whether to log: every path here is reachable from the trust boundary at any rate, so
// the first five are logged, then every tenth to a hundred, then every hundredth, per code, so
// a flood of one reason cannot hide the first of another.
bool Refuse(coop::net::Session* s, uint8_t slot, coop::net::CoinGunResultCode code) {
    g_salesRefused.fetch_add(1, std::memory_order_relaxed);
    SendResult(s, slot, code, 0);
    // Per sender as well as per code: a global counter would let one peer's flood silence another's
    // first legitimate refusal.
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

    // The artifact's name off the wire. WireKey's contract says the bytes beyond `len` are zero,
    // and a hostile sender is not bound by it: clamp the length and stop at the first NUL.
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

    // The eid range. Either band is accepted, as the PropDestroy receiver does: a sale references
    // an existing shared entity rather than allocating one, and the keyless families can carry an
    // eid allocated by the peer that first expressed them. An id in neither band never reaches the
    // resolver.
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

    // Resolve the artifact against our own world, key first: a client mints no Element row for its
    // own save-loaded keyed prop, so the eid it sends is 0 for exactly the props a player shoots,
    // and the key is the identity that survives; the eid stays as the keyless fallback. Index only,
    // no walk: a key not in the maintained index is not a prop this peer can have shot, since the
    // client knows the key only because it mirrored the prop from us, which put it in the index. A
    // cold walk on an attacker-chosen key would be a guaranteed miss at whatever rate the sender
    // likes.
    void* prop = nullptr;
    const wchar_t* how = L"key";
    if (!keyStr.empty()) prop = coop::prop_element_tracker::FindLiveActorByKey(keyStr);
    if (!prop && p.elementId != 0u) {
        // The keyless fallback, for a prop whose Key reads None: an O(1) registry lookup. Reachable
        // through the gun: its gate is an interface check, not class descent, and the keyless pile
        // families implement that interface; they die one statement later at the asProp cast.
        prop = coop::element::LivePropActor(static_cast<coop::element::ElementId>(p.elementId));
        how  = L"eid";
    }
    if (!prop) {
        const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::NoSuchProp);
        if (logIt) UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' (key='%ls' eid=%u) -- "
                "REASON=no-such-prop. NEITHER name resolves to a live prop in our world. The usual "
                "cause is on this side: a key index holding a few dozen props against a world of "
                "thousands sends every sale of an ordinary save-loaded prop here. That root "
                "is fixed (registry_reaper publishes it now); reaching this line again means a "
                "genuinely unknown key. The client's own destroy still lands, so it costs the "
                "ITEM -- the lane's header names that invariant and does not yet hold it.",
                senderSlot, artifact.c_str(), keyStr.empty() ? L"None" : keyStr.c_str(),
                p.elementId);
        return;
    }

    // The authorisation gate, the first question about the actor, before every remaining question
    // about the artifact, so a peer probing keys learns nothing it did not have. The shared
    // IntentTarget owns the question: the same producer-derived reach, the measured-bounds
    // expansion, the staleness budget, and a fail-closed answer when the sender has no live puppet.
    // Authorize rather than Resolve, since identity was answered by key above.
    {
        const auto tok = coop::element::IntentTarget::ForClientIntent(*s, senderSlot, kGunReachUU);
        const coop::element::IntentSubject sub = tok.Authorize(prop);
        if (!sub) {
            const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::TooFarAway);
            if (logIt) UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=too-far-away "
                    "(verdict=%s dist=%.0f allowed=%.0f; 'no-body' with -1 for both means the sender "
                    "has no live puppet here, so there is no body to measure a reach from and we "
                    "refuse rather than assume one). The gun traces 1000 uu from the "
                    "sender's own camera, so a prop outside that reach was not shot -- naming it is "
                    "enumeration, not a sale.",
                    senderSlot, artifact.c_str(), coop::element::OutcomeName(sub.outcome),
                    sub.distUU, sub.reachUU);
            return;
        }
    }

    // The consumption guard: was this exact artifact already minted for?
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
                "the RTT). We mint nothing rather than inventing an instance: a CDO executor "
                "would mint ZERO coins anyway, since EX_Self is the WorldContextObject of every "
                "deferred spawn inside `sell`.", senderSlot, artifact.c_str());
        return;
    }

    // Price it from our own copy.
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
        // Near-unreachable by construction, and fail-closed anyway: the refusal gate is a price
        // check over cooked table data, byte-identical on both peers, so a client whose own sell
        // refused never destroyed the prop and never authored a sale. Reaching here means the
        // peers' tables or prop names disagree. (Refusal by price is routine in single-player:
        // every loose-garbage row prices at 1 and is refused.)
        const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::NotSellable);
        if (logIt) UE_LOGW("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=not-sellable (sellObject said "
                "sold=0 for name='%ls'). Our prop is left UNMUTATED; the client's destroy still "
                "lands, exactly as today. If this fires, the peers disagree about this prop's name "
                "or about list_props itself.", senderSlot, artifact.c_str(), propNameStr.c_str());
        return;
    }

    // Mint through the game's own verb. `comp` is the sold prop's static mesh: sell casts it and
    // derives the spawn location from it.
    if (g_offPropMesh < 0) g_offPropMesh = R::FindPropertyOffset(R::ClassOf(prop), L"StaticMesh");
    void* meshComp = (g_offPropMesh >= 0)
        ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(prop) + g_offPropMesh)
        : nullptr;
    if (!g_sellFn) g_sellFn = R::FindFunction(R::ClassOf(gun), L"sell");   // cached
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
    sf.Set<int32_t>(L"Index", points);           // `Index` is the price
    sf.Set<void*>(L"comp", meshComp);
    const bool ok = ue_wrap::Call(gun, sf);
    if (!ok) {
        // The dispatch failed, so nothing was minted: the artifact is not consumed, and the seller
        // is not told it sold.
        const bool logIt = Refuse(s, senderSlot, coop::net::CoinGunResultCode::HostInternal);
        if (logIt) UE_LOGE("coingun[host]: REFUSED slot=%u artifact='%ls' -- REASON=sell-dispatch-failed "
                "(gun=%p comp=%p price=%d). No coins were minted and the artifact is NOT consumed.",
                senderSlot, artifact.c_str(), gun, meshComp, points);
        return;
    }
    g_sold[artifact].Set(prop);      // consumed
    SendResult(s, senderSlot, coop::net::CoinGunResultCode::Sold, points);

    // The arbiter consumes what it paid for. The sold prop's destroy lives in the gun's ubergraph
    // right after the sell call, not inside sell, so sell alone mints coins and leaves the prop
    // standing; delegated to the client's own PropDestroy it was a cost paid on the counterparty's
    // honour, and an attacker simply omitted it. It runs here in the native order on the host's own
    // copy, and needs no broadcast of its own: DestroyActor dispatches K2_DestroyActor, the
    // UFunction the destroy seam patches, so the ordinary PropDestroy goes to every peer, and the
    // seller's own PropDestroy then lands as an echo no-op. What the guard covers now is not a
    // same-frame replay (both resolvers reject a PendingKill actor before the guard is consulted)
    // but the failed-destroy case: minted, and still resolvable. The key is marked before the
    // destroy: our destroy evicts it from the index, and the seller's PropDestroy arriving behind
    // us would otherwise pay a full cold walk to rediscover a prop that is gone.
    if (!keyStr.empty()) coop::prop_echo_suppress::MarkArbiterConsumedKey(keyStr);
    E::DestroyActor(prop);

    // The coins the mint spawns are caught by the world-actor enumerator's Func thunk and enrolled
    // by world_actor_sync, which allocates each eid and broadcasts the spawn; nothing here enrolls
    // anything.
    UE_LOGI("coingun[host]: SOLD slot=%u artifact='%ls' (resolved by %ls) name='%ls' price=%d -> "
            "minted via gun=%p comp=%p, and WE destroyed the prop ourselves (v140 A50: the arbiter "
            "performs the whole transaction; the destroy rides the ordinary K2_DestroyActor seam out "
            "to every peer). Coins are HOST-OWNED; whoever's body trips one credits the host.",
            senderSlot, artifact.c_str(), how, propNameStr.c_str(), points, gun, meshComp);
}

namespace internal {

void InstallArbiter() {
    // Called from the sale lane's Install inside its 1 Hz throttle; every resolve is a linear walk
    // with a name render per entry.
    if (!g_libCdo)        g_libCdo        = R::FindClassDefaultObject(L"lib_C");
    if (!g_sellObjectFn && g_libCdo)
        g_sellObjectFn = R::FindFunction(R::ClassOf(g_libCdo), L"sellObject");
}

bool ArbiterResolved() { return g_sellObjectFn != nullptr; }

void OnDisconnectArbiter() {
    UE_LOGI("coingun[arbiter]: SESSION SUMMARY -- salesRefused=%llu soldSet=%zu",
            g_salesRefused.load(std::memory_order_relaxed), g_sold.size());
    // Every world-scoped thing goes; the resolved class, function and CDO pointers are not
    // world-scoped and stay.
    g_sold.clear();
    g_gunRef = ue_wrap::CachedObjRef{};
}

void SweepSoldSet() {
    // Erase consumed artifacts whose prop has died, the guard's lifetime: a CachedObjRef reads dead
    // as null, the world moving on included. Normally none or one entry, since the arbiter destroys
    // the prop itself.
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
