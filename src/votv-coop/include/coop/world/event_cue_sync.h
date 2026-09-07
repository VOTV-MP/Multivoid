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
// since time_sync pins its TimeScale to 0, so it never fires an event itself. The host diffs the
// live particle components against its last poll and broadcasts {cueId, pos} for a new one; the
// client replays that emitter at the broadcast position. No suppression, since clients never fire;
// no relay, since the host is the only origin; no echo, since the host never receives its own send.

#pragma once

namespace coop::net { class Session; struct EventCuePayload; }

namespace coop::event_cue_sync {

// Cache the session and opportunistically resolve the spawn path and the cue templates. Idempotent,
// called every net-pump tick from subsystems::Install; detection lives in Tick(), not here.
//
// The cue REGISTRY, in the .cpp, maps an emitter template to a cueId, plus an optional fixed spawn
// location for the cues the blueprint hardcodes. starRain is cue 0 and the ONLY cue registered so
// far; the eye moon, pink beam, TriFO, blinking lights and green fire are candidates, one registry
// line each when they are added. cueId is on the wire, so the registry is APPEND-ONLY.
//
// Detection is a ~1 Hz poll diff, so a cue must LIVE longer than about a second to be caught.
// Before registering a sub-second one-shot, give it a synchronous capture instead, the way
// firefly_sync brackets its tick -- otherwise it can spawn and die between polls and never
// broadcast at all.
void Install(coop::net::Session* session);

// HOST: the ~1 Hz authoritative detection poll (new cue PSC -> broadcast; kPollIntervalMs).
// No-op on a client / a solo peer. Game thread (driven from subsystems::TickGameplay, beside
// the other host pollers).
void Tick();

// CLIENT: replay a cue's emitter at the host's broadcast position. Game thread.
void OnReliable(const coop::net::EventCuePayload& payload);

// HOST, the answer to joining mid-event: at a joiner's world-ready edge, re-send every live,
// already-broadcast cue component's {cueId, pos} to that slot alone (ConnectReplayForSlot). One
// bounded component walk, since the join edge is rare. A mid-shower joiner replays the emitter
// fresh -- emitter PHASE is not synced, so it sees the remaining shower from t=0, accepted for
// transient cosmetics. Only cues already in the poll snapshot are re-sent: one newer than the last
// poll is delivered by the next broadcast instead, since the slot's send gate is open by then, and
// re-sending it here too would double the emitter on the joiner. No-op on a client, or when no cue
// template resolved. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// Teardown: clear the poll snapshot + drop the session pointer.
void OnDisconnect();

}  // namespace coop::event_cue_sync
