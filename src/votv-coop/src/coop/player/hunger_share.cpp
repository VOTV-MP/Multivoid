// coop/player/hunger_share.cpp -- see coop/player/hunger_share.h.

#include "coop/player/hunger_share.h"

#include "coop/config/config.h"
#include "coop/net/net_clock.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"

#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace coop::hunger_share {
namespace {

namespace V = ue_wrap::vitals;
namespace sg = ue_wrap::script_gate;

constexpr int kTagBody = 0x48474250;    // 'HGBP'
constexpr int kTagRanout = 0x48475241;  // 'HGRA'
// A body is born with the game's foodDraining of 1, whatever this module wrote on the one before.
UE_FACT_FN(kRowBodyBegin, "hunger_share", L"mainPlayer_C", L"ReceiveBeginPlay", Declared);
// An effect's end: every effect leaves the gamemode's list here, and the ariral vaccine, the one writer of foodDraining,
// stops writing it (its last tick leaves the value at about 1).
UE_FACT_FN(kRowRanout, "hunger_share", L"effect_C", L"removeRanout", Declared);

constexpr uint64_t kRetryMs = 1000;
constexpr float kBase = 1.0f;       // mainPlayer_C::foodDraining as the class leaves it
constexpr float kSame = 0.0005f;

enum class Put { Applied, Driven, NoPlayer };

float g_written = kBase;  // what this module last put on the local player
int   g_saidPlayers = -1;  // the last line said: its player count and the rule
bool  g_saidOn = false;
bool  g_pending = false;  // a write the player was not up for, asked again at kRetryMs
uint64_t g_nextMs = 0;
bool  g_subscribed = false;
bool  g_bodyWatched = false;
bool  g_ranoutWatched = false;
bool  g_bodySettled = false;    // each watch live or dead for good, its outcome said once
bool  g_ranoutSettled = false;
bool  g_armed = false;          // both settled
ue_wrap::CachedObjRef g_begun;  // the body a begin announced and the write has not yet landed on

bool Near(float a, float b) { return std::fabs(a - b) < kSame; }

// Put `want` on the local player when the value there is the game's or this module's. Driven while an effect
// holds it (the vaccine overwrites it every tick, and its end is announced); NoPlayer while there is no player body.
Put PutRate(float want) {
    float cur = 0.f;
    if (!V::Read(V::Field::FoodDrain, &cur)) return Put::NoPlayer;
    if (!Near(cur, kBase) && !Near(cur, g_written)) return Put::Driven;
    if (!Near(cur, want) && !V::Write(V::Field::FoodDrain, want)) return Put::NoPlayer;
    g_written = want;
    return Put::Applied;
}

// A new body is done once the pawn the write lands on IS that body: the gamemode's `mainPlayer` may still be the
// old live one when the begin is announced, and a write there would leave the new body at the game's 1.
void SettleBegun() {
    if (!g_begun.Raw()) return;
    if (!g_begun.Alive()) { g_begun.Reset(); return; }
    if (V::LocalPawn() == g_begun.Raw()) g_begun.Reset();
    else g_pending = true;
}

bool RuleOn() { return coop::config::ResolveFlag(::coop::config_registry::rows::net_hunger_shared); }

// The rate this player's hunger runs at now, written on the player.
void Apply() {
    const bool on = RuleOn();
    const int players = std::max(coop::roster_ledger::OccupiedCount(), 1);
    const float rate = on ? RateFor(players) : 1.0f;
    const Put res = PutRate(kBase * rate);
    g_pending = res == Put::NoPlayer;
    SettleBegun();
    // One line per change of what is applied, for a drill to read on each peer.
    if (res != Put::Applied || (players == g_saidPlayers && on == g_saidOn)) return;
    g_saidPlayers = players;
    g_saidOn = on;
    UE_LOGI("hunger_share: players=%d rate=%.0f%% (row %s)", players, static_cast<double>(rate) * 100.0,
            on ? "on" : "off");
}

void OnSlotReplaced(int, const coop::roster_ledger::Row&, const coop::roster_ledger::Row&) { Apply(); }
void OnRuleChanged() { Apply(); }

void OnBodyBegun(const sg::Call& call) {
    // The new body is not the gamemode's `mainPlayer` yet at its own begin: the write waits until it is. A
    // puppet is a mainPlayer_C too, registered before its begin, and is not this player's body.
    if (!call.object || coop::players::Registry::Get().IsPuppet(call.object)) return;
    g_begun.Set(call.object);
    g_pending = true;
    g_nextMs = 0;
}
void OnEffectEnded(const sg::Call&) { Apply(); }

// A watch registered once, then asked about until it is live or settled dead.
bool Settled(const ue_wrap::facts::Function& row, int tag, const char* what) {
    if (sg::NameWatchLive(row, tag)) return true;
    if (!sg::NameWatchSettled(row, tag)) return false;
    UE_LOGE("hunger_share: the %s watch is dead -- the shared rate is not re-applied after it", what);
    return true;
}

void Arm() {
    sg::ResolvePendingNames();
    if (!g_bodySettled) g_bodySettled = Settled(kRowBodyBegin, kTagBody, "new-body");
    if (!g_ranoutSettled) g_ranoutSettled = Settled(kRowRanout, kTagRanout, "effect-end");
    if (g_bodySettled && g_ranoutSettled) {
        g_armed = true;
        UE_LOGI("hunger_share: watching a new player body and the end of an effect (script-gate name watches)");
    }
}

}  // namespace

float RateFor(int players) {
    return players <= 1 ? 1.0f : std::pow(static_cast<float>(players), -0.44f);
}

void Install() {
    if (!g_subscribed) {
        g_subscribed = true;
        coop::roster_ledger::SubscribeSlotReplaced(&OnSlotReplaced);
        coop::config::Subscribe(::coop::config_registry::rows::net_hunger_shared, &OnRuleChanged);
        g_pending = true;  // the rows and the rule may be set already: the first tick writes
    }
    if (!g_bodyWatched) g_bodyWatched = sg::WatchName(kRowBodyBegin, kTagBody, nullptr, &OnBodyBegun);
    if (!g_ranoutWatched) g_ranoutWatched = sg::WatchName(kRowRanout, kTagRanout, nullptr, &OnEffectEnded);
}

void Tick() {
    if (!g_armed && g_bodyWatched && g_ranoutWatched) Arm();
    if (!g_pending) return;
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextMs) return;
    g_nextMs = now + kRetryMs;
    Apply();
}

}  // namespace coop::hunger_share
