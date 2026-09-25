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

// HOST-only receiver for KerfurConvertRequest (wired in event_dispatch_intent). Validates, then runs the
// real verb on the host's copy; the verb's return converges it through ConvergeAtReturn, inside the
// call. payload.elementId is the requester's mirror eid of the current form; senderPeerSlot is who
// asked, which the converge's log names. Game thread (the event_feed drain).
void OnConvertRequest(const coop::net::KerfurConvertPayload& payload, uint8_t senderPeerSlot);

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

// Clear the running request. Net disconnect (fanned from kerfur_convert::OnDisconnect).
void OnDisconnect();

}  // namespace coop::kerfur_convert_host
