// coop/save/save_button_disable.h -- grey out and disable the client's pause-menu Save button.
//
// The visible half of the guarantee in coop/save/save_block.cpp: persistence is host-only, so a
// client must not write the world save, and the block already stops the write at
// UGameplayStatics::SaveGameToSlot. This makes the restriction READ as one rather than as a silent
// failed save -- on the client's pause menu (ui_menu_C with isPause set) button_Save is disabled
// and dimmed with SetRenderOpacity. The native disableSave bool stays untouched on purpose: it
// also blocks OPENING the ESC menu, which would trap the client.
//
// Mechanism: a POST observer on mainPlayer_C::InpActEvt_Escape, the ProcessEvent-dispatched input
// event that opens the menu (enterPause is blueprint-internal and fires no observer), applies the
// disable on each open; a self-filtered POST observer on ui_menu_C::Tick re-applies it if the
// blueprint flips the button back on -- a raw bIsEnabled read, with a UFunction call only on the
// re-enable edge. Client-only. MTA shape: the client GUI is disabled for a host-authoritative
// action (CGUIElement::SetEnabled) while the host enforces the real rule.
#pragma once

namespace coop::net { class Session; }

namespace coop::save_button_disable {

// Install the client pause-menu Save-button grey-out. Idempotent; safe to call every tick
// from net_pump::InstallObservers (the idempotent retry hub). NO-OP on the host. On the
// client, resolves ui_menu_C / mainPlayer_C / the UWidget UFunctions and registers the two
// POST observers once; retries until the menu BP class is loaded.
void Install(coop::net::Session* session);

}  // namespace coop::save_button_disable
