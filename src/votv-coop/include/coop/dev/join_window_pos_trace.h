// coop/dev/join_window_pos_trace.h -- CLIENT read-only trace of where a KEYED prop ends up after a
// join. Ini-gated [dev] join_window_pos_trace=1; absent or 0 makes every call an early return.
//
// A host that moves a keyed prop while a client joins can leave the client rendering it at the SAVE
// position, and two paths produce that: the client's own loadObjects recreating the prop on top of
// the snapshot's host position, or the prop being HELD when the snapshot went out, which
// drive-skips the express so nothing is placed. They want different fixes, so the probe separates
// them by recorded ORDER rather than by inference.
//
// Per key it stamps the snapshot express (host position, the actor's position then, whether it was
// held) and the keyed-churn re-bind onto the recreate; a re-bind stamp above the express stamp is
// what proves the recreate landed second. At load quiescence it resolves each key's final actor by
// eid and classifies it: clobbered, snapshot-won, held at the snapshot by the host or by us, dead
// (a rock's pickup destroys the world actor), no-snapshot, or unresolved. Piles are not traced.
// Game-thread only.
#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap { struct FVector; }

namespace coop::dev::join_window_pos_trace {

// True iff [dev] join_window_pos_trace=1 (latched once). When false, every call below is a cheap no-op.
bool IsEnabled();

// CLIENT join edge: arm the trace + clear the per-join record set. Called from BeginClaimTracking
// (SnapshotBegin), the same seam spawn_order_probe arms at. No-op when disabled.
void ArmForJoin();

// A) CLIENT snapshot expression (remote_prop_spawn.cpp exact-key branch): the host is expressing keyed
// prop `key`/`eid`; `hostPos` is the pos the snapshot carried, `actor` the resolved local actor,
// `hostHeld` whether the host held it (a drive-skip / local-grab-skip fired -> pos NOT placed).
// Behind IsEnabled it reads the actor's current pos. No-op when disabled / not armed.
void NoteSnapshotExpression(const std::wstring& key, uint32_t eid, void* actor,
                            const ue_wrap::FVector& hostPos, bool hostHeld);

// B) CLIENT keyed-churn RE-BIND (join_membership_sweep.cpp): the loadObjects-recreate of `key`/`eid`
// re-bound the mirror row onto `actor`. Behind IsEnabled it reads the recreate actor's pos (the
// candidate save-pos) + stamps the order. No-op when disabled / not armed.
void NoteRecreateRebind(const std::wstring& key, uint32_t eid, void* actor);

// CLIENT load quiescence (the sweep-fire point): resolve each key's final bound actor by eid, classify
// the root per key, emit the aggregate verdict, disarm. No-op when disabled / not armed.
void EmitVerdictAtQuiescence();

}  // namespace coop::dev::join_window_pos_trace
