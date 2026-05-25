// dev/restore_vitals.h -- F3 dev-key: refill food/sleep/health on both peers'
// UsaveSlot_C simultaneously. (coffeePower is intentionally excluded -- writing
// it to 100 triggers a screen-shake post-coffee BP side-effect; see commit
// 5421d6f for the user-retest finding.)
//
// Gated by votv-coop.ini ([dev] devkeys=1); OFF by default. While enabled:
//   F3 -- restore the local player's vitals AND broadcast a RestoreVitals
//         reliable packet so the remote peer's vitals are also restored.
//
// Direction: any peer (host OR client) can press F3 -- the packet is
// broadcast, both ends call ApplyLocally(). Echo is safe: applying max-out
// twice is idempotent (the field just stays at max).
//
// [dev] enabled=0 master-kill still applies; devkeys=1 alone won't enable
// if the master is off. Mirrors the pos_hud Init pattern.

#pragma once

namespace coop::net { class Session; }

namespace dev::restore_vitals {

// Cache the Session pointer so F3 can broadcast the RestoreVitals packet.
// Called once from harness boot, BEFORE Init(). Mirrors prop_lifecycle::SetSession.
void SetSession(coop::net::Session* session);

// Read votv-coop.ini; if [dev] devkeys=1 (and master not killed), start the
// F3 hotkey thread. No-op otherwise. Idempotent -- repeated calls do nothing.
void Init();

// Receiver: max-out food/sleep/health on the local UsaveSlot_C (coffeePower
// intentionally excluded -- see header comment).
// Called from event_feed.cpp on incoming ReliableKind::RestoreVitals, AND
// called locally by the F3 hotkey before broadcasting. Game thread only.
void ApplyLocally();

}  // namespace dev::restore_vitals
