// coop/player/hand_item_pose.h -- where a hand item's display mirror sits: the owner's measure and the
// mirror's per-tick drive, both in view space (coop/player/hand_item.h is the axis).
//
// Display placement: the held item appears in front of the camera, as in the single-player
// view, seen in third person on the puppet. Natively the held item is welded into the
// first-person arms viewmodel chain, which is drawn camera-relative; that chain is hidden and
// never ticks on a puppet, and the puppet's camera never pitches, so the look is reproduced
// by a per-tick drive from the eye anchor and the synced look basis, following yaw and
// pitch. Measured, not tuned: the item's pose relative to the camera is item-specific (the
// weapon socket basis plus per-item offsets), so the owner measures its held actor's
// transform in its own view space and ships it with the announce, and the mirror re-composes
// it onto the puppet's synced view basis. The two view spaces match by construction: the
// origin is the head bone plus the same lift on both sides, and the basis is control yaw and
// pitch with roll 0.

#pragma once

namespace coop { class RemotePlayer; }

namespace coop::hand_item_pose {

// Measure the owner's natively welded held actor in its own view space: the position relative to the
// head anchor and the rotation, both in the view frame. False means do not ship (the mesh or bone not
// readable yet, or a wild pre-weld transform); the caller's periodic re-check retries. Game thread.
bool MeasureLocalHoldRelative(void* local, void* held, float outPos[3], float outRot[3]);

// Re-command a mirror to the puppet's view-space hold point: the roll-0 view basis from the synced aim,
// with the owner-measured transform re-composed onto it. Game thread, per tick.
void DriveMirror(void* mirrorActor, coop::RemotePlayer* pup, const float relPos[3], const float relRot[3]);

}  // namespace coop::hand_item_pose
