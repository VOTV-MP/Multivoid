// ue_wrap/devices/atv_parts.h -- engine access for the ATV's removable wheels: where its own look
// fields say the player is pointing, the wheel prop a tyre becomes, and the wrench's last step as a
// callable verb. Principle-7 engine-wrapper layer: no network or coop state;
// coop/interactables/atv_part_intent decides who may run what.
//
// ATV_C's getActionOptions writes lookAtTire (a fitted wheel, 0..3), lookAtTireSocket (an empty
// socket), lookAtSpareTireBox and lookAtContainer each time a player looks at it. unscrewPanel reads
// them: a looked-at wheel leaves as a prop_atvWheel_C carrying its durability, dirt and fixes. The
// battery's own part (ue_wrap/devices/atv's GetBattery, SpawnBattery) and the condition fields
// (ue_wrap/devices/atv_condition) are not here. Game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace ue_wrap::atv_parts {

// What the ATV's own look fields hold.
struct Look {
    int32_t tire = -1;      // lookAtTire
    int32_t socket = -1;    // lookAtTireSocket
    bool spareBox = false;  // lookAtSpareTireBox
    bool container = false; // lookAtContainer
};

// A wheel prop's three scalars, the ones a tyre of the ATV keeps per wheel.
struct Wheel {
    float   durability = 0.f;
    float   dirt = 0.f;
    int32_t fixes = 0;
};

// Resolve the look fields and the three verbs on ATV_C and the wheel class with its scalars.
// One attempt once ATV_C and the wheel class are both loaded (a lookup only, so a client's hook never blocks on a
// load); true only when everything resolved.
bool Resolve();

// The look fields of `atv`. False when unresolved or null.
bool ReadLook(void* atv, Look& out);

// Is `actor` a wheel prop?
bool IsWheel(void* actor);

// A wheel prop's scalars. False when unresolved or null.
bool ReadWheel(void* wheel, Wheel& out);

// The wrench's last step, unscrewPanel(getUnscrewSpawn()), run with the ATV looking at wheel `tire`
// (0..3). The look fields are put back to what they held before the call returns. False when a call
// did not dispatch.
bool RunUnscrew(void* atv, int tire);

// The ATV's own resetUnscrew: its progress widget, its timer and its held player let go.
bool ResetUnscrew(void* atv);

// A wheel prop spawned at `at` with its scalars written before its init pass. The wheel class is loaded here when this
// process has not (the host's refund), and Resolve is finished with it. Null when it does not resolve or
// the spawn failed.
void* SpawnWheel(const FVector& at, const Wheel& w);

}  // namespace ue_wrap::atv_parts
