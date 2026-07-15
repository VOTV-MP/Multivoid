// ui/menu_sfx.h -- VOTV native menu button sounds for our ImGui surfaces.
//
// Plays VOTV's own menu SoundWaves through the GAME's audio engine
// (UGameplayStatics::PlaySoundAtLocation, 2D) so they honor the game's sound
// settings (the SoundWave's SoundClass + the active SoundMix / volume) exactly like
// the native menu buttons -- NOT through any custom audio path. Two sounds:
//   - buttonclick     : on click / press
//   - buttonrollover  : on hover-enter (the VOTV "rollover" chirp)
//
// The native main-menu UButtons play these via their FButtonStyle (Slate). ImGui
// buttons have no native audio, so this supplies the same two sounds for our server
// browser (and any future ImGui menu surface). Safe to call from the ImGui render
// thread -- the UFunction dispatch is posted to the game thread internally.
#pragma once

#include "imgui.h"

namespace ui::menu_sfx {

// Fire the sounds directly (game-thread dispatch handled internally).
void PlayClick();
void PlayRollover();

// Drop-in replacement for ImGui::Button that plays buttonrollover on hover-enter and
// buttonclick on click -- so an ImGui button behaves like a native VOTV menu button.
// Wrap a surface's buttons between FrameBegin()/FrameEnd() so a continuous hover only
// chirps once (the enter edge), re-arming when the pointer leaves all wrapped buttons.
bool Button(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));
void FrameBegin();
void FrameEnd();

}  // namespace ui::menu_sfx
