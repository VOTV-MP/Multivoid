// coop/kerfur_convert.h -- host-authoritative kerfur NPC <-> prop conversion
// (v67, 2026-06-12). Fixes the user-reported dupe: "client sees a turned off
// kerfur-object, client turns it on, client turns it off - now host sees 2
// kerfurs lying turned off."
//
// GROUND TRUTH (kismet disassembly, research/findings/votv-kerfur-convert-RE-
// 2026-06-12.md):
//   - turn OFF: radial menu -> kerfurOmega_C::actionName(Player, Hit, "turn_off")
//     [cross-object, ProcessEvent-VISIBLE] -> ubergraph `if (kill) return;` ->
//     dropKerfurProp() [LOCAL self-call, PE-invisible]: spawns the floppy prop
//     (if hasFloppy, with data/readWrites copied), spawns the `dropProp` class
//     (Aprop_kerfurOmega_C skin variant) at the NPC's transform (or Z=20000 in
//     the flesh room), copies `sentient`, K2_DestroyActor()s the NPC. A SENTIENT
//     kerfur refuses (audio blip only -- no spawn, no destroy).
//   - turn ON: radial menu -> prop_kerfurOmega_C::actionOptionIndex(.., Action==8)
//     [PE-VISIBLE] -> spawnKerfuro() [LOCAL]: BeginDeferred+FinishSpawning the
//     `spawnKerfur` NPC class at spwn+50Z (yaw only); on success K2_DestroyActor()s
//     the prop, on failure shows a hint and the prop SURVIVES.
//
// WHY IT DUPED: every spawn/destroy inside the verbs is BP-INTERNAL
// (EX_CallMath BeginDeferredActorSpawnFromClass; by-name K2_DestroyActor) --
// none of it dispatches through ProcessEvent, so our interceptors/observers
// never saw it (zero npc-suppress[client] / npc-sync[host]-broadcast lines in
// every real-session log). A client's conversion ran fully LOCAL (untracked
// rogue kerfur, then an untracked local prop), and even a HOST conversion
// spawned an NPC no client could see until the next connect-edge world walk.
//
// THE FIX (MTA request shape -- DoorOpenRequest precedent):
//   - CLIENT: PRE-interceptors on the two PE-visible menu dispatchers CANCEL the
//     local conversion and queue a request; Tick() (game thread) resolves the
//     WIRE eid off the actor's mirror Element (the host id -- the local tracker
//     only holds a peer-range shadow) and sends KerfurConvertRequest{eid,
//     toProp}. (Cancel-at-the-script-fn is the ambient_spawner_suppress
//     precedent; the verbs themselves are the v44 EX_LocalVirtualFunction trap
//     -- not hookable.)
//   - HOST: the request handler resolves eid -> actor, validates (live, right
//     class, the BP's `kill` guard replicated from the disassembly), executes
//     the REAL verb via ProcessEvent, then CONVERGES the BP-internal side
//     effects the pipelines could not see:
//       actor !IsLive       -> npc_sync::SyncDestroyedNpcActor /
//                              prop_lifecycle::SyncDestroyedPropActor
//       new prop (+ floppy) -> ExpressSpawnedProp over a targeted GUObjectArray
//                              walk (untracked prop_kerfurOmega_C- /
//                              prop_floppyDisc_C-descendants)
//       new NPC             -> npc_sync::RegisterExistingWorldNpcs (broadcasts
//                              EntitySpawn for newly-registered while connected)
//   - HOST's OWN menu use: the same interceptors pass the dispatch through
//     (the BP converts natively) and queue the converge for the next Tick()
//     (interceptor context may not Post / re-enter ProcessEvent).
//
// Principle 7: gameplay/network module; engine access via ue_wrap reflection /
// game_thread only. One feature, own file pair.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct KerfurConvertPayload;
}  // namespace coop::net

namespace coop::kerfur_convert {

// Idempotent install, retried each net-pump tick until the kerfur BP classes
// load: resolves kerfurOmega_C::actionName + prop_kerfurOmega_C::actionOptionIndex
// (+ their 'name' / 'action' param offsets) and registers the two PRE
// interceptors; resolves dropKerfurProp / spawnKerfuro for the host-execution
// path + the `kill` field offset for the replicated guard + the prop/floppy
// classes for the converge walk. Caches `session`.
void Install(coop::net::Session* session);

// THE conversion detector -- called from npc_sync::NpcSuppress_Interceptor (the
// PE-visible BeginDeferredActorSpawnFromClass detour) with the spawn's params
// frame + ActorClass + the caller's role. The kerfur radial menu's actionName/
// actionOptionIndex dispatch is EX_LocalVirtualFunction = PE-INVISIBLE (so the
// interceptors on them never fire); the conversion is detected here instead, by
// the handler's BeginDeferred whose WorldContextObject == the kerfur self
// (WCO=kerfurOmega + spawn=prop => turn_off; WCO=prop + spawn=kerfurOmega =>
// turn_on). Records the host-auth request (client) / converge (host) for Tick().
// RETURNS true iff the CLIENT should VETO this local spawn (the ghost-prop /
// duplicate-NPC dupe source) -- the caller then zeroes the BP ReturnValue. Host
// always returns false (the real conversion runs locally + mirrors via converge).
// Interceptor-safe: reflection reads + leaf mutex only. No-op until installed.
bool OnBeginDeferredSpawn(void* params, void* actorClass, bool isClient, bool isHost);

// HOST-only receiver for KerfurConvertRequest (wired in event_dispatch_state).
// Validates + executes the verb + converges, all on the game thread (the
// event_feed drain). senderPeerSlot is log-only.
void OnConvertRequest(const coop::net::KerfurConvertPayload& payload,
                      uint8_t senderPeerSlot);

// Drain the deferred-action queue (pushed by the interceptors, which may run
// on a parallel-anim worker and must not call engine functions or walk the
// element registries): client entries resolve the wire eid off the actor's
// mirror Element + send the request; host entries converge -- one full tick
// AFTER they were pushed (two-phase arming; a nested dispatch inside the
// conversion verb can drain this queue mid-verb otherwise). Cheap no-op when
// empty. Game thread (net-pump tick).
void Tick();

// Clear per-session state (the pending queue). Net disconnect.
void OnDisconnect();

}  // namespace coop::kerfur_convert
