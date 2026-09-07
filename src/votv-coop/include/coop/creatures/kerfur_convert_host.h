// coop/kerfur_convert_host.h -- the HOST executor half of the kerfur conversion
// feature (extracted verbatim from kerfur_convert.cpp, 2026-07-19 s27 cut):
// the KerfurConvertRequest execution (verb dispatch + the request-verb bracket)
// and the post-verb CONVERGE (new-form search/registration, BindFormActor ->
// KerfurConvert broadcast, floppy express). The DETECTION (death-watch poll +
// first-refusal seams) stays in kerfur_convert.{h,cpp}; the CLIENT half lives
// in kerfur_convert_client.h. Feature narrative: kerfur_convert.h.
//
// Principle 7: gameplay/network module; engine access via ue_wrap only.

#pragma once

#include "coop/element/element.h"

#include <cstdint>

namespace coop::net {
struct KerfurConvertPayload;
}  // namespace coop::net

namespace coop::kerfur_convert_host {

// Resolved kerfur class pointers (npc base / prop base / floppy). Pushed by
// kerfur_convert::Install every attempt right after its opportunistic resolve
// block (idempotent overwrite) -- the converge machinery (poll-driven
// ConvergeAfterConversion / ExpressConversionFloppies) works as soon as the
// classes resolve, including in the DISABLED install state, exactly like the
// pre-cut single-TU statics.
void SetClasses(void* npcClass, void* propClass, void* floppyClass);

// Resolved verb refs + the request latch. Pushed ONLY at the Install SUCCESS
// site (the same instant the residual's g_installed latches) -- so
// OnConvertRequest's gate flips at exactly the pre-cut moment. Deliberate
// fail-closed deviation (s27 cut, documented): the DISABLED install state
// (signature-changed verbs) no longer reaches this, so a request in that state
// is DROPPED instead of CallFunction-ing a signature-changed verb over a
// zeroed 16-byte frame (the gate this replaced passed it -- a latent over-read;
// unreachable on the current game build where the verbs take no params).
void SetVerbs(void* dropPropFnBase, void* dropPropFnCol, void* dropPropFnColGamer,
              void* colClass, void* colGamerClass, void* spawnKerfuroFn, int32_t killOff);

// HOST-only receiver for KerfurConvertRequest (wired in event_dispatch_intent).
// Validates + executes the verb + converges, all on the game thread (the
// event_feed drain). senderPeerSlot is log-only. payload.elementId is the
// dying-form's host-range MIRROR eid (the client is eid-based); the host
// resolves both the actor and the stable KerfurId from it.
void OnConvertRequest(const coop::net::KerfurConvertPayload& payload,
                      uint8_t senderPeerSlot);

// The post-verb converge: find/adopt the new-form actor, silently register it,
// release the dying form, BindFormActor -> ONE KerfurConvert broadcast (or the
// rejected echo when the old form survived). Called by OnConvertRequest (both
// verbs) and by the residual death-watch poll's host branches. Game thread.
void ConvergeAfterConversion(void* oldActor, int32_t oldIdx, coop::element::ElementId oldEid,
                             uint8_t toProp, float px, float py, float pz,
                             void* capturedForm = nullptr, int32_t capturedIdx = -1);

// Express the conversion's dropped floppy prop(s) the normal keyed way (they
// spawn BP-internally). Called by the converge and by the residual spawn-edge
// first refusal (TryAdoptFreshKerfurProp). Game thread.
void ExpressConversionFloppies(float x, float y, float z);

// OBSERVE (2026-07-14, G1 increment): the host-range eid of the conversion request the host is
// CURRENTLY executing via R::CallFunction inside OnConvertRequest, or kInvalidId when not inside one
// (the g_requestVerbEid bracket). The host-exec-client-request path runs the verb via CallFunction,
// which is ASSEMBLER-BLIND -- the 0x45 substrate only catches the local EX_Local menu toggle, never a
// CallFunction dispatch. The kerfur_form_assembler consults this to recognize the CallFunction bracket
// as a SECOND capture scope (else a CallFunction-route B lands in formOut, indistinguishable from a
// world-load spawn). Game-thread only marker (no lock needed).
coop::element::ElementId ActiveRequestVerbEid();

// One-way owner API for the residual destroy seam (s21b MarkDirtyFromVerb shape):
// record the seam's inline converge for OnConvertRequest ONLY when the seam fired
// inside ITS verb bracket -- a host-OWN toggle has no consumer and an unconditional
// write would leave a stale eid that a later recycled-eid request could falsely
// consume (audit finding 3). The bracket predicate lives HERE, next to the state.
void RecordSeamConvergedInBracket(coop::element::ElementId dyingEid);

// Clear the request-verb bracket pair. Net disconnect (fanned from
// kerfur_convert::OnDisconnect).
void OnDisconnect();

}  // namespace coop::kerfur_convert_host
