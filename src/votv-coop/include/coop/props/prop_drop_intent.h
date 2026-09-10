// coop/props/prop_drop_intent.h -- the CLIENT-place to HOST-authoritative keyed-prop DROP INTENT
// lane.
//
// ONE concept: a CLIENT placing a keyed world prop it had PICKED UP (hold-R place). Prop sync is
// host-authoritative, so a client's own fresh Aprop_C spawn is skipped in prop_lifecycle and a
// client-placed rock would be INVISIBLE to the host. The answer is the pattern chipPiles already
// use: the CLIENT sends an INTENT and the HOST is the sole authority. This is the DROP half -- the
// grab half already works, the hold-R pickup DESTROY crossing the bidirectional destroy-seam. Each
// step is documented on the entry point performing it: NoteClientKeyedDestroy, Install, Tick,
// OnPropDropIntent.
//
// WHY Tick sends a tick LATE: the place ALSO destroys the in-hand display husk, and if that crosses
// as DESTROY(key) the reliable channel delivers it BEFORE the intent, so the host processes it
// against a rock it no longer has -- a no-op -- and only then spawns from the intent, and the spawn
// survives. Authoring in the same tick lets the husk-destroy kill it instead.

#pragma once

#include <string>

#include "coop/net/protocol.h"

namespace coop::net { class Session; }

namespace coop::prop_drop_intent {

// Install the CLIENT FinishSpawn post-hook (chains after host_spawn_watcher's on the same
// FinishSpawningActor UFunction). Idempotent + retry-throttled while the UFunction is unresolved.
// Game thread. Safe to call every subsystem-install tick.
void Install(coop::net::Session* session);

// Per-net-pump-tick drain (CLIENT): for each pending place spawn whose Key is now restored AND
// parked, author a PropDropIntent{className,key,propName,transform,scale,physFlags} to the host and
// unpark. The Key only becomes readable a tick after the spawn, once loadData has restored it. Game
// thread. No-op on the host, or on empty pending.
void Tick(coop::net::Session* session);

// Called from prop_lifecycle::DestroySeamBody right AFTER a CLIENT broadcasts a keyed-prop DESTROY:
// park the key so a later same-key place authors a host-authoritative drop intent, and only then.
// The park is the SAFETY INVARIANT -- it means the host has already destroyed its copy, so the
// later re-spawn authors exactly ONE host prop and there is no dup. Game thread. Bounded FIFO set.
void NoteClientKeyedDestroy(const std::wstring& key);

// HOST handler for a received PropDropIntent: spawn the authoritative Aprop_C by Key at the
// transform, NOT echo-suppressed, so the host's own FinishSpawn watcher expresses it and broadcasts
// PropSpawn to every peer. The placing client adopts its own untracked local rock by Key, through
// ResolveLiveActorByKey's scan fallback. Dup-guarded: skipped if the host already has the Key live.
// Game thread.
void OnPropDropIntent(coop::net::Session& session, const coop::net::PropDropIntentPayload& p,
                      uint8_t senderSlot);

// HOST handler for ReliableKind::ReelEjectIntent -- a CLIENT's device eject birthed a prop that a
// client Aprop_C spawn never broadcasts, so the host authors it through the SAME HostSpawnPlacedProp
// path. CLASS-WHITELISTED to FOUR lineages, not a general client-spawn door: the reel (caddy and
// reelbox), the desk module, the drive and the floppy disc. The kind's name predates the other
// three and now understates it; the receiver gate and the client's own fresh-birth gate name the
// same four. The kSleep flag makes the host copy spawn inert until the client's held-prop pose
// stream drives it, and it is set for the three born INTO A HAND -- a disc is dropped at its
// device's mouth with nobody holding it, so it falls on the host instead. The prop's own state
// follows on PropSaveDataIntent in the same FIFO.
void OnReelEjectIntent(coop::net::Session& session, const coop::net::PropDropIntentPayload& p,
                       uint8_t senderSlot);

// Session teardown -- clear the park set + pending. Game thread.
void Reset();

}  // namespace coop::prop_drop_intent
