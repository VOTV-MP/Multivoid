// ui/net_stats_panel.h -- the real-time session NETWORK STATS overlay (host + clients).
//
// A compact, passive top-right panel: live receive/send rates (GNS wire-level, ~1 s
// window), the session's total bytes down/up, packet rates, peer count + worst ping,
// and a 60 s rate sparkline. Data = coop::net::net_stats (the one counter owner --
// Session's GNS choke points + its ~1 Hz real-time sample); this file only draws.
// MTA precedent: CNetworkStats (reference/mtasa-blue/Client/mods/deathmatch/logic/) --
// the same ping/packets/bytes rows; the sparkline + auto-units are ours.
//
// OFF by default. Toggled from F1 > Network > Stats (all players -- not a dev tool);
// persisted as votv-coop.ini ui.netstats (the ui.scale/ui.font pref shape). Render
// thread only, no input capture (the chat-window flag set), no per-frame engine reads.

#pragma once

namespace ui::net_stats_panel {

// The persisted pref (lazy ini read on first call). Render-thread safe.
bool Enabled();
void SetEnabled(bool on);  // + persist votv-coop.ini ui.netstats

// The passive overlay window (render thread, inside the ImGui frame). No-op unless
// Enabled(). Driven from imgui_overlay beside ui::hud::Render.
void Render();

// The F1 menu page (Network > Stats): the enable checkbox + a live readout of the
// same numbers (works as a preview while the overlay itself is off).
void RenderMenuPref();

}  // namespace ui::net_stats_panel
