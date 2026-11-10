// ue_wrap/actors/nametag.h -- a prop's nametag and the nametag item that writes it. Engine-wrapper layer: field reads
// and writes and the game's own verb, no network logic.
//
// The item, prop_nametag_C, carries `tag` (an FName). Its playerHandUse_LMB, a left click with the item in hand, reads
// the player's interaction hit; if that is a prop it writes the tag into the prop's `nametag` (prop_C, an FName), runs
// the prop's setNametag() -- which takes down the label component it had and builds one for a nametag that is not
// None -- and destroys the item. A prop's nametag is base save state: Aprop_C::getData packs it beside the name.
// Game thread.

#pragma once

#include <string>

namespace ue_wrap::nametag {

// Whether `actor` is a nametag item (prop_nametag_C or below). By the class chain's names.
bool IsNametagItem(void* actor);

// The item's `tag`. False when `item` is not a nametag item or the field does not resolve.
bool ReadItemTag(void* item, std::wstring& out);

// The prop's `nametag`. False when `prop` is not a prop or the field does not resolve. "None" when it has none.
bool ReadPropNametag(void* prop, std::wstring& out);

// Write `tag` into the prop's `nametag` and run its setNametag(), as the item's own use does: the label it shows is
// rebuilt from the new value, and an empty tag or "None" takes it down. False when the field or the verb does not
// resolve, the name cannot be made, or the call fails.
bool ApplyPropNametag(void* prop, const std::wstring& tag);

}  // namespace ue_wrap::nametag
