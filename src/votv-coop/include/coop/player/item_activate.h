// coop/item_activate.h -- unified item-activation sync (Phase 5F).
//
// First instance: flashlight on/off. Architecture per the RE doc
// (research/findings/votv-flashlight-RE-2026-05-25.md) -- Case b: the
// world light cone is mainPlayer_C::light_R, not a component on the
// flashlight item actor. The puppet (also a mainPlayer_C orphan) has
// the same light_R at the same offset; toggling its visibility is the
// full implementation.
//
// Sender path: ProcessEvent POST-observer on
// AmainPlayer_C::updateFlashlight -- reads `mp.flashlight @0x0838` +
// broadcasts ItemActivatePayload to peers. Echo-suppression flag keeps
// the receiver's apply from bouncing back.
//
// Receiver path: ApplyToPuppet writes puppet.flashlight @0x0838 +
// toggles puppet.light_R visibility via the existing
// engine::SetComponentVisible wrapper.
//
// [probe] flashlight_log=1 in votv-coop.ini adds extra log lines
// before+after every observed toggle so we can verify hands-on that
// the BP's early-return guard fires when !hasFlashlight, and that
// light_R.bVisible flips in lockstep with mp.flashlight. Default
// disabled.

#pragma once

#include <cstdint>

namespace coop::net { class Session; struct ItemActivatePayload; }

namespace coop::item_activate {

// Idempotent install: resolve mainPlayer_C class + updateFlashlight
// UFunction, register the POST observer. Short-circuits after success.
// Safe to call every NetPumpTick; the lookup retries until the class
// has been loaded by the game. Pass the session pointer so the
// observer can SendReliable; nullptr disables broadcasting.
void Install(coop::net::Session* session);

// Receiver-side apply: peer reported their flashlight state/cone changed.
// puppetActor = the mainPlayer_C orphan we already spawned for that peer.
// Payload carries: state (on/off), intensity, outer/inner cone angles,
// mode byte. classHash distinguishes flashlight vs future radio/torch
// payloads -- mismatched classes no-op. `senderPeerSlot` is the resolved
// sender peer slot (from event_feed's Registry::Get(senderElementId)
// lookup, with senderPeerSlot fallback when the mirror isn't yet
// established); used by the click-sound state map and per-peer logging.
// Game thread only.
void ApplyToPuppet(void* puppetActor, const coop::net::ItemActivatePayload& p,
                   uint8_t senderPeerSlot);

// Phase 5F Inc5 (connect-time replay) receiver entry: if `puppetActor`
// is valid, applies immediately (= ApplyToPuppet). Otherwise stashes
// the latest payload in a per-peer slot keyed by `senderPeerSlot`. The
// stashed payload is drained on the next TickConnect() call once the
// puppet has been spawned. Latest-wins -- a newer ItemActivate
// overrides a still-pending older one. `senderPeerSlot` is the resolved
// sender peer slot (see ApplyToPuppet above). Game thread only.
void ApplyToPuppetOrDefer(uint8_t senderPeerSlot, void* puppetActor,
                          const coop::net::ItemActivatePayload& p);

// Per-slot connect-time flashlight replay. Snapshots the LOCAL
// mainPlayer's current flashlight state and SENDS IT TO ONE SPECIFIC
// PEER via SendReliableToSlot. Called from the harness's per-slot
// connect-edge -- a single global aggregate broadcast that fired once
// would skip late-joiners (they'd never see the local flashlight).
//
// `peerSlot` is the coop::players::Registry slot of the newly-joined
// peer (1..kMaxPeers-1 on host; 0 on client). No-op + log if local
// flashlight is OFF (puppet default on receiver is already OFF, saves
// a redundant packet). Game thread only.
void QueueConnectBroadcastForSlot(int peerSlot);

// T2-4 (host-relay late-joiner peer-state replay). Where
// QueueConnectBroadcastForSlot replays THIS peer's OWN item state to a
// newly-joined slot, this replays every OTHER existing peer client's
// current item state to the joiner -- so it converges to the live world
// (ItemActivate is edge-triggered, so a flashlight already ON before the
// joiner arrived would otherwise stay dark on its screen until the owner
// next toggles). Host-only (the relay hub holds the per-peer cache);
// no-op on clients. `newSlot` is the freshly-connected client's slot.
// Game thread only.
void ReplayPeerStatesToSlot(int newSlot);

// Phase 5F Inc5 per-tick worker. Drains:
//   (a) pending broadcast queued by QueueConnectBroadcast() -- retries
//       SendReliable until the channel accepts it, then updates the
//       observer dedup signature so the next press doesn't re-send.
//   (b) per-peer pending applies stashed by ApplyToPuppetOrDefer --
//       walks the puppet registry; for each peer slot with a pending
//       payload AND a valid puppet, applies + clears.
// Cheap (early-return) when no pending state. Game thread only.
void TickConnect();

// Phase 5F Inc5 disconnect hook: clears the pending broadcast + every
// per-peer pending apply. The stashed state belonged to the dead
// session; replaying it onto the next session's peers (possibly a
// different machine after IP change) would be wrong.
void OnDisconnect();

// Per-slot variant of OnDisconnect for mid-session peer drops. Clears
// ONLY peerSlot's pending apply (if any). Without this, a stale
// flashlight payload from a departed peer would re-apply when a NEW
// peer reuses that slot. Safe to call when no pending apply exists.
void OnDisconnectForSlot(int peerSlot);

// FNV-1a 32-bit hash of a wide string. Used for itemClassHash
// (CRC32 was named in the RE doc but FNV-1a is just as cross-peer
// stable, simpler to inline, no table). Exposed so the receiver can
// compute the expected hash to compare against the payload.
uint32_t HashClassName(const wchar_t* utf16);

// Autonomous-test entry point. Flips `mainPlayer.flashlight @0x0838`
// directly + invokes our send path with the new state. Used by the
// LAN flashlight test (harness/autotest.cpp) to drive toggles without
// relying on the BP graph (calling 'Flashlight Update' via reflection
// runs the graph but doesn't actually toggle the bool -- the BP is
// gated on input-state we can't fake from reflection). Returns the
// new state (true=on, false=off). Game thread only.
bool DebugForceToggle(void* mainPlayer);

}  // namespace coop::item_activate
