// coop/nameplate.h -- floating nickname labels above remote players (ImGui screen-space).
//
// Gameplay/network layer (principle 7). The label used to be a 3D world-space
// UWidgetComponent the ENGINE rendered; it is now drawn by our OWN ImGui overlay
// as a screen-space PROJECTION -- the proper MTA nametag shape (project the head
// world point to the screen, fade with distance, draw centred outlined text),
// finally possible because we control an ImGui canvas (VOTV never runs the stock
// HUD canvas, which is why the old code had to fall back to a world widget).
//
// This module is the GAME-THREAD half: Update() projects each live remote puppet's
// head world point to viewport-pixel screen coords via the local player's
// PlayerController (engine::ProjectWorldToScreen -- a UFunction, so game thread)
// and publishes a thread-safe POD snapshot. The RENDER-THREAD half (ui::hud) copies
// the snapshot and draws nick + ping + health bar at each screen point. Same split
// as coop::roster -> ui::scoreboard (game-thread snapshot, render-thread draw).

#pragma once

#include "coop/players_registry.h"  // kMaxPeers

namespace coop::nameplate {

// One projected label (plain data; the render thread reads it). nick is a fixed
// ASCII buffer (peer nicks are sanitized to ASCII upstream in player_handshake).
struct Plate {
    float x = 0.f;           // screen px (viewport pixels, top-left origin) -- the head anchor
    float y = 0.f;
    float alpha = 0.f;       // distance fade 0..1 (0 => skip)
    float scale = 1.f;       // distance SIZE scale: 1 = base size up close, shrinks ~1/dist far away
    bool  onScreen = false;  // projected in FRONT of the camera AND within fade range
    bool  flash = false;     // hurt-flash red (read from RemotePlayer::IsHurtFlashing)
    int   healthPct = 100;   // 0..100 streamed-vitals health (display-only)
    int   ping = -1;         // RTT ms (-1 = unmeasured -> no suffix; 0 = sub-ms LAN -> "<1ms")
    uint8_t voiceIcon = 0;   // v66: coop::voice_chat::VoiceIcon badge right of the plate (0 = none)
    char  nick[24] = {};
};

struct Snapshot {
    int   count = 0;
    Plate plates[coop::players::kMaxPeers];
};

// GAME THREAD: project every live remote puppet -> a fresh snapshot, then publish.
// Cheap no-op when there are no puppets (early-out before the controller resolve) /
// no local player. Called UNCONDITIONALLY from the harness tick (~60 Hz) so the
// snapshot self-clears to empty at the menu (the HUD then auto-hides).
void Update();

// Copy the latest snapshot. Safe from ANY thread (the render thread reads it).
void GetSnapshot(Snapshot& out);

// True if there is at least one label to draw -- the overlay uses this (lock-free)
// to decide whether to run the always-on HUD pass. Any thread.
bool HasAny();

}  // namespace coop::nameplate
