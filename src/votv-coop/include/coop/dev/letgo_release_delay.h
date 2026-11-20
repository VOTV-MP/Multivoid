// coop/dev/letgo_release_delay.h -- dev knob (ini letgo_release_delay_ms / env VOTVCOOP_LETGO_RELEASE_DELAY_MS, default 0 =
// off): a CLIENT holds each reliable PropRelease it receives for that many milliseconds before applying it.
//
// The host's let-go row streams on the unreliable pose lane and its hand's release travels on the reliable lane; a let-go
// whose first pose lands before the release is the order the coast drill's letgo arm needs and a run only sometimes gives. The
// hold forces it. It must stay below the hand lane's 500 ms stream-silence release (remote_prop::Tick), which would end the
// slot's drive before the held release arrives. The host's own releases are never held.
//   Red (letgo_unyield_off): the driven-prop stream erases the yield it kept pending instead of lifting it (prop_drive_stream).

#pragma once

namespace coop::net { struct PropReleasePayload; }

namespace coop::dev::letgo_release_delay {

// The configured hold in ms, 0 when the knob is off. Read once.
long DelayMs();

// The PropRelease dispatch's call, game thread: applies the release through remote_prop::OnRelease at once, or, on a client
// with the knob on, queues it for DelayMs() and applies it in Tick.
void Release(int senderSlot, const coop::net::PropReleasePayload& payload, void* localPlayer);

// Applies the held releases whose time has come, oldest first. Every pump tick. Game thread.
void Tick();

// The session ended: a held release is dropped with it, as at a world change. At most 64 are held;
// a full queue sheds its oldest.
void OnDisconnect();

}  // namespace coop::dev::letgo_release_delay
