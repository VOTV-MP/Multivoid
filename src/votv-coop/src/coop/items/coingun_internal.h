// coop/items/coingun_internal.h -- the shared substrate of the two coin-gun lanes.
//
// SRC-TREE PRIVATE (not under include/): only coingun_sync.cpp and coingun_collect.cpp include it.
// The established pattern in this tree -- npc_sync_internal.h, puppet_internal.h,
// coop/props/remote_prop/internal.h -- for an extraction that splits one concept's file without
// splitting the concept itself (the folder-per-domain-concept rule: `coop/items/` still owns "the
// coin gun and its coins" whole; this is the modular FILE-SIZE rule's remedy, not a re-homing).
//
// THE CUT. Two distinct subsystems shared one file until it passed the 800-LOC soft cap:
//   - THE SALE (coingun_sync.cpp)     -- a client shoots a prop; the host prices and mints.
//     Owns: CoinGunSell / CoinGunResult, the `playerHandUse_LMB` verb, the client-coin barrier,
//     the sold-set, PrepareCoinMirror.
//   - THE COLLECT (coingun_collect.cpp) -- somebody picks a coin up; the host performs the credit.
//     Owns: CoinCollect, the `actionOptionIndex` verb, the overlap interceptor, the host perform.
// They meet only here, on four reads: which session, is this actor a coin, am I inside verb X, and
// where does a coin keep its `points`. Everything else in each lane is private to its own TU.
//
// The two lanes register SEPARATE verbs with SEPARATE callbacks -- `vm_dispatch` is one callback per
// NAME, and the names differ -- so neither TU needs the other's entry point. That is what made the
// cut clean; a shared dispatcher would have forced the collect lane's guts back into this header.
//
// Game thread unless a function says otherwise.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }
namespace ue_wrap::vm_dispatch { struct ActiveVerb; }

namespace coop::coingun_sync {

// The two 0x45 verb registrations. The NAME is the handle, never the id: `vm_dispatch`'s active-verb
// window is ONE global thread-local, so an id is a caller-chosen tag in a project-wide namespace
// (`[V]` container_contents_sync, meadow_db_sync and drive_sync all publish 1) and `active` is true
// for ANY registered verb. See ue_wrap/core/vm_dispatch.h's contract box.
extern const wchar_t* const kVerbNameGunUse;    // prop_coingun_C::playerHandUse_LMB -- the shot
extern const wchar_t* const kVerbNameCollect;   // baocoin_C::actionOptionIndex     -- the E-press
inline constexpr int kVerbCoinGunUse  = 6;
inline constexpr int kVerbCoinCollect = 7;

inline constexpr const wchar_t* kGunClassName  = L"prop_coingun_C";
inline constexpr const wchar_t* kCoinClassName = L"baocoin_C";

namespace internal {

// The session both lanes were installed with, or nullptr. Lock-free; any thread.
coop::net::Session* Session();

// Is `actor` a baocoin_C? Falls back to a name compare before the class resolves.
bool IsCoinActor(void* actor);

// Is this thread inside a 0x45 dispatch of the verb called `name`? The SALE lane's ambient-window
// read (the collect lane reads `b.ctx` off its own bracket and does not call this). Pointer-compares first (both lanes pass a literal they registered themselves,
// and RegisterVirtualVerb requires static lifetime, so the pointers are identical), then wcscmp.
bool InVerb(const ue_wrap::vm_dispatch::ActiveVerb& av, const wchar_t* name);

// Abaocoin_C's UClass once resolved, else nullptr. Resolution is the SALE lane's Install (it runs
// first and already retries at ~1 Hz); the collect lane reads it and stays inert until it appears.
void* CoinClass();

// (There is no GunClass() accessor. The extraction added one on the belief that the arbiter would
//  read it; the arbiter resolves its gun with FindObjectByClass and never asks for the class, so it
//  was dead the day it was written and is deleted per RULE 2 rather than left as a false promise.)

// Byte offset of Abaocoin_C::points, or -1 if unresolved. Resolved by NAME beside CoinClass().
int32_t CoinPointsOffset();

// Is `coin` one of OUR OWN coins, captured inside our gun bracket and still held at the barrier?
//
// The collect lane needs this because such a coin is NOT a mirror and NOT map-placed -- it is the
// third member of a set the non-mirror branch's comment described as having two, so a client
// overlapping its own pre-barrier coins credited itself under a comment blaming level content. The
// answer at overlap time must be "suppress": if the shot goes on to author a sale the host mints the
// real coins and a local credit is a phantom; if it does not, the coin is RELEASED rather than
// destroyed (v140) and stays pickable, so suppressing costs a step off and back on, never the coin.
// Cheap: the held set is one shot's worth of coins for at most one pump interval, and empty
// otherwise. Any thread (takes the barrier lock).
bool IsCapturedCoin(void* coin);

// ---- the ARBITER's own lifecycle (coingun_arbiter.cpp) -----------------------------------------
// The HOST half of the sale: the CoinGunSell receiver plus everything only it uses (the sold-set,
// the reach check, the gun cache, sellObject/sell). Extracted 2026-08-25 when the A50/A51 work took
// coingun_sync.cpp to 925 LOC. Driven from the sale lane's Install/Tick/OnDisconnect rather than
// from subsystems.cpp, for the same reason the collect lane is: one public surface, one concept.

// Resolve the host half's reflection. Called INSIDE the sale lane's ~1 Hz Install throttle, because
// every resolve is a linear GUObjectArray walk with a name render per entry. Idempotent.
void InstallArbiter();

// True once sellObject resolved, i.e. the host half can actually price a sale.
bool ArbiterResolved();

// Dump the host half's session summary and drop its world-scoped state (the sold-set, the cached
// gun). The resolved UClass / UFunction / CDO pointers are not world-scoped and stay (CLAUDE.md 4j).
void OnDisconnectArbiter();

// Drop consumed artifacts whose prop has died -- what gives the consumption guard a real lifetime.
// Called from the sale lane's Tick at ~1 Hz. Cheap when idle; the set is normally empty.
void SweepSoldSet();

// ---- the COLLECT lane's own lifecycle (coingun_collect.cpp) ------------------------------------
// Driven from the SALE lane's Install/OnDisconnect rather than from subsystems.cpp: the two lanes
// are one public surface (`coop/items/coingun_sync.h`) and one concept, and the collect lane's
// resolve depends on the sale lane's class resolution having run. Both are idempotent + retrying.
void InstallCollect();
void OnDisconnectCollect();

// True once BOTH collect entries are live. The sale lane's Install reads it so its own success latch
// cannot stop driving this lane's retry -- the two have DIFFERENT dependencies (the sale lane needs
// FinishSpawningActor + sellObject; this one needs the coin's BndEvt), so either can resolve first
// and neither may latch the other out.
bool CollectInstalled();

}  // namespace internal
}  // namespace coop::coingun_sync
