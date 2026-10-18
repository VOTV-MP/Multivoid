// coop/player/player_profile_store.h -- where the HOST keeps each player's profile blob: the newest
// one in memory, and a copy on disk that is cut only when the host's world is saved.
//
// A profile and the world are two halves of one state. An item a player pockets leaves the world
// and enters the profile, so a disk profile NEWER than the saved world holds that item twice after
// a host restart, and an OLDER one loses what the player dropped. So a profile goes to disk as it
// stood when the saved world was GATHERED, and at no other moment: not on a disconnect, not on
// shutdown, not on a timer. A host that quits without saving reverts its world to the last save,
// and the profiles revert with it. docs/players.md has the whole account.
// MTA saves an account when it changed and 15 s have passed, and at quit (CAccountManager.cpp):
// its world is not persisted, so its accounts have nothing to stay consistent WITH; ours has.
// The file is <mod data dir>/coop_players/<save slot>/<guid>.json, keyed per save slot. The blob is
// opaque here (coop/items/inventory_wire owns its layout); the file wraps it in a magic, an FNV
// hash, a nick and a last-seen time. A GUID that is not exactly 32 hex characters never becomes a
// path component. Game thread: the memory half has no lock.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::player_profile_store {

// A nick as it may stand inside the file's JSON string field: UTF-8, capped, with the two JSON
// metacharacters dropped.
std::string NickForJson(const std::wstring& nick);

// Keep `blob` as the newest profile of `guid`. False when it is byte-identical to the one held,
// which costs nothing at the next cut. Held for as long as the hosted world lives -- past the
// player's disconnect and past the last player's, so a rejoin gets what they left with and a save
// made after they left still cuts their last state. Loading another world to host
// (save_transfer::SetHostSlot) drops what is held: that world is as old as its save.
bool Put(const std::string& guid, std::vector<uint8_t> blob, const std::string& nickJson);

// The newest profile of `guid`: the one held in memory, else the file (FNV-verified, falling back
// to the .bak of the last good file). A corrupt file never yields unverified bytes, and it is NOT
// the same answer as no file: Absent is the first-join test, Unreadable is a returning player
// whose profile is still on disk.
enum class Found { Yes, Absent, Unreadable };
Found Get(const std::string& guid, std::vector<uint8_t>& outBlob);

// This player's stored profile did not read, or was written by a build this one cannot parse.
// Nothing of theirs is held or written for the rest of this hosted world, so the files stay as
// they are for recovery: a starter kit streamed back must not overwrite them.
void Quarantine(const std::string& guid);

// At a host session start: notes once (coop/mod_data/legacy_note) that a `coop_players` folder an
// older build left beside the exe is read by nothing now. The store's own first path comes only at
// a new-game ForgetSlot, a world save or a join, so the notice cannot wait for it. Any thread.
void NoteLegacyFolder();

// A save was just CREATED under `slot`. Its name was free, which the name of a deleted save is as
// well, and the profile files such a save left behind belong to no world: they are removed, so a
// returning player is a first joiner of the new world and not the owner of items it never held.
// Returns how many files went. Pure file work, any thread.
size_t ForgetSlot(const std::wstring& slot);

// Is any profile held that a gather or a save would have to do something with?
bool AnythingPending();

// The game gathered the world into its save object: set aside every profile that changed since
// the last gather, as the copy the next save writes. Not always the moment of a save: during an
// event the game writes saves WITHOUT gathering (ue_wrap/engine/save_capture.h), and the world in
// such a file is the last gather's, so the profiles beside it must be too. Not closed: a profile
// reaches the host up to a second after the pickup it reports (the client's 1 Hz poll), so a save
// inside that second cuts a profile one pickup behind its world.
void MarkWorldGathered();

// The host's world save was written to `writtenSlot`: bring the set beside that file if it stood
// under another slot, then write what the last gather set aside (atomically, through
// coop/atomic_file, keeping the previous file as .bak). Returns how many were written. A failed write
// stays for the next cut; a profile that is on disk and unchanged since is let go of, because Get
// gives it back from its file. One world is written under more than one name: a quicksave writes
// a new <main>_SUB_<n> file, and a plain save made after loading a subsave writes the main slot.
// So a cut to another slot first makes that slot's directory a copy of the set -- every player's
// file, held or not, and nothing of what it held before -- and the set stands there from then on.
// Nothing is removed until every copy has landed, and with no slot known nothing is removed.
size_t CutToDisk(const std::wstring& writtenSlot);

}  // namespace coop::player_profile_store
