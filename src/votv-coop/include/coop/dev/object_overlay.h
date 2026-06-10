// coop/dev/object_overlay.h -- dev 3D-projected world-object debug labels (layered).
//
// The "what IS this object" diagnostic: every nearby world object gets a
// screen-space label projected at its world position, so a misbehaving actor
// (a white-cube mirror, a falling wall, a local-only prop that never syncs)
// can be identified with certainty instead of guessed at. Layers:
//   1. Object names -- class leaf + the Aprop_C list_props Name row (the visual
//      identity; 'cube' vs 'cubicleP_0' is THE white-cube discriminator).
//   2. Net identity -- ElementId (hex, matches the logs), wire-mirror vs
//      local-owned, key suffix. A label with NO eid is an UNTRACKED local-only
//      actor -- exactly the objects that break cross-peer mirroring.
//   3. Physics state -- live sim vs at-rest + the static/frozen/sleep save flags
//      (the falling-walls discriminator).
// Master checkbox + per-layer checkboxes + radius live in the F1 menu
// (Player > HUD); `[dev] object_overlay=1` force-enables at boot so the
// autonomous smoke can exercise it without clicking.
//
// Same split as coop::nameplate -> ui::hud: THIS module is the GAME-THREAD half.
// Update() (called from the harness pumps next to nameplate::Update) rebuilds a
// cached candidate set every ~2 s -- element-registry Prop/Npc entries PLUS a
// GUObjectArray walk for UNTRACKED prop-lineage actors -- then re-projects the
// capped, distance-sorted set each tick and publishes a POD snapshot. ui::hud
// copies the snapshot and draws it (render thread).
//
// Dev tool: defaults OFF; when off the per-tick cost is one atomic load. The
// refresh walk + per-candidate location reads have the ue_wrap/prop.h
// FindNearest cost profile -- acceptable at the 2 s cadence behind an explicit
// dev toggle, never on by default. Nothing here crosses the wire.

#pragma once

#include <cstdint>

namespace coop::dev::object_overlay {

inline constexpr int kMaxLabels = 64;

// One projected label (plain data; the render thread reads it). Identity text is
// baked at refresh time (it is stable); position/alpha re-projected per tick.
struct Label {
    float   x = 0.f;        // screen px (viewport pixels, top-left origin) -- anchor
    float   y = 0.f;
    float   dist = 0.f;     // cm from camera (render side derives the size scale)
    float   alpha = 0.f;    // distance fade 0..1 (0 => skip)
    uint8_t kind = 2;       // 0 = wire mirror, 1 = tracked local-owned, 2 = UNTRACKED
    char    line1[56] = {}; // names layer ('\0' when the layer is off)
    char    line2[56] = {}; // net-identity layer
    char    line3[40] = {}; // physics layer
};

struct Snapshot {
    int   count = 0;
    Label labels[kMaxLabels];
    char  status[112] = {};  // one-line summary (top-right) -- proves the overlay is
                             // ON even when nothing is in range / no player yet
};

// Boot (harness Init, next to dev_menu::Init): `[dev] object_overlay=1` (under the
// MasterEnabled kill-switch) force-enables the overlay with the default layers.
void InitFromIni();

// GAME THREAD (harness pumps, next to nameplate::Update). Self-throttling:
// candidate refresh every ~2 s, projection every other tick (~30 Hz). Publishes
// an empty snapshot when disabled / no local player (the HUD then auto-hides).
void Update();

// Copy the latest snapshot. Safe from any thread (the render thread reads it).
void GetSnapshot(Snapshot& out);

// Master toggle state -- lock-free; ui::hud gates its draw (and the overlay's
// ImGui frame) on this, ui::dev_menu reflects it.
bool IsEnabled();
void SetEnabled(bool on);

// Layer toggles + radius (F1 menu, render thread; atomics -- a change marks the
// candidate cache dirty so the next Update rebuilds immediately).
bool  LayerNames();  void SetLayerNames(bool on);
bool  LayerNet();    void SetLayerNet(bool on);
bool  LayerPhys();   void SetLayerPhys(bool on);
float RadiusM();     void SetRadiusM(float meters);

}  // namespace coop::dev::object_overlay
