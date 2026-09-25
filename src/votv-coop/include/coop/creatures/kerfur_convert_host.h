// coop/creatures/kerfur_convert_host.h -- the HOST executor half of the kerfur conversion feature:
// the KerfurConvertRequest execution (verb dispatch plus the request-verb bracket) and the
// post-verb CONVERGE (new-form search and registration, BindFormActor into a KerfurConvert
// broadcast, floppy express). The DETECTION -- the death-watch poll and the first-refusal seams --
// stays in kerfur_convert.{h,cpp}, and the CLIENT half lives in kerfur_convert_client.h. Feature
// narrative: kerfur_convert.h.
//
// Principle 7: gameplay/network module; engine access through ue_wrap only.

#pragma once

#include "coop/element/element.h"
#include "ue_wrap/core/types.h"  // FRotator (ConvergeAfterConversion)

#include <cstdint>

namespace coop::net {
struct KerfurConvertPayload;
}  // namespace coop::net

namespace coop::kerfur_convert_host {

// Resolved kerfur class pointers (npc base, prop base). Pushed by kerfur_convert::Install as soon as
// both resolve, as an idempotent overwrite: the converge machinery (poll-driven
// ConvergeAfterConversion and ExpressConversionFloppies) works from then, including in the DISABLED
// install state. The floppy class and a collar variant's dropKerfurProp are looked up where used.
void SetClasses(void* npcClass, void* propClass);

// Resolved verb refs and the request latch. Pushed ONLY at the Install SUCCESS site, the same
// instant the residual's g_installed latches, so OnConvertRequest's gate flips exactly when the
// feature is live. This is deliberately fail-closed: the DISABLED install state, where a verb's
// signature changed, never reaches here, so a request in that state is DROPPED rather than
// CallFunction-ing a signature-changed verb over a zeroed 16-byte frame. That would be an over-read
// -- latent on the current game build, where the verbs take no params.
void SetVerbs(void* spawnKerfuroFn, int32_t killOff);

// HOST-only receiver for KerfurConvertRequest (wired in event_dispatch_intent).
// Validates + executes the verb + converges, all on the game thread (the
// event_feed drain). senderPeerSlot is log-only. payload.elementId is the
// dying-form's host-range MIRROR eid (the client is eid-based); the host
// resolves both the actor and the stable KerfurId from it.
void OnConvertRequest(const coop::net::KerfurConvertPayload& payload,
                      uint8_t senderPeerSlot);

// The post-verb converge: find/adopt the new-form actor, silently register it,
// release the dying form, BindFormActor -> ONE KerfurConvert broadcast (or the
// rejected echo when the old form survived). The new form is bound at its own pose;
// unread, the converge fails as a failed register does. (px,py,pz) and rot0 are the
// old form's location and rotation, read before the verb or the death-watch's last
// live ones: the location centres the successor search, and a reject falls back to
// each half when the surviving form's own read fails; rot0 is null when no read gave one,
// and always from the poll, whose old form is dead and so never rejects. The new form is
// the one the form assembler captured in the verb's bracket, else the nearest untracked
// kerfur of that form within 500 cm. Called by OnConvertRequest (both verbs) and by the
// residual death-watch poll's host branches. Game thread.
void ConvergeAfterConversion(void* oldActor, int32_t oldIdx, coop::element::ElementId oldEid,
                             uint8_t toProp, float px, float py, float pz, const ue_wrap::FRotator* rot0);

// Express the conversion's dropped floppy prop(s) the normal keyed way (they
// spawn BP-internally). Called by the converge and by the residual spawn-edge
// first refusal (TryAdoptFreshKerfurProp). Game thread.
void ExpressConversionFloppies(float x, float y, float z);

// A kerfur NPC form's death with no KerfurConvert to follow, retired as a plain one: its element
// released with an EntityDestroy to every peer, and its kerfur record released. The turn-off
// destroys the NPC inside a blueprint by name, which npc_sync's ProcessEvent destroy observer
// does not see, so this is that NPC's only wire death when its conversion cannot be bound. Host,
// game thread.
void RetireNpcFormAsDeath(coop::element::ElementId eid, void* actor);

// One-way owner API for the residual destroy seam: record the seam's inline converge for
// OnConvertRequest ONLY when the seam fired inside ITS verb bracket. A host-own toggle has no
// consumer, and an unconditional write would leave a stale eid that a later recycled-eid request
// could falsely consume. The bracket predicate lives HERE, next to the state.
void RecordSeamConvergedInBracket(coop::element::ElementId dyingEid);

// Clear the request-verb bracket pair. Net disconnect (fanned from
// kerfur_convert::OnDisconnect).
void OnDisconnect();

}  // namespace coop::kerfur_convert_host
