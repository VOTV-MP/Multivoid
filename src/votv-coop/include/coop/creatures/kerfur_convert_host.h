// coop/creatures/kerfur_convert_host.h -- the HOST executor half of the kerfur conversion: the
// KerfurConvertRequest execution (the real verb, run on the host's copy) and the CONVERGE at the verb's
// return (the captured successor registered, BindFormActor into a KerfurConvert broadcast). The verb's
// gate watches live in kerfur_convert.{h,cpp}, the CLIENT half in kerfur_convert_client.h. Feature
// narrative: kerfur_convert.h.
//
// Principle 7: gameplay/network module; engine access through ue_wrap only.

#pragma once

#include "coop/element/element.h"

#include <cstdint>

namespace coop::net {
class Session;
struct KerfurConvertPayload;
}  // namespace coop::net

namespace coop::kerfur_convert_host {

// The session, for the one wire send of its own (a failed turn-on's prop death); the kerfur_convert
// install calls it on every attempt, so a reconnect that changes the session stays fresh here.
void SetSession(coop::net::Session* session);

// Resolved kerfur class pointers (npc base, prop base), pushed by kerfur_convert::Install as soon as
// both resolve. A collar variant's dropKerfurProp is looked up where used.
void SetClasses(void* npcClass, void* propClass);

// Resolved verb refs and the request latch. Pushed ONLY at the Install SUCCESS site, so
// OnConvertRequest's gate flips exactly when the feature is live. Fail-closed: the DISABLED install
// state, where a verb's signature changed, never reaches here, so a request in that state is DROPPED
// rather than CallFunction-ing a signature-changed verb over a zeroed 16-byte frame.
void SetVerbs(void* spawnKerfuroFn, int32_t killOff);

// HOST-only receiver for KerfurConvertRequest (wired in event_dispatch_intent). Refuses an unknown sender, a
// request before install resolved and an eid outside the host range into the lane's refusal tally, and queues the
// rest per sender, up to a cap, refusing past it. payload.elementId is the requester's mirror eid of the current
// form; senderPeerSlot is who asked, which the converge's log names. Game thread (the event_feed drain).
void OnConvertRequest(const coop::net::KerfurConvertPayload& payload, uint8_t senderPeerSlot);

// A refusal the router made before the lane saw the request (a short payload, a bad toProp), said through the
// lane's tally. The slot is the router's, 0xFF for an unknown sender. Game thread.
void NoteRefused(uint8_t senderPeerSlot, const char* why);

// HOST per pump tick: serves one queued request per sender at a bounded rate, in arrival order. It checks the
// kerfur as it is now (a live one of the right form, not in kill mode), runs the real verb on the host's copy, and
// the verb's return converges it through ConvergeAtReturn, inside the call. Then says the refusals' summed line.
// Game thread.
void Tick();

// Instruments (the kerfur_flood drill, coop/dev/hostile_drill_kerfur), game thread, per client slot (0 for any
// other, the host's included): how many
// requests were served (entered the cost path: the checks and the verb), and how many the lane refused before the
// serve: the router's refusals (a short payload, a bad toProp), an unknown sender, a request before install, an eid
// outside the host range, and a full queue. A refusal at the serve (no live kerfur of that form, kill mode) is a run
// and is not counted here.
uint32_t RunsFor(int slot);
uint32_t RefusedFor(int slot);

// The slot whose request is running a verb on `object` right now, or -1: the verb's entry records it,
// so its return can name whose request it was. Game thread.
int RequestSlotFor(void* object);

// The converge at a conversion verb's return, from the gate's post on the host. `oldEid` is the old
// form's element id at the verb's entry, invalid for a kerfur never enrolled. The old form still alive
// means the verb refused (a sentient kerfur, a failed spawn): nothing converted anywhere, since a
// client never runs the verb, so it is only logged. Otherwise the successor the verb's bracket
// captured is registered silently, the old form released, and BindFormActor broadcasts the one
// KerfurConvert; no captured successor, or one that cannot be bound, is a plain death, relayed. The
// conversion's floppy is an ordinary keyed prop, expressed where every fresh prop is. Game thread.
void ConvergeAtReturn(void* oldActor, int32_t oldIdx, coop::element::ElementId oldEid, bool toProp,
                      int requestSlot);

// Clear the running request and the tally. Net disconnect (fanned from kerfur_convert::OnDisconnect).
void OnDisconnect();

}  // namespace coop::kerfur_convert_host
