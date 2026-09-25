// coop/creatures/kerfur_convert_client.h -- the client half of the kerfur conversion: the
// KerfurConvert wire apply (the old form's mirror destroyed, the new form's materialised). A client
// never converts a kerfur itself: its gate refuses the verb and asks the host (kerfur_convert), so
// every client, the one that asked included, takes the new form from this broadcast. The host
// executor lives in kerfur_convert_host; kerfur_convert.h carries the feature narrative. Gameplay
// layer: engine access through ue_wrap only.

#pragma once

namespace coop::net {
class Session;
struct KerfurConvertBroadcastPayload;
}  // namespace coop::net

namespace coop::kerfur_convert_client {

// The session pointer for the wire-apply role gate; the kerfur_convert install calls it on
// every attempt, so a reconnect that changes the session stays fresh here.
void SetSession(coop::net::Session* session);

// The client-only receiver for KerfurConvert, host to all. The sole conversion-transition signal:
// destroy the old-form mirror at oldEid, then materialise a mirror of the new form at newEid.
// `localPlayer` may be null; it feeds the prop teardown and materialise (the held-prop guard and the
// handle release). Game thread.
void OnKerfurConvert(const coop::net::KerfurConvertBroadcastPayload& payload, void* localPlayer);

}  // namespace coop::kerfur_convert_client
