// coop/dev/object_overlay.h -- dev 3D-projected world-object debug labels, in layers. The
// "what IS this object" diagnostic: every nearby world object gets a screen-space label projected
// at its world position, so a misbehaving actor -- a white-cube mirror, a falling wall, a
// local-only prop that never syncs -- is identified with certainty instead of guessed at.
//   1. Object names -- the class leaf plus the Aprop_C list_props Name row, the visual identity,
//      and 'cube' against 'cubicleP_0' is THE white-cube discriminator.
//   2. Net identity -- the ElementId in hex, matching the logs, wire-mirror against local-owned,
//      and the key suffix. A label with NO eid is an UNTRACKED local-only actor, exactly the kind
//      of object that breaks cross-peer mirroring.
//   3. Physics state -- live simulation against at-rest, plus the static, frozen and sleep save
//      flags, which is the falling-walls discriminator.
//   4. Health -- live hp for creatures (any class-chain float 'health', and 'maxHealth' when
//      present) and for props (comp_physicsImpact: the live pool against damageData.health). With
//      this layer on, the candidate walk also admits ACharacter-lineage actors -- creatures and
//      puppets, the local player skipped -- so damage testing reads directly off the screen.

#pragma once

#include <cstdint>

namespace coop::dev::object_overlay {

// A master checkbox, a checkbox per layer and the radius live in the F1 menu, under Player > HUD;
// `[dev] object_overlay=1` force-enables at boot, so an autonomous run can exercise it without
// anything being clicked.
//
// The same split as coop::nameplate and ui::hud: THIS module is the GAME-THREAD half. Update(),
// called from the harness pumps beside nameplate::Update, rebuilds a cached candidate set every ~2
// s -- the element registry's Prop and Npc entries plus a GUObjectArray walk for untracked
// prop-lineage actors -- then re-projects the capped, distance-sorted set each tick and publishes a
// plain-data snapshot. ui::hud copies the snapshot and draws it on the render thread.
//
// A dev tool: it defaults OFF, and while off the per-tick cost is one atomic load. The refresh walk
// and its per-candidate location reads have the ue_wrap/prop.h FindNearest cost profile, which is
// acceptable at a 2 s cadence behind an explicit toggle and never on by default. Nothing here
// crosses the wire.

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
    char    line4[24] = {}; // health layer ("hp 42/100"; live, rebuilt per projection)
    float   healthFrac = -1.f;  // cur/max 0..1 for the color ramp; -1 = no max known
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
bool  LayerHealth(); void SetLayerHealth(bool on);
float RadiusM();     void SetRadiusM(float meters);

}  // namespace coop::dev::object_overlay
