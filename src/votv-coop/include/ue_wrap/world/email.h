// ue_wrap/email.h -- standalone engine access for the meadow-PC email
// pipeline: the saveSlot.emails array (watermark side) and the
// gamemode.addEmail apply (one reflected call = persistence append + list
// row + the email ding at the physical laptop + tab highlight). Principle-7
// engine-wrapper layer -- NO network logic; coop::email_sync drives the
// mirror through here.
//
// RE (votv-computers-phase2-impl-RE-2026-06-12.md SS3): every producer
// (daynightCycle task mails, drone sell responses, console/desk status
// mails) funnels through gamemode.addEmail -- EX_LocalVirtualFunction =
// PE-invisible, so detection is the array watermark. addEmail RE-STAMPS the
// date from the (host-synced) clock, so dates converge without shipping
// them. Emails are NOT append-only: ui_laptop.delEmail(Index) is the
// player's delete (removes the list slot widget AND the saveSlot row in
// one); coop::email_sync mirrors it content-keyed via the RowKey/DelEmail
// surface below. The pfp is a stored UTexture2D pointer
// (list_emailCharacters is NOT part of the runtime pipeline); ship its leaf
// name + FindObject-or-null on the receiver (a null pfp renders an empty
// brush, no crash).

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::email {

// One email row, string-ified for the wire.
struct Row {
    uint8_t      username = 0;   // TEnumAsByte<enum_emailChars> (identical enum on every peer)
    std::wstring pfpLeaf;        // the pfp texture's leaf object name ("" = null pfp)
    std::wstring topic;          // FText -> string
    std::wstring text;           // FText -> string
};

// Per-row INSTANCE key: POD bytes read raw off the array row -- zero
// reflected calls, so coop::email_sync can re-key the whole array at cadence
// for free. Stable for the row's lifetime: rows only ever move by byte copy
// (TArray growth realloc / Array_Remove tail shift), which preserves the
// FText inner data pointers, the pfp pointer, the username byte, and the
// date stamped once by addEmail. NOT a cross-peer identity (pointers are
// per-process) -- cross-peer identity is the serialized-blob hash that
// email_sync stores alongside.
struct RowKey {
    uint64_t topicData = 0;  // FText topic's first 8 bytes (the ITextData ref)
    uint64_t textData  = 0;  // FText text's first 8 bytes
    uint64_t pfp       = 0;  // UTexture2D* value
    int32_t  dateX     = 0;  // FIntVector.X of the addEmail date stamp
    uint8_t  username  = 0;

    bool operator==(const RowKey& o) const {
        return topicData == o.topicData && textData == o.textData &&
               pfp == o.pfp && dateX == o.dateX && username == o.username;
    }
};

// Resolve mainGamemode + saveSlot.emails + addEmail (throttled lazy retry).
// Game thread.
bool EnsureResolved();

// The current saveSlot.emails element count (-1 if unresolved / no world).
int32_t EmailCount();

// Read row `index` of saveSlot.emails into `out` (FTexts read via the
// KismetTextLibrary bridge). False on bounds/unresolved.
bool ReadRow(int32_t index, Row& out);

// Read row `index`'s POD instance key (raw memory only -- no reflected
// calls). False on bounds/unresolved.
bool ReadRowKey(int32_t index, RowKey& out);

// Reflected ui_laptop.delEmail(Index): the native player-delete path
// (removes the uicomp_emailSlot list widget AND the saveSlot.emails row at
// the same index). The two arrays cannot diverge: every append flows
// through ui_laptop.addEmail (which appends both in one body), loads
// rebuild both (updEmails), and SP players delete history mails through
// this same function -- our reflected call IS the button path. False on
// bounds / unresolved laptop. Game thread.
bool DelEmail(int32_t index);

// Apply one wire row: build the Fstruct_email param (new=true, date zeroed --
// addEmail re-stamps it from the synced clock, FTexts minted from strings,
// pfp resolved by leaf name or null) and dispatch the reflected
// gamemode.addEmail. Reproduces persistence + list + ding + tab highlight in
// one call. Game thread.
bool AddEmail(const Row& row);

}  // namespace ue_wrap::email
