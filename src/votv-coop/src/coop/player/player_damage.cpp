// coop/player/player_damage.cpp -- see coop/player/player_damage.h.
//
// The refusals on a peer's puppet. A hit lands on the puppet in the host's world and must not write the
// host's per-machine health; its owner takes it on its own player. For a creature of a parked family the
// host also delivers the hit, as a PlayerHit that the owner's machine applies (coop/player/player_hit.h).

#include "coop/player/player_damage.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/creature_hit.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/interactables/interactable_sync.h"  // the cremator lane's key
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/cremator_hit.h"
#include "coop/player/player_hit.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/cremator.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace coop::player_damage {
namespace {

namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace sg = ue_wrap::script_gate;

std::atomic<coop::net::Session*> g_session{nullptr};  // read in the impact interceptor (any thread)

// The impact-entry cancel. mainPlayer_C's three native->BP impact events, one shared PRE callback
// registered once for each. The class is loaded at the main menu and lives for the process
// (coop/dev/class_lifetime_probe measures it), so the registrations hold across a world change.
UE_FACT_FN(kRowImpactDamage, "player_damage", L"mainPlayer_C", L"impactDamage", Declared);
UE_FACT_FN(kRowImpactDamageCpp, "player_damage", L"mainPlayer_C", L"impactDamageCPP", Declared);
UE_FACT_FN(kRowImpactSquishCpp, "player_damage", L"mainPlayer_C", L"impactSquishCPP", Declared);
UE_FACT_FN(kRowAddPlayerDamage, "player_damage", nullptr, L"Add Player Damage", AnyClass);
UE_FACT_FN(kRowIgnite, "player_damage", L"mainPlayer_C", L"ignite", Declared);
UE_FACT_FN(kRowAddDamage, "player_damage", L"mainPlayer_C", L"addDamage", Declared);
UE_FACT_FN(kRowPunch, "player_damage", L"mainPlayer_C", L"punch", Declared);
UE_FACT_FN(kRowRagdollMode, "player_damage", L"mainPlayer_C", L"ragdollMode", Declared);
UE_FACT_FN(kRowAchievement, "player_damage", L"lib_C", L"progressAchievement", Declared);
UE_FACT_FN(kRowPlaySound2D, "player_damage", L"GameplayStatics", L"PlaySound2D", Declared);
const ue_wrap::facts::Function* const kImpactEntryRows[3] = {
    &kRowImpactDamage, &kRowImpactDamageCpp, &kRowImpactSquishCpp,
};
void*    g_impactFns[3] = {};
bool     g_impactInterceptorsDone = false;
uint32_t g_canceled = 0;   // rate-latched log counter

// The LOCAL possessed player, published by Tick (game thread) for the interceptor's any-thread
// compare. The interceptor may not reach the registry: Registry::Local() mutates game-thread-only
// cache state, and a ProcessEvent interceptor can fire on a parallel-anim worker (game_thread.h's
// own contract). A published atomic keeps the CANCEL effective on every dispatching thread with one
// acquire load and no registry access.
std::atomic<void*> g_localPawn{nullptr};

// PRE cancel: a physical impact dispatched on a mainPlayer BODY that is NOT the local possessed
// player (a peer's puppet, or the F1 skin-preview mannequin) must not run the BP body -- it writes
// the PER-MACHINE saveSlot.health singleton, so lethal contact resolved against a puppet in the
// host's world drained the HOST, and killing a player took the server down with them. The victim's
// own machine computes the same contact on its own possessed player (self==local -> pass-through),
// so nothing is lost: MTA's victim-authoritative shape, no relay. Coop-session gated, because solo
// play has only the local body anyway and an unset registry must never eat solo damage.
bool OnImpactEntryPre(void* self, void* /*params*/) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return false;
    if (!self) return false;
    void* localPawn = g_localPawn.load(std::memory_order_acquire);
    if (!localPawn) return false;           // pre-resolve boot window: native behavior
    if (self == localPawn) return false;    // own body: native path
    const uint32_t n = ++g_canceled;
    if (n <= 5 || (n % 50) == 0) {
        UE_LOGI("player_damage: canceled impact entry on NON-LOCAL body %p (#%u) -- "
                "the hit body's owner computes its own damage", self, n);
    }
    return true;  // cancel: the singleton saveSlot write must not run for a foreign body
}

// The same rule for the damage verb itself. Fire, burning and the other status effects do not come
// through an impact entry: the body's own ubergraph calls Add Player Damage on itself once a second
// for as long as it burns. A peer's puppet that walked through fire only this machine shows caught
// it here and burned for the rest of the session, and every tick ran the damage body against this
// machine's player: its camera shook once a second (the burn loop's own once-a-second Delay; the
// [DMG] probe, coop/dev/damage_probe, logs each call). The puppet's owner sees no such fire and takes nothing,
// which is the victim-authoritative answer the impact cancel already gives. Watched at the
// script-body gate because the verb is Blueprint-internal on every route. The mod's own calls get no
// pass: the origin flag spans every Blueprint call nested inside one of ours, so it proves nothing
// about which body a nested damage or ignite was aimed at. A puppet is refused whoever asks.
constexpr int kDamageVerbTag = 0x504C4447;  // 'PLDG'
bool g_damageWatchDone = false;
std::atomic<uint32_t> g_verbCanceled{0};

// One refusal of any of the four verbs on a puppet, counted in RefusedDamage and logged on a rate.
void NoteRefused(const char* verb, void* puppet) {
    const uint32_t n = ++g_verbCanceled;
    if (n <= 5 || (n % 100) == 0)
        UE_LOGI("player_damage: canceled %s on a peer's puppet %p (#%u) -- its owner computes its own damage", verb,
                puppet, n);
}

void DeliverWalkerDamage(const sg::Call& c);

sg::Verdict OnDamageVerbPre(const sg::Call& c) {
    // IsPuppet alone: no session or local-pawn early-out in front of it, since the local pawn reads
    // null for a whole death or travel window, and one call let through then burns for the session.
    if (!c.object || !coop::players::Registry::Get().IsPuppet(c.object)) return sg::Verdict::Run;
    NoteRefused("the damage verb", c.object);
    DeliverWalkerDamage(c);
    return sg::Verdict::Cancel;
}

// The body never catches fire either. mainPlayer_C burns on its own: ignite(fuel) sets burningTime and
// starts a once-a-second loop that runs the damage verb, plays a 2D pain sound and turns
// GetPlayerController(0) -- this machine's camera -- by a random ten degrees; burningTime only runs out
// on the burning body's own tick, which a puppet does not run, so a puppet set alight burned for the
// rest of the session and shook this machine's view every second. The damage cancel above stops the
// health write and nothing else, so ignite is refused on a peer's puppet: the owner computes its own
// fire as it computes its own damage. It is the one way in. attemptIgnite, the verb a spreading fire
// calls, is an empty stub on the player (its ubergraph entry is a bare pop), and startBurning is
// private and reached from ignite alone (mainPlayer_C's control-flow graph and its offset
// listing). Both refusals ask IsPuppet rather than "not the local pawn", the
// impact cancel's test, because a stale local pawn across a respawn must never make this machine's
// own player immune.
constexpr int kIgniteTag = 0x49474E54;  // 'IGNT'
bool g_igniteWatchDone = false;
std::atomic<uint32_t> g_igniteCanceled{0};

// HOST: a cancelled addDamage or ignite whose caller is a cremator is the machine's hit on a peer, delivered to that
// peer's machine as a CrematorHit (defined with the hit verbs below). True when the caller is a cremator.
bool DeliverFromCremator(const sg::Call& c, uint8_t verb);

sg::Verdict OnIgnitePre(const sg::Call& c) {
    // IsPuppet alone: no session or local-pawn early-out in front of it, since the local pawn reads
    // null for a whole death or travel window, and one call let through then burns for the session.
    if (!c.object || !coop::players::Registry::Get().IsPuppet(c.object))
        return sg::Verdict::Run;
    DeliverFromCremator(c, coop::net::kCrematorHitVerbIgnite);
    const uint32_t n = ++g_igniteCanceled;
    if (n <= 5 || (n % 100) == 0)
        UE_LOGI("player_damage: refused setting a peer's puppet %p alight (#%u) -- its fire is its owner's",
                c.object, n);
    return sg::Verdict::Cancel;
}

// The hit verbs. A creature's hit on a player is addDamage (damage, then a punch above 25) or punch (a
// knock-down, then the damage); ragdollMode is the knock-down itself. Each is refused on a puppet like the
// damage verb above, whatever the caller, a call the puppet makes on itself included: MTA locks a remote copy's
// health and the damage handler restores it (reference/mtasa-blue/Client/mods/deathmatch/logic/CClientGame.cpp:4509).
// Beside the refusal:
//   - HOST, a cancelled addDamage or punch whose caller is a creature of a parked family is delivered to the
//     puppet's peer, rebuilt from the call's parameters. A parked family's mirror makes no hit of its own, so
//     this is the only one; any other caller, and a call routed through ProcessEvent (no caller object), is
//     only refused. A cremator's addDamage and ignite are delivered the same way, as a CrematorHit
//     (DeliverFromCremator). Source's delivery with MTA's ownership: Source's NPC runs and hits on the server
//     (reference/source-sdk-2013/src/game/server/hl2/npc_BaseZombie.cpp:1267), MTA's player owns its health
//     on its own client (reference/mtasa-blue/Client/mods/deathmatch/logic/CNetAPI.cpp:1189).
//   - CLIENT, a call on this machine's own player whose caller is a parked creature's mirror, outside the
//     delivery's own apply, is counted and said once per class: the brain-off leaked. A tripwire; it runs.
constexpr int kAddDamageTag = 0x50444144;  // 'PDAD'
constexpr int kPunchTag     = 0x50445055;  // 'PDPU'
constexpr int kRagdollTag   = 0x50445247;  // 'PDRG'
constexpr int kAchievementTag = 0x50444143;  // 'PDAC'
bool g_hitWatchesDone = false;
std::atomic<uint32_t> g_delivered{0};
std::atomic<uint32_t> g_mirrorHits{0};

constexpr size_t kSeenMax = 16;
struct SeenKey {
    void*   cls  = nullptr;
    uint8_t verb = 0;
};
SeenKey g_sentSeen[kSeenMax];     // (creature class, verb) pairs whose first delivery was said
size_t  g_sentSeenCount = 0;
void*   g_mirrorSeen[kSeenMax];   // classes whose mirror hit was said
size_t  g_mirrorSeenCount = 0;

// First time for this key? Records it; a full list says nothing more (the counters still count).
bool FirstSent(void* cls, uint8_t verb) {
    for (size_t i = 0; i < g_sentSeenCount; ++i)
        if (g_sentSeen[i].cls == cls && g_sentSeen[i].verb == verb) return false;
    if (g_sentSeenCount >= kSeenMax) return false;
    g_sentSeen[g_sentSeenCount++] = SeenKey{cls, verb};
    return true;
}

bool FirstMirror(void* cls) {
    for (size_t i = 0; i < g_mirrorSeenCount; ++i)
        if (g_mirrorSeen[i] == cls) return false;
    if (g_mirrorSeenCount >= kSeenMax) return false;
    g_mirrorSeen[g_mirrorSeenCount++] = cls;
    return true;
}

// A parameter out of the call's frame, by its reflected offset; zero when the function has no such parameter.
float ReadFloat(const sg::Call& c, R::ParamOffset& off) {
    const int32_t o = off.Of(c.function);
    float v = 0.f;
    if (o >= 0) std::memcpy(&v, c.locals + o, sizeof(v));
    return v;
}

void ReadVec(const sg::Call& c, R::ParamOffset& off, float (&out)[3]) {
    const int32_t o = off.Of(c.function);
    if (o >= 0) std::memcpy(out, c.locals + o, sizeof(out));
}

// The payload of an addDamage call on a puppet. The target is stamped by player_hit::Deliver.
coop::net::PlayerHitPayload BuildAddDamage(const sg::Call& c) {
    static R::ParamOffset sActor{L"actor"}, sDamage{L"damage"}, sImpact{L"impact"};
    coop::net::PlayerHitPayload p{};
    p.verb = coop::net::kPlayerHitVerbAddDamage;
    p.damage = ReadFloat(c, sDamage);
    ReadVec(c, sImpact, p.v0);
    const int32_t o = sActor.Of(c.function);
    void* actor = nullptr;
    if (o >= 0) std::memcpy(&actor, c.locals + o, sizeof(actor));
    // A null actor travels as 0. An actor with no element travels as the invalid id, which no mirror resolves, so
    // the victim drops the hit rather than run the other branch of the Blueprint.
    if (actor) p.sourceEid = coop::element::Registry::Get().EidForActor(actor);
    return p;
}

// The payload of a punch call on a puppet.
coop::net::PlayerHitPayload BuildPunch(const sg::Call& c) {
    static R::ParamOffset sFullBody{L"fullBody"}, sBoneImp{L"boneImp"}, sBone{L"bn1"}, sDamage{L"damage"},
        sDamageLoc{L"damageLocation"};
    coop::net::PlayerHitPayload p{};
    p.verb = coop::net::kPlayerHitVerbPunch;
    p.damage = ReadFloat(c, sDamage);
    ReadVec(c, sFullBody, p.v0);
    ReadVec(c, sBoneImp, p.v1);
    ReadVec(c, sDamageLoc, p.v2);
    const int32_t o = sBone.Of(c.function);
    if (o >= 0) {
        R::FName bone{};
        std::memcpy(&bone, c.locals + o, sizeof(bone));
        p.bone = coop::player_hit::BoneIndex(R::ToString(bone));
    }
    return p;
}

// A cremator whose empty lane key was said: the machine hits again each second, so one line per cremator.
void* g_unkeyedSeen[kSeenMax];
size_t g_unkeyedSeenCount = 0;

bool FirstUnkeyed(void* cremator) {
    for (size_t i = 0; i < g_unkeyedSeenCount; ++i)
        if (g_unkeyedSeen[i] == cremator) return false;
    if (g_unkeyedSeenCount >= kSeenMax) return false;
    g_unkeyedSeen[g_unkeyedSeenCount++] = cremator;
    return true;
}

// HOST: the cremator's hit on a puppet. The machine's ubergraph calls addDamage and ignite on the player it burns
// (its tick, once a second per collected actor), so the caller of the cancelled call is the cremator. Its owner's
// machine applies the hit to its own player (coop/player/cremator_hit): the health is the owner's as in MTA
// (reference/mtasa-blue/Client/mods/deathmatch/logic/CNetAPI.cpp:1189, applied at CClientGame.cpp:4543), the hit
// is made where the machine runs as Source's NPC does (reference/source-sdk-2013/src/game/server/hl2/
// npc_BaseZombie.cpp:1267). The amount is the owner's, where MTA's server computes it (SET_ELEMENT_HEALTH,
// CStaticFunctionDefinitions.cpp:1771). Beside PlayerHit and not through it: that kind names its source by an Npc
// element id and resolves it only through the Npc mirrors.
// A call that carries no caller frame cannot be told from any other, so it delivers nothing; said once per verb.
bool DeliverFromCremator(const sg::Call& c, uint8_t verb) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return false;
    if (!c.callerObject) {
        static bool sSaid[2] = {};
        bool& said = sSaid[verb == coop::net::kCrematorHitVerbIgnite ? 1 : 0];
        if (!said) {
            said = true;
            UE_LOGW("player_damage: a refused %s on a puppet carried no caller frame -- not delivered (said once per verb)",
                    verb == coop::net::kCrematorHitVerbIgnite ? "ignite" : "addDamage");
        }
        return false;
    }
    if (!ue_wrap::cremator::IsCremator(c.callerObject)) return false;
    const uint8_t slot = coop::players::Registry::Get().PeerIdOfActor(c.object);
    if (slot == coop::players::kPeerIdHost || slot >= coop::players::kMaxPeers) return true;
    static const bool sOff = coop::config::ResolveFlag(coop::config_registry::rows::cremator_hit_off);
    if (sOff) {
        static bool sOffSaid = false;
        if (!sOffSaid) {
            sOffSaid = true;
            UE_LOGW("player_damage: DRILL -- a cremator's hit on a peer's puppet is NOT delivered "
                    "(dev.cremator_hit_off); the peer takes none");
        }
        return true;
    }
    const std::wstring key = coop::interactable_sync::CrematorKey(c.callerObject);
    if (key.empty()) {
        if (FirstUnkeyed(c.callerObject))
            UE_LOGW("player_damage: cremator %p has no lane key -- its hit on a peer's puppet is not delivered "
                    "(said once per cremator)", c.callerObject);
        return true;
    }
    float seconds = 0.f;
    if (verb == coop::net::kCrematorHitVerbIgnite) {
        static R::ParamOffset sFuel{L"fuel"};
        seconds = ReadFloat(c, sFuel);
    }
    if (!coop::cremator_hit::Deliver(*s, slot, key, verb, seconds)) return true;   // Deliver says and counts it
    void* const cls = R::ClassOf(c.callerObject);
    if (FirstSent(cls, verb)) {
        const std::wstring caller = c.callerFunction ? R::ToString(R::NameOf(c.callerFunction)) : L"none";
        UE_LOGI("player_damage: a %ls's hit on a puppet is delivered to its owner (verb=%s slot=%u key='%ls' caller=%ls)",
                R::ClassNameOf(c.callerObject).c_str(), verb == coop::net::kCrematorHitVerbIgnite ? "ignite" : "damage",
                static_cast<unsigned>(slot), key.c_str(), caller.c_str());
    }
    return true;
}

// A walker tests its victim's ragdoll and spares one, but a puppet's flag stays clear: what shows a peer lying down is its
// puppet's display. Game thread.
bool WalkerSparesPuppet(void* caller, uint8_t slot) {
    if (!ue_wrap::ambient_creature::IsWalker(caller)) return false;
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    return rp && rp->IsRagdollDisplayed();
}

// HOST: the cancelled hit's delivery. Game thread (a script-gate watch).
void DeliverToOwner(const sg::Call& c, bool punch) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    if (!punch && DeliverFromCremator(c, coop::net::kCrematorHitVerbDamage)) return;
    if (!coop::creature_hit::IsDelivered(c.callerObject)) return;
    const uint8_t slot = coop::players::Registry::Get().PeerIdOfActor(c.object);
    if (slot == coop::players::kPeerIdHost || slot >= coop::players::kMaxPeers) return;
    if (WalkerSparesPuppet(c.callerObject, slot)) return;
    const coop::net::PlayerHitPayload p = punch ? BuildPunch(c) : BuildAddDamage(c);
    if (!coop::player_hit::Deliver(slot, p)) return;   // not sent: not delivered (Deliver says and counts it)
    ++g_delivered;
    void* const cls = R::ClassOf(c.callerObject);
    if (FirstSent(cls, p.verb)) {
        const std::wstring caller = c.callerFunction ? R::ToString(R::NameOf(c.callerFunction)) : L"none";
        UE_LOGI("[HIT] sent verb=%s slot=%u creature=%ls caller=%ls", punch ? "punch" : "addDamage",
                static_cast<unsigned>(slot), R::ClassNameOf(c.callerObject).c_str(), caller.c_str());
    }
}

// HOST: the walker's timer_damage ends its hit with a 2D break_flesh_3, which would sound here whoever it hit. While its hit on
// a puppet runs, a pre hook on PlaySound2D refuses that walker's next call, and the victim plays it beside the hit (the hit's
// cue); a spared puppet hears none, as the game's own test spares a player lying down. Game thread.
void* g_soundFn = nullptr;
bool g_soundHooked = false, g_soundRefused = false;
void* g_hushWalker = nullptr;
uint32_t g_hushed = 0;

sg::Verdict OnSound2DPre(void* /*context*/, void* source, const uint8_t* /*parms*/) {
    if (!g_hushWalker || source != g_hushWalker) return sg::Verdict::Run;
    g_hushWalker = nullptr;
    ue_wrap::ufunction_hook::SetArmed(g_soundFn, &OnSound2DPre, false);
    if (++g_hushed <= 3) UE_LOGI("player_damage: refused the walker's hit sound here -- its victim is a peer (#%u)", g_hushed);
    return sg::Verdict::Cancel;
}

void InstallSound() {
    if (g_soundHooked || g_soundRefused) return;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowPlaySound2D);
    if (!fn) return;
    g_soundFn = fn.Fn();
    if (!ue_wrap::ufunction_hook::InstallPreHook(fn, &OnSound2DPre, /*armed=*/false)) {
        g_soundRefused = true;
        UE_LOGE("player_damage: no pre hook on PlaySound2D -- a walker's hit on a peer sounds on the host");
        return;
    }
    g_soundHooked = true;
}

void HushWalker(void* walker) {
    if (!g_soundHooked) return;
    g_hushWalker = walker;
    ue_wrap::ufunction_hook::SetArmed(g_soundFn, &OnSound2DPre, true);
}

// HOST: a walker's hit names the damage verb itself, not addDamage, with no actor to carry. The peer runs its addDamage with
// no source, as a nameless hit, and the punch that follows is delivered as any other. Game thread (a script-gate watch).
void DeliverWalkerDamage(const sg::Call& c) {
    if (!c.callerObject || !ue_wrap::ambient_creature::IsWalker(c.callerObject)) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    const uint8_t slot = coop::players::Registry::Get().PeerIdOfActor(c.object);
    if (slot == coop::players::kPeerIdHost || slot >= coop::players::kMaxPeers) return;
    HushWalker(c.callerObject);
    if (WalkerSparesPuppet(c.callerObject, slot)) return;
    static R::ParamOffset sDamage{L"damage"};
    coop::net::PlayerHitPayload p{};
    p.verb = coop::net::kPlayerHitVerbAddDamage;
    p.cue = coop::net::kPlayerHitCueWalker;
    p.damage = ReadFloat(c, sDamage);
    if (!coop::player_hit::Deliver(slot, p)) return;
    ++g_delivered;
    UE_LOGI("[HIT] sent the walker's damage %.1f to slot %u", p.damage, static_cast<unsigned>(slot));
}

// CLIENT: a hit on this machine's own player that a parked creature's mirror made. MTA's guard refuses a remote
// player's duplicate damage event (reference/mtasa-blue/Client/mods/deathmatch/logic/CClientGame.cpp:4345-4377);
// this only counts. A refusal here would hide a leaked brain-off instead of showing it, and the mark table fails open:
// a mirror left unmarked would hit here and again through the delivery, and the count is what shows that double.
void WatchMirrorHit(const sg::Call& c) {
    if (!c.callerObject || coop::player_hit::InApply()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return;
    if (!coop::players::Registry::Get().IsLocal(c.object)) return;
    if (!coop::creature_hit::IsDelivered(c.callerObject)) return;
    ++g_mirrorHits;
    if (FirstMirror(R::ClassOf(c.callerObject)))
        UE_LOGW("player_damage: a parked %ls mirror hit this player -- the brain-off leaked",
                R::ClassNameOf(c.callerObject).c_str());
}

sg::Verdict OnHitVerbPre(const sg::Call& c) {
    if (!c.object) return sg::Verdict::Run;
    const bool ragdoll = c.tag == kRagdollTag;
    const bool punch   = c.tag == kPunchTag;
    if (!coop::players::Registry::Get().IsPuppet(c.object)) {
        if (!ragdoll) WatchMirrorHit(c);
        return sg::Verdict::Run;
    }
    NoteRefused(ragdoll ? "ragdollMode" : punch ? "punch" : "addDamage", c.object);
    if (!ragdoll) DeliverToOwner(c, punch);
    return sg::Verdict::Cancel;
}

// The cremator's burn loop earns its machine's player the cremator achievement when the actor it burns is under 10
// health, read from the machine's own save slot, before it calls addDamage on that actor. When the actor is a peer's
// puppet that call is cancelled above and the hit is delivered to its owner, who earns the achievement on its own
// profile; the host's own health says nothing about the burn. The call is refused when its caller is the cremator's
// own ubergraph and the loop's current actor, read from the caller's frame, is a puppet. A frame variable that did
// not resolve refuses nothing: the host then earns the achievement for a peer's burn when its own health is under 10.
bool g_achievementWatchDone = false;
std::atomic<uint32_t> g_achievementRefused{0};

sg::Verdict OnAchievementPre(const sg::Call& c) {
    if (!c.callerObject || !ue_wrap::cremator::IsCremator(c.callerObject)) return sg::Verdict::Run;
    void* actor = nullptr;
    if (!ue_wrap::cremator::LoopActor(c.callerFunction, c.callerLocals, actor)) return sg::Verdict::Run;
    if (!coop::players::Registry::Get().IsPuppet(actor)) return sg::Verdict::Run;
    const uint32_t n = ++g_achievementRefused;
    if (n <= 3 || (n % 100) == 0)
        UE_LOGI("player_damage: refused the cremator achievement for the burn of a peer's puppet %p (#%u) -- it is "
                "the owner's", actor, n);
    return sg::Verdict::Cancel;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

uint32_t RefusedDamage() { return g_verbCanceled.load(std::memory_order_relaxed); }
uint32_t RefusedIgnites() { return g_igniteCanceled.load(std::memory_order_relaxed); }
uint32_t RefusedAchievements() { return g_achievementRefused.load(std::memory_order_relaxed); }
uint32_t DeliveredCount() { return g_delivered.load(std::memory_order_relaxed); }
uint32_t MirrorHitCount() { return g_mirrorHits.load(std::memory_order_relaxed); }
uint32_t HushedSounds() { return g_hushed; }

void Tick() {
    // Publish the local pawn for the interceptor's any-thread compare (GT here;
    // Registry::Local() is GT-only and cached).
    g_localPawn.store(coop::players::Registry::Get().Local(), std::memory_order_release);
    InstallSound();
    // A hit whose sound did not follow in its own call: the hush is for that call alone.
    if (g_hushWalker) {
        g_hushWalker = nullptr;
        ue_wrap::ufunction_hook::SetArmed(g_soundFn, &OnSound2DPre, false);
    }
    // The dmghazard probe's red: no refusal is watched, so a puppet takes damage and fire here, and
    // nothing is delivered.
    static const bool s_noRefusal =
        coop::config::ResolveFlag(coop::config_registry::rows::player_damage_no_refusal);
    if (s_noRefusal && (!g_damageWatchDone || !g_igniteWatchDone || !g_hitWatchesDone || !g_achievementWatchDone)) {
        g_damageWatchDone = g_igniteWatchDone = g_hitWatchesDone = g_achievementWatchDone = true;
        UE_LOGW("player_damage: DRILL -- the damage, hit-verb and ignite refusals on a peer's puppet are OFF "
                "(dev.player_damage_no_refusal); a puppet takes them here and no hit is delivered");
    }
    if (!g_damageWatchDone) {
        g_damageWatchDone = true;
        if (!sg::WatchName(kRowAddPlayerDamage, kDamageVerbTag, &OnDamageVerbPre, nullptr))
            UE_LOGE("player_damage: the damage verb watch did not register -- a burning puppet damages "
                    "this machine's player");
    }
    if (!g_igniteWatchDone) {
        g_igniteWatchDone = true;
        if (!sg::WatchName(kRowIgnite, kIgniteTag, &OnIgnitePre, nullptr))
            UE_LOGE("player_damage: the ignite watch did not register -- a peer's puppet can burn here");
    }
    if (!g_hitWatchesDone) {
        g_hitWatchesDone = true;
        if (!sg::WatchName(kRowAddDamage, kAddDamageTag, &OnHitVerbPre, nullptr))
            UE_LOGE("player_damage: the addDamage watch did not register -- a creature's hit runs on a puppet here");
        if (!sg::WatchName(kRowPunch, kPunchTag, &OnHitVerbPre, nullptr))
            UE_LOGE("player_damage: the punch watch did not register -- a creature's punch runs on a puppet here");
        if (!sg::WatchName(kRowRagdollMode, kRagdollTag, &OnHitVerbPre, nullptr))
            UE_LOGE("player_damage: the ragdollMode watch did not register -- a puppet can ragdoll here");
    }
    if (!g_achievementWatchDone) {
        g_achievementWatchDone = true;
        if (!sg::WatchName(kRowAchievement, kAchievementTag, &OnAchievementPre, nullptr))
            UE_LOGE("player_damage: the achievement watch did not register -- the host earns the cremator achievement "
                    "for a peer's burn when its own health is under 10");
    }
    if (g_impactInterceptorsDone) return;
    void* cls = ue_wrap::object_index::ClassByName(L"mainPlayer_C");
    if (!cls) return;
    // A Blueprint class loads with its functions, so the first attempt with the class present is the
    // last: a name that does not resolve is a recooked Blueprint, and a full table is a capacity fault
    // to fix, not a slot to wait for.
    g_impactInterceptorsDone = true;
    bool all = true;
    for (int i = 0; i < 3; ++i) {
        const ue_wrap::facts::Resolved impact = ue_wrap::facts::ResolveOn(*kImpactEntryRows[i], cls);
        g_impactFns[i] = impact.Fn();
        if (impact && GT::RegisterInterceptor(impact, &OnImpactEntryPre)) continue;
        all = false;
        UE_LOGE("player_damage: impact entry %ls %s -- a non-local body runs its damage body", kImpactEntryRows[i]->Name(),
                g_impactFns[i] ? "did not register (the interceptor table is full)"
                               : "is not declared on mainPlayer_C (a recooked Blueprint)");
    }
    if (all) {
        UE_LOGI("player_damage: impact-entry PRE cancels installed (impactDamage=%p "
                "impactDamageCPP=%p impactSquishCPP=%p) -- non-local bodies never run "
                "the saveSlot damage body",
                g_impactFns[0], g_impactFns[1], g_impactFns[2]);
    }
}

bool DebugForceHitPuppet(int ownerSlot, float damage) {
    if (ownerSlot <= 0 || ownerSlot >= coop::players::kMaxPeers) {
        UE_LOGW("player_damage: DebugForceHitPuppet invalid ownerSlot=%d", ownerSlot);
        return false;
    }
    auto& reg = coop::players::Registry::Get();
    if (!reg.Puppet(static_cast<uint8_t>(ownerSlot))) {
        UE_LOGW("player_damage: DebugForceHitPuppet slot=%d has no puppet (peer not connected) "
                "-- cannot simulate a hit", ownerSlot);
        return false;
    }
    coop::net::PlayerHitPayload p{};
    p.verb = coop::net::kPlayerHitVerbAddDamage;
    p.damage = damage;
    return coop::player_hit::Deliver(static_cast<uint8_t>(ownerSlot), p);
}

}  // namespace coop::player_damage
