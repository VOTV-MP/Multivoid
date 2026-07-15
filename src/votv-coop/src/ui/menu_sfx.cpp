// ui/menu_sfx.cpp -- see ui/menu_sfx.h.

#include "ui/menu_sfx.h"

#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

namespace ui::menu_sfx {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace prof = ue_wrap::profile;

namespace {

// Hover-enter edge bookkeeping (main/render thread; the browser draws single-threaded).
ImGuiID g_frameHoveredId = 0;   // a wrapped button was hovered THIS frame (its id)
ImGuiID g_lastRollId     = 0;   // id we last chirped rollover for (0 = re-armed)

// Play a VOTV SoundWave (resolved by name) as a 2D UI sound through the game's audio
// engine. Posted to the game thread (UFunction dispatch is GT-only). The world context
// is the live ui_menu_C -- the browser is a main-menu-only surface, so it is always
// present + carries the menu world (with an audio device). PlaySoundAtLocation with a
// null attenuation plays 2D; routing through UGameplayStatics means the SoundWave's
// SoundClass + the game's SoundMix / volume apply (honors the game's sound settings).
void PlayNamed(const wchar_t* soundName) {
    GT::Post([soundName] {
        void* worldCtx = R::FindObjectByClass(prof::name::UiMenuClass);
        if (!worldCtx) return;
        void* sound = R::FindObject(soundName, L"SoundWave");
        if (!sound) return;
        E::PlaySoundAtLocation(worldCtx, sound, ue_wrap::FVector{0.f, 0.f, 0.f},
                               /*attenuation*/ nullptr, /*volume*/ 1.f, /*pitch*/ 1.f);
    });
}

}  // namespace

void PlayClick()    { PlayNamed(L"buttonclick"); }
void PlayRollover() { PlayNamed(L"buttonrollover"); }

void FrameBegin() { g_frameHoveredId = 0; }
void FrameEnd()   { if (g_frameHoveredId == 0) g_lastRollId = 0; }  // left all buttons -> re-arm

bool Button(const char* label, const ImVec2& size) {
    const ImGuiID id = ImGui::GetID(label);
    const bool clicked = ImGui::Button(label, size);
    if (ImGui::IsItemHovered()) {
        g_frameHoveredId = id;
        if (id != g_lastRollId) { g_lastRollId = id; PlayRollover(); }  // hover-enter edge
    }
    if (clicked) PlayClick();
    return clicked;
}

}  // namespace ui::menu_sfx
