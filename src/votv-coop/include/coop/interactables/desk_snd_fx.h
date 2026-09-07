// coop/interactables/desk_snd_fx.h -- the desk audio-effect mirror.
//
// Every unit-1 desk sound the presser hears is played by presser-LOCAL blueprint paths -- the key
// clicks (audio_coordKeyPress), the verb beeps (beepLong1 and beep4, through the
// playButtonSound/playPingSound EX_Local* helpers), the broken-radar fail (audio_coordFail), the
// cursor loop and the ping loop -- so observers heard NOTHING. The lane forwards the EFFECT at the
// NATIVE audio seam rather than classifying inputs. It func-patches AudioComponent:Play and
// ActorComponent:SetActive/Activate: every whitelisted call site measures as EX_VirtualFunction on
// a NATIVE target, so the dispatch funnels through UFunction->Func whatever the caller's opcode
// (docs/COOP_DISPATCH_VISIBILITY.md). The detour filters by POINTER COMPARE against the desk's six
// resolved components in ue_wrap/desk/desk_audio, excluding the laptop's same-named components for free,
// and enqueues {op, comp, cue} into a game-thread ring that Tick ships as the relayed
// ReliableKind::DeskSndFx. This lane owns ALL unit-1 one-shot and loop desk audio; the hums,
// stopSound and live-volume effects stay with the DeskInput lane, on different components.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::desk_snd_fx {

void Install(coop::net::Session* session);

// Per net-pump tick (GT): lazy hook install (latched), instance-cache
// refresh, ring flush -> wire, pending loop-apply retry, fires/sec counters
// log (60 s cadence, only when nonzero).
void Tick();

// Receiver (event_dispatch_state, GT). Symmetric, trusting any sender, per the claim-free desk
// doctrine that whoever really pressed authors. Mirrors replay SetSound+Play or SetActive under the
// wire-apply guard below -- our own replay funnels through ->Func as well, and the guard is what
// kills the echo.
void OnDeskSndFx(const coop::net::DeskSndFxPayload& p, uint8_t senderSlot);

// HOST: re-assert current loop ground truth to a joiner (ConnectReplayForSlot). Loops are STATE,
// not events, so the answer comes from the components themselves (bIsActive) rather than from a
// replayed history.
void QueueConnectBroadcastForSlot(int slot);

// HOST: a peer left -- broadcast and apply OFF for the loops it authored. Teardown is host-owned
// because the attribution map is host-only. OnDisconnect forces wire-set loops off locally instead.
void OnPeerLeft(int slot);

void OnDisconnect();

// The wire-apply echo guard (GT-only). EVERY code path that drives desk
// engine state from the WIRE (this lane's replays, desk_input applies, the
// cursor mirror's writes/unfocus replay) holds this so organic-vs-wire
// attribution at the audio detour stays exact.
bool InWireApply();
struct ScopedWireApply {
    ScopedWireApply();
    ~ScopedWireApply();
};

}  // namespace coop::desk_snd_fx
