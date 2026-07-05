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

#include "coop/player/players_registry.h"  // kMaxPeers

namespace coop::net { class Session; }

namespace coop::nameplate {

// One projected label (plain data; the render thread reads it). nick is a fixed
// ASCII buffer (peer nicks are sanitized to ASCII upstream in player_handshake).
struct Plate {
    float x = 0.f;           // screen px (viewport pixels, top-left origin) -- the head anchor
    float y = 0.f;
    float alpha = 0.f;       // distance fade 0..1 (0 => skip)
    float scale = 1.f;       // distance SIZE scale: 1 = base size up close, shrinks ~1/dist far away
    bool  onScreen = false;  // projected in FRONT of the camera AND within fade range
    bool  occluded = false;  // world geometry between the camera and the head -> the plate
                             // renders GRAY (minecraft nametag shape; user 2026-07-04)
    bool  flash = false;     // hurt-flash red (read from RemotePlayer::IsHurtFlashing)
    int   healthPct = 100;   // 0..100 streamed-vitals health (display-only)
    int   ping = -1;         // RTT ms (-1 = unmeasured -> no suffix; 0 = sub-ms LAN -> "<1ms")
    uint8_t voiceIcon = 0;   // v66: coop::voice_chat::VoiceIcon badge right of the plate (0 = none)
    uint32_t colorRGB = 0;   // v103 (12f): packed custom nick color (coop::nick_color; 0 = white)
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

// ---- per-player visibility pref (v94; user 2026-07-02: any peer can hide its
// OWN plate, synced so every peer -- including late joiners -- agrees) ----
// The VISIBILITY AXIS is owned HERE (the nameplate domain); the wire layer
// (player_handshake) parses Join/PlayerJoined/NameplateChange and stores into
// this module, exactly like skins store into RemotePlayer::ApplySkin. The whole
// store is ATOMIC (any-thread): writers span the game thread (wire handlers),
// the render thread (the F1 checkbox request path) and the bringup thread
// (session-start reset via player_handshake::Reset on the TimelineThread).

// Boot-time init from votv-coop.ini nameplate= (harness, before the pump ticks).
void SetInitialLocalVisible(bool visible);

// The local pref. Any thread (atomic) -- the F1 checkbox reads it.
bool LocalVisible();

// UI entry (render thread): persist to votv-coop.ini, apply locally, announce
// to the session (host: broadcast; client: to host for rebroadcast).
void RequestLocalVisible(bool visible);

// Wire store: peer `slot` announced its pref. Update() skips hidden slots from
// the next snapshot on. Any thread (atomic slot flags).
void StoreVisibleForSlot(int slot, bool visible);
bool VisibleForSlot(int slot);

// Session wiring + lifecycle edges (called by subsystems / player_handshake's
// reset paths -- the session-start reset runs on the BRINGUP thread by design).
// Disconnect resets the slot to VISIBLE so a reused slot never inherits the
// departed peer's pref.
void Install(coop::net::Session* session);
void ResetSlots();
void OnSlotDisconnected(int slot);

}  // namespace coop::nameplate
