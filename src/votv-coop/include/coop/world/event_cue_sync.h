// coop/world/event_cue_sync.h -- HOST-AUTHORITATIVE cosmetic emitter-cue mirror.
//
// Many of VOTV's scheduled, random and story events manifest ONLY as a one-shot cosmetic particle
// emitter spawned through UGameplayStatics::SpawnEmitterAtLocation -- an EX_CallMath native call
// that BYPASSES our ProcessEvent detour, the same trap as the fireflies, and leaves NO mirrorable
// artifact: no actor, no Key, no spawn UFunction. So a client sees nothing at all. The case that
// started it is the meteor shower: trigger_eventer.runEvent('starRain') calls
// SpawnEmitterAtLocation with eff_shootingStar_rain at (0, 0, 6000).
//
// Unlike the fireflies, which are peer-symmetric because each peer rolls its own near its camera,
// these are WORLD events with a SINGLE producer, the host -- a client runs a dormant scheduler,
// since time_sync pins its TimeScale to 0, so it never fires an event itself. The host sees the
// eventer's runEvent through the script gate and broadcasts {cueId, pos} for a row that spawns a
// cue; the client replays that emitter there. No suppression, since clients never fire; no relay,
// since the host is the only origin; no echo, since the host never receives its own send.

#pragma once

namespace coop::net { class Session; struct EventCuePayload; }

namespace coop::event_cue_sync {

// Cache the session and register the host's watch on runEvent. Idempotent, called every net-pump
// tick from subsystems::Install, which is also the retry until the gate resolves the watch's name.
//
// The cue REGISTRY, in the .cpp, gives each cue an id, an emitter template and one of two detections.
// A ROW cue maps a runEvent row to the location its body spawns the emitter at: starRain, cue 0. A
// BORN cue is for a body that computes its position, or that no row announces: the host takes one
// object-index listener, keeps each ParticleSystemComponent birth until the next tick, and sends the
// component's own world place when its template is a born cue's: stolasHeal, cue 1, the growth area's
// glow at the plant it fertilizes. A client never takes the listener. The eye moon, pink beam, TriFO,
// blinking lights and green fire are candidates, one registry line each once their rows are read.
// cueId is on the wire, so the registry is APPEND-ONLY.
void Install(coop::net::Session* session);

// CLIENT: replay a cue's emitter at the host's broadcast position; an emitter the game has not loaded here is loaded by its asset
// path first, once per cue. Game thread.
void OnReliable(const coop::net::EventCuePayload& payload);

// HOST, the answer to joining mid-event: at a joiner's world-ready edge, send every live cue
// component's {cueId, pos} to that slot alone (ConnectReplayForSlot), found through the object
// index's particle-component list. Each was broadcast as it spawned and dropped for this
// still-loading slot, and the send gate opens in the same handler, so each arrives once. A mid-shower
// joiner replays the emitter fresh -- emitter PHASE is not synced, so it sees the remaining shower
// from t=0, accepted for transient cosmetics. No-op on a client, or when no cue template is loaded.
// Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// Teardown: drop the session pointer.
void OnDisconnect();

// [dev] How many cue emitters this peer has replayed, for the event drill. Any thread.
unsigned ReplayCount();

// [dev] How many cues this peer, as host, has sent (to every peer or to one slot), for the plant drill. Any thread.
unsigned SentCount();

// [dev] The place of this peer's latest sent / replayed cue, for the plant drill. Game thread.
void LastSentAt(float (&xyz)[3]);
void LastReplayAt(float (&xyz)[3]);

}  // namespace coop::event_cue_sync
