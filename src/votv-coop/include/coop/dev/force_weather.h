// coop/dev/force_weather.h -- F5 dev-key: toggle SNOW on host (broadcasts to client).
//
// Snow was chosen as the unambiguous weather visual signal per the 2026-05-27
// session: rain particles were too subtle to verify cross-peer, red sky color
// curves were ambiguous in some lighting. intComs_triggerSnow fans out to 53 BP
// listeners (snow particles + ground accumulation + sky tint), making it
// trivially visible on screen.
//
// HOST-ONLY hotkey. F5 toggles the local g_snowOn flag and calls
// coop::weather_sync::DebugForceSnow(g_snowOn) on the game thread. The host's
// POST observer on intComs_triggerSnow catches the call + broadcasts a
// WeatherState packet; the client's ApplyFromHost drives the same UFunction
// locally for the BP fan-out.
//
// Gated by votv-coop.ini ([dev] devkeys=1 + [dev] enabled!=0); same gates as
// coop::dev::restore_vitals / coop::dev::teleport_client. Foreground-window check prevents
// a single keypress from triggering both processes on a same-box LAN test.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::force_weather {

// Cache the Session pointer so the hotkey can verify host role. Called once
// from harness boot, BEFORE Init(). Mirrors restore_vitals::SetSession.
void SetSession(coop::net::Session* session);

// Read votv-coop.ini; if [dev] devkeys=1 (and master not killed), start the
// F5 hotkey thread. No-op otherwise. Idempotent.
void Init();

}  // namespace coop::dev::force_weather
