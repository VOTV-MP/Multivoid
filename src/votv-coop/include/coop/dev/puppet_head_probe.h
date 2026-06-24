// coop/dev/puppet_head_probe.h -- positive-confirm probe for the puppet head-look
// FREEZE (RE: research/findings/votv-puppet-head-freeze-backturned-RE-2026-06-24.md).
//
// Measures, observer-side, the DESIRED head twist (the replicated look-input vs the
// puppet's presentation body yaw) against the ACTUAL rendered 'head'/'neck' bone twist
// and the native FAnimNode_LookAt clamp. If the head PINS at ~67 deg (head 45 + neck
// 22 @ Alpha 0.5) while the desired offset keeps growing past it, the LookAtClamp is
// POSITIVELY confirmed as the freeze gate (vs the by-elimination diagnosis). Then we
// widen the clamp puppet-only (90 deg). Diagnostic-only, ini-gated
// ([votvcoop] puppet_head_probe=1), ~1 Hz per puppet; RULE-2-exempt (probe;
// [[feedback-rule2-exempts-probes-diagnostics-tools]]).
#pragma once

namespace coop::puppet_head_probe {

// Call once per puppet per tick from RemotePlayer::ApplyToEngine, AFTER the head drive.
//   bodyYawDeg        = the presentation body yaw the actor shows (bodyYaw_.Yaw()).
//   desiredLookYawDeg = curYaw_ + curHeadYawDelta_ (the camera world yaw the head aims at).
//   desiredPitchDeg   = curPitch_.
// No-op unless the ini flag is set; self-throttled to ~1 Hz per actor. Game thread only.
void Tick(void* puppetActor, float bodyYawDeg, float desiredLookYawDeg, float desiredPitchDeg);

}  // namespace coop::puppet_head_probe
