// ue_wrap/actors/container_inventory.h -- a world container (prop_container_C) and the inventoryComponent_C that
// holds its contents: the component's canvasInventory, a TArray<Fstruct_inventoryItem> on the component itself, its
// volume fields, and the game's own verb that re-derives what the container shows from them. A nested container's
// contents travel inside its own entry's actorData (prop_container's betterGetData serialises its component there),
// so no entry names another container's storage. Engine-wrapper layer (principle 7): no wire and no arbitration,
// which coop/props/container_contents_sync owns. Every reflected offset is resolved once and cached; -1 means looked
// and failed, never retried, never guessed. Game thread.
#pragma once

#include "ue_wrap/actors/inventory_item.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ue_wrap::container_inventory {

using Item = ue_wrap::inventory_item::Item;

// Whether `actor` is a container, and `obj` an inventory component. A class not loaded yet, or still loading,
// answers false and is asked for again at the next call.
bool IsContainer(void* actor);
bool IsInventory(void* obj);

// The inventory component of a container actor, live, or null.
void* InventoryOf(void* container);
// The actor that owns an inventory component (its outer), live, or null.
void* OwnerOf(void* inventory);

// Fail closed: true only for the inventory of a WORLD container, a prop_container that is not the legacy
// player-inventory container. A player's own pockets are mainPlayer's inventoryComponent_player, and a drone's or an
// ATV's component belongs to no container, so each reads as false.
bool IsWorldInventory(void* inventory);

// The live canvasInventory TArray header of this inventory (the array at +0 of the returned address), or null when
// the field does not resolve.
uint8_t* ContentsSlot(void* inventory);

// The entries the inventory holds, read whole, blanks included (a removed entry is blanked in place until the game
// trims it). False when the array or the item layout does not resolve, or when it holds more than `maxItems`
// (nothing is read then: a cut read is a silent lie); `countOut`, when given, is the array's count, or -1 when it
// does not resolve.
bool ReadContents(void* inventory, std::vector<Item>& out, size_t maxItems, int32_t* countOut = nullptr);

// Whether an entry's class is a container.
bool IsContainerItem(const Item& it);

// What a put into `container` is judged on, as addItemAdvanced reads it: the volume room, whether the inventory is
// infinite, whether the container is a mailbox, whether it is locked (playerUsedOn returns early on it).
struct PutTerms {
    float maxVolume;
    float totalVolume;
    bool infinite;
    bool ownerIsMailbox;
    bool locked;
};
// False when `container` has no live inventory or a field does not resolve; `out` is then zeroed.
bool ReadPutTerms(void* container, PutTerms& out);
// The same terms read off an inventory component whose owner is no container (a drone's cargo): never locked, never a
// mailbox.
bool ReadInventoryPutTerms(void* inventory, PutTerms& out);

// The inventory's own filing of an entry, as a drag onto its canvas runs it: addItemAdvanced(data, setPositionAndRotation
// = true) with the entry's own position and rotation. True when the inventory took the entry (the volume fit and the class
// is valid); false when it refused, or the verb does not resolve or the call fails. The source of `data` is the caller's.
bool AddItemAdvanced(void* inventory, const Item& data);

// The inventory's own put of a world actor, as a player's put and a vacuum's gather run it: addItemAsObject(object),
// which files the actor's betterGetData and destroys the actor when it took it. True when it took the actor; false when
// it refused, or the verb does not resolve or the call fails.
bool AddItemAsObject(void* inventory, void* actor);

// Replace the inventory's entries with `items`: a fresh array is built and installed, the old one orphaned (freeing its
// strings recursively is the riskier act), a screen showing the inventory is rebuilt from the new entries (its widgets are
// indexed like the array), else the volumes are re-derived. Written, NotReady (the array or the entry layout does not
// resolve yet: the caller waits) or Refused (no allocator, or an allocation or an entry failed: nothing is installed).
enum class WriteResult : uint8_t { Written, NotReady, Refused };
WriteResult WriteContents(void* inventory, const std::vector<Item>& items);

// Re-derive what the container shows -- totalVolume, the open canvas's volume labels -- through the game's own
// calculateVolumes(countSlotBar_A=false, updateVisible=true, recalculateMaxVolume=true), the call removeInventoryItem
// ends with; never by raw writes. A verb that does not resolve is said once.
void RederiveShownState(void* inventory);

// A screen shows an inventory's items as widgets on its activeDisplayCanvas, indexed like canvasInventory, and a raw
// write of the array leaves them showing the old entries. DetachDisplay takes the widgets off the canvas (the
// canvas's own despawncanvas, which first writes each widget's place back into its entry) and returns the canvas, null
// when none is shown; after the write, AttachDisplay shows the entries again through the component's own
// assignCanvas, which ends in the volume re-derivation.
void* DetachDisplay(void* inventory);
void AttachDisplay(void* inventory, void* canvas);

// The inventory's totalVolume. False when the field does not resolve.
bool CurrentVolume(void* inventory, float& out);

// The inventory's maxVolume, the capacity addItemAdvanced's volume check reads. False when the field does not resolve.
bool MaxVolume(void* inventory, float& out);
// Dev: a raw write of maxVolume, for the intent drill's full-pocket arm; the caller writes the old value back. False
// when the field does not resolve.
bool SetMaxVolume(void* inventory, float value);

// Whether the container's inventory component has begun play: its `owner` field, set by makeOwner at BeginPlay and
// never cleared, is live. False when the component or the field does not resolve.
bool InventoryInitialized(void* container);

// Whether the gamemode's allContainers field resolves, which AllContainers cannot say: it reads an
// empty registry and an unresolved field both as zero entries.
bool AllContainersResolved(void* gamemode);

// The gamemode's allContainers registry, null and non-live entries skipped: copies up to `cap` actor
// pointers into `out` and returns the count copied. Zero when the gamemode or the field does not resolve.
size_t AllContainers(void* gamemode, void** out, size_t cap);

}  // namespace ue_wrap::container_inventory
