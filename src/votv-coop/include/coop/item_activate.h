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
// payloads -- mismatched classes no-op. Game thread only.
void ApplyToPuppet(void* puppetActor, const coop::net::ItemActivatePayload& p);

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
