// coop/player/cremator_hit.cpp -- see coop/player/cremator_hit.h.

#include "coop/player/cremator_hit.h"

#include "coop/interactables/interactable_sync.h"  // the cremator lane's resolve
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine_player_verbs.h"
#include "ue_wrap/world/profile.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace coop::cremator_hit {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace EN = ue_wrap::engine;

// The cremator Blueprint's own numbers, as its ignite loop uses them: the ignite's fuel is a random float in
// [20, 30]; a hit on a player under 10 health is the lethal one (9999 damage, the cremator achievement), any other
// is 10 damage.
constexpr float kIgniteMinS     = 20.f;
constexpr float kIgniteMaxS     = 30.f;
constexpr float kLethalBelow    = 10.f;
constexpr float kLethalDamage   = 9999.f;
constexpr float kOrdinaryDamage = 10.f;

// Game thread: the receive and apply side. The counts are said once, by OnDisconnect.
uint32_t g_applied = 0;
uint32_t g_dropped = 0;
uint32_t g_unsourced = 0;  // damage applied with no source: the cremator's key did not resolve here
uint32_t g_failedApplies = 0;
uint32_t g_sent = 0;
bool g_noPlayerSaid = false;
bool g_healthSaid = false;
bool g_igniteSaid = false;  // the first applied ignite has been said
bool g_damageSaid = false;  // the first applied ordinary damage has been said (a lethal hit is always said)

// The keys whose drop was said, by their hash: the machine hits again each second, so a cremator that does not
// resolve here is one line, not one a second.
constexpr size_t kSeenMax = 8;
uint64_t g_unresolvedSeen[kSeenMax] = {};
size_t   g_unresolvedCount = 0;

// First time for this key? Records it; a full list says nothing more (the counter still counts).
bool FirstUnresolved(uint64_t hash) {
    for (size_t i = 0; i < g_unresolvedCount; ++i)
        if (g_unresolvedSeen[i] == hash) return false;
    if (g_unresolvedCount >= kSeenMax) return false;
    g_unresolvedSeen[g_unresolvedCount++] = hash;
    return true;
}

// The send side. Why Deliver sent nothing: counted every time, said on the first of each reason.
enum class RefusedWhy : uint8_t { BadSlot, BadKey, SessionRefused, Count };
uint32_t g_refusedSends = 0;
bool g_refusedSaid[static_cast<size_t>(RefusedWhy::Count)] = {};

// Always false, so a refusing branch of Deliver reads `return Refused(...)`.
bool Refused(RefusedWhy why, uint8_t slot) {
    const uint32_t n = ++g_refusedSends;
    bool& said = g_refusedSaid[static_cast<size_t>(why)];
    if (said) return false;
    said = true;
    switch (why) {
    case RefusedWhy::BadSlot:
        UE_LOGW("[CREMATOR-HIT] Deliver invalid slot=%u -- the hit is lost (#%u refused in all)",
                static_cast<unsigned>(slot), n);
        break;
    case RefusedWhy::BadKey:
        UE_LOGW("[CREMATOR-HIT] Deliver to slot=%u with an empty or over-long key -- the hit is lost (#%u refused in all)",
                static_cast<unsigned>(slot), n);
        break;
    default:
        // The session refused the send: the slot is not world-ready, or its backlog dropped it.
        UE_LOGW("[CREMATOR-HIT] the session refused a CrematorHit send to slot=%u -- the hit is lost (#%u refused in all)",
                static_cast<unsigned>(slot), n);
        break;
    }
    return false;
}

// The verb on this machine's own player, as the cremator's Blueprint would have run it on the player it burns. The
// host's own split read the host's save slot, so none of its numbers cross. Game thread.
void Apply(const coop::net::CrematorHitPayload& p) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* const body = coop::players::Registry::Get().Local();
    if (!body || !R::IsLive(body)) {
        const uint32_t n = ++g_dropped;
        if (!g_noPlayerSaid) {
            g_noPlayerSaid = true;
            UE_LOGW("[CREMATOR-HIT] a hit but no live local player to apply it to -- dropped (#%u dropped in all)", n);
        }
        return;
    }
    if (p.verb == coop::net::kCrematorHitVerbIgnite) {
        const float seconds = std::clamp(p.igniteSeconds, kIgniteMinS, kIgniteMaxS);
        const bool ok = EN::InvokeMainPlayerIgnite(body, seconds);
        if (ok) ++g_applied;
        else if (++g_failedApplies == 1) UE_LOGW("[CREMATOR-HIT] ignite did not run on the local player");
        if (!g_igniteSaid) {
            g_igniteSaid = true;
            UE_LOGI("[CREMATOR-HIT] applied verb=ignite seconds=%.1f ok=%d (first; the rest are counted, #%u applied)",
                    seconds, ok ? 1 : 0, g_applied);
        }
        return;
    }
    // The cremator is the damage's source and nothing else: the amount and the achievement do not read it. A copy this
    // machine's lane does not resolve costs the hit nothing; it lands with no source, as MTA applies damage whose
    // damager does not resolve (reference/mtasa-blue/Client/mods/deathmatch/logic/CClientGame.cpp:4540).
    void* const cremator = coop::interactable_sync::ResolveCremator(key);
    if (!cremator) {
        ++g_unsourced;
        if (FirstUnresolved(coop::net::FnvKey(key)))
            UE_LOGW("[CREMATOR-HIT] a hit from cremator key='%ls', which this machine's cremator lane does not resolve "
                    "-- applied with no source (said once per key)", key.c_str());
    }
    float health = 0.f;
    const bool readHealth = ue_wrap::vitals::Read(ue_wrap::vitals::Field::Health, &health);
    if (!readHealth && !g_healthSaid) {
        g_healthSaid = true;
        UE_LOGW("[CREMATOR-HIT] this player's health did not read -- the hit is taken as the ordinary one (said once)");
    }
    const bool lethal = readHealth && health < kLethalBelow;
    bool achievement = false;
    if (lethal) achievement = ue_wrap::profile::ProgressAchievement(L"cremator", /*autosave=*/true);
    const ue_wrap::FVector none{0.f, 0.f, 0.f};
    const bool ok = EN::InvokeAddDamage(body, cremator, lethal ? kLethalDamage : kOrdinaryDamage, none);
    if (ok) ++g_applied;
    else if (++g_failedApplies == 1) UE_LOGW("[CREMATOR-HIT] addDamage did not run on the local player");
    // The lethal hit is always said, with the achievement it earned here; the ordinary one the first time.
    if (lethal || !g_damageSaid) {
        if (!lethal) g_damageSaid = true;
        UE_LOGI("[CREMATOR-HIT] applied verb=damage health=%.1f lethal=%d achievement=%d ok=%d (#%u applied)", health,
                lethal ? 1 : 0, achievement ? 1 : 0, ok ? 1 : 0, g_applied);
    }
}

}  // namespace

bool Deliver(coop::net::Session& session, uint8_t slot, const std::wstring& key, uint8_t verb, float igniteSeconds) {
    if (slot == 0 || slot >= coop::net::kMaxPeers) return Refused(RefusedWhy::BadSlot, slot);
    if (key.empty() || key.size() > sizeof(coop::net::WireKey::data)) return Refused(RefusedWhy::BadKey, slot);
    coop::net::CrematorHitPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.verb = verb;
    p.igniteSeconds = verb == coop::net::kCrematorHitVerbIgnite ? igniteSeconds : 0.f;
    if (session.SendReliableToSlot(slot, coop::net::ReliableKind::CrematorHit, &p, sizeof(p), /*senderSlot=*/0)) {
        ++g_sent;
        return true;
    }
    return Refused(RefusedWhy::SessionRefused, slot);
}

void OnDisconnect() {
    if (g_sent || g_refusedSends || g_applied || g_dropped || g_failedApplies)
        UE_LOGI("[CREMATOR-HIT] session end: sent=%u refused_sends=%u applied=%u (of them no source=%u) "
                "dropped_no_player=%u failed_applies=%u",
                g_sent, g_refusedSends, g_applied, g_unsourced, g_dropped, g_failedApplies);
    g_sent = g_refusedSends = g_applied = g_unsourced = g_dropped = g_failedApplies = 0;
    g_noPlayerSaid = g_healthSaid = g_igniteSaid = g_damageSaid = false;
    g_unresolvedCount = 0;
    for (bool& said : g_refusedSaid) said = false;
}

void OnReceived(const coop::net::CrematorHitPayload& payload) {
    if (payload.verb == coop::net::kCrematorHitVerbIgnite &&
        (!std::isfinite(payload.igniteSeconds) || payload.igniteSeconds < 0.f)) {
        UE_LOGW("[CREMATOR-HIT] dropping an ignite with igniteSeconds=%.3f", payload.igniteSeconds);
        return;
    }
    const coop::net::CrematorHitPayload p = payload;
    GT::Post([p] { Apply(p); });
}

}  // namespace coop::cremator_hit
