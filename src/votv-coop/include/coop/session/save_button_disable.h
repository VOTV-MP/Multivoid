// coop/save_button_disable.h -- grey out + disable the client pause-menu Save button.
//
// PR-FOUNDATION-2 (save-game safety) increment B PART 2: the honest UX complement to
// the hard guarantee in coop/save_block.cpp. Under host-only persistence (user decision
// 2026-05-30) a coop CLIENT must not write the world save; part 1 already blocks the
// write at the engine chokepoint (UGameplayStatics::SaveGameToSlot). This part makes the
// restriction VISIBLE instead of a silent failed-save: on the client's in-game pause
// menu (ui_menu_C with isPause==true), the "Save Game" button (button_Save) is disabled
// (blocks the click) and dimmed (SetRenderOpacity) so it reads as greyed-out. The rest of
// the pause menu stays fully usable -- we deliberately do NOT touch the native disableSave
// bool (it also blocks OPENING the ESC menu, which would trap the client).
//
// Mechanism (design + adversarial verify in workflow wf_72018941-ee0): a POST observer on
// the engine input event that opens the pause menu (mainPlayer_C::InpActEvt_Escape -- the
// SAME proven-ProcessEvent-dispatched class as the flashlight InpActEvt_* we already
// observe; ui_menu_C::enterPause is BP-internal/ProcessInternal and would NOT fire our
// observer -- the updateFlashlight trap) applies the disable each open, and a self-filtered
// POST observer on ui_menu_C::Tick re-applies if the BP ever flips the button back on
// (cheap raw bIsEnabled-bit read; UFunction call only on the re-enable edge). Client-only;
// the host installs nothing (its Save button stays fully functional -- it keeps the save).
//
// MTA shape: disable client GUI for a host-authoritative action (cf. CGUIElement::SetEnabled
// / guiSetEnabled, reference/mtasa-blue/Client/sdk/gui/CGUIElement.h:55) while the server
// (here: the host's save_block detour) enforces the real rule.
#pragma once

namespace coop::net { class Session; }

namespace coop::save_button_disable {

// Install the client pause-menu Save-button grey-out. Idempotent; safe to call every tick
// from net_pump::InstallObservers (the idempotent retry hub). NO-OP on the host. On the
// client, resolves ui_menu_C / mainPlayer_C / the UWidget UFunctions and registers the two
// POST observers once; retries until the menu BP class is loaded.
void Install(coop::net::Session* session);

}  // namespace coop::save_button_disable
