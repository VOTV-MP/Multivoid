// coop/interactables/desk_snd_fx.h -- v115: the desk audio-effect mirror.
//
// PROBLEM (user hands-on 2026-07-17): every unit-1 desk sound the presser
// hears is played by presser-LOCAL BP paths -- OnKeyDown/OnKeyUp clicks
// (audio_coordKeyPress), verb outcome beeps (beepLong1/beep4 through the
// playButtonSound/playPingSound EX_Local* helpers), the broken-radar fail
// (audio_coordFail), the cursor-movement loop (corrds_loop, spaceRenderer
// edge-guard) and the ping loop -- so observers heard NOTHING.
//
// ROOT SHAPE (qf 2026-07-17, 6 rounds "that holds"): forward the EFFECT at
// the NATIVE audio seam instead of classifying inputs. Func-patch
// AudioComponent:Play + ActorComponent:SetActive/Activate (every whitelist-
// comp call site measured EX_VirtualFunction on a NATIVE target -> the
// dispatch funnels through UFunction->Func regardless of caller opcode --
// docs/COOP_DISPATCH_VISIBILITY.md, K2_DestroyActor precedent). The detour
// filters by POINTER COMPARE against the desk's 6 resolved comps
// (ue_wrap/desk_audio) -- the laptop's same-named comps are excluded for
// free -- and enqueues {op, comp, cue} into a GT ring; Tick ships it as the
// relayed ReliableKind::DeskSndFx. Mirrors replay SetSound+Play / SetActive
// under the wire-apply guard below (our replay dispatch also funnels through
// ->Func; the guard kills the echo).
//
// AXIS OWNERSHIP (anti-smear): this lane owns ALL unit-1 one-shot/loop desk
// audio. RULE-2 retirements in the same change: PlayScanEffects' beep (its
// spawnDirs VISUAL stays on DeskScanEvent) and signal_catch's
// PlayPingSuccess replay. The hums/stopSound/live-volume effects stay with
// the v112 DeskInput lane (different comps, different axis).
//
// LOOPS ARE STATE: the host re-asserts current loop state (component ground
// truth, bIsActive) to a joiner at ConnectReplayForSlot; leaver teardown is
// HOST-OWNED (attribution map host-only; the host broadcasts the OFF as a
// normal event). OnDisconnect forces wire-set loops off locally.

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

// Receiver (event_dispatch_state, GT). Symmetric, trust any sender (the
// claim-free desk doctrine: whoever really pressed authors).
void OnDeskSndFx(const coop::net::DeskSndFxPayload& p, uint8_t senderSlot);

// HOST: re-assert current loop ground truth to a joiner (ConnectReplayForSlot).
void QueueConnectBroadcastForSlot(int slot);

// HOST: a peer left -- broadcast + apply OFF for loops it authored.
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
