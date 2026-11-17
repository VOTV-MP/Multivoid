// ue_wrap/desk/desk_panels.h -- engine access for the desk's BACK PANELS, the covers over its physical
// module slots. Principle-7 engine-wrapper layer: no network logic; coop::desk_panel_sync drives the
// mirror through here.
//
// AanalogDScreenTest_C keeps two TArray<bool> beside its `panels` components, one element per panel:
// panelsState (the panel is on the desk) and panelsUnscrew (its bolts are out). A wrench or power tool
// held on a panel toggles its panelsUnscrew (unscrewPanel); E on an unbolted panel clears its
// panelsState and hands the player a prop_computerBackPanel_C subclass; that prop carried back into its
// lid sets panelsState again and is destroyed (ReceiveActorBeginOverlap). panelTakeOff is raised by the
// take-off and holds the overlap's re-attach off until the panel prop has left every lid. updPanels()
// is a pure function of the two arrays -- each panel's visibility, collision and tilt -- so a mirror
// may call it. The save carries both arrays.

#pragma once

#include <cstdint>

namespace ue_wrap::desk_panels {

inline constexpr int kPanels = 4;

// One bit per panel, bit i for element i of the desk's arrays.
struct Masks {
    uint8_t on = 0;
    uint8_t unscrewed = 0;
    bool operator==(const Masks& o) const { return on == o.on && unscrewed == o.unscrewed; }
    bool operator!=(const Masks& o) const { return !(*this == o); }
};

// Resolve the two arrays, panelTakeOff and updPanels on the desk class, and the panel prop's base
// class. Piggybacks console_desk::EnsureResolved for the desk. Throttled lazy retry; idempotent; game
// thread.
bool EnsureResolved();

// The live arrays as masks. False when unresolved, with no desk, or when an array is shorter than
// kPanels.
bool Read(Masks& out);

// Write the first kPanels elements of both arrays in place. False as Read; never grows an array.
bool Write(const Masks& in);

// The desk's panelTakeOff. False when unresolved or with no desk.
bool WriteTakeOff(bool on);

// Reflected desk updPanels(): every panel's look and collision from the arrays.
bool CallUpdPanels();

// Is `cls` prop_computerBackPanel_C or a descendant, the prop a taken-off panel becomes?
bool IsPanelClass(void* cls);

// The prop class panel `panel` becomes when it is taken off (its place's own subclass), and the panel
// a prop class belongs to. Null and -1 when unresolved or for no panel's class.
void* ClassForPanel(int panel);
int PanelForClass(void* cls);

}  // namespace ue_wrap::desk_panels
