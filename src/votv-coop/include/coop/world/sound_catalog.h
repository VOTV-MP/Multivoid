// coop/world/sound_catalog.h -- the sounds the relay (coop/world/sound_relay) may carry, as a table every peer builds the
// same way, so the wire names a sound by its place in the table and never by a path.
//
// An entry is every distinct sound the game's `list_physSound` table names in its impact column (what a melee hit plays
// for the struck material), plus the vent knocker's knock and the church's bell. The entries are sorted by path, so the
// order is the same on every peer whatever order the game's table stores its rows in, and a peer's table is identified
// by a digest of the paths (FNV-1a over the sorted paths joined by newlines): a receiver whose own digest differs drops
// the sound. Each entry carries what the game does with it: the callers that play it, its attenuation, and the volume and
// pitch range its callers pass (read from the three Blueprint calls), so the host judges a client's ask by the index alone
// and a sound is never taken from a value the sender wrote.
// MTA's sound id (playSoundFrontEnd's 7-bit id) and Source's sound precache index are the same idea: an index into a
// table both sides hold. Game thread.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace coop::sound_catalog {

// The Blueprint callers a sound may come from, as bits.
enum Caller : uint8_t {
    kAttack = 1,   // mainPlayer_C::attack: the struck material's impact sound, from the player's own machine
    kKnock  = 2,   // kocker_C::knock: the vent knocker, an actor of the host's world
    kBell   = 4,   // church_C's bell, an actor of the host's world
};

struct Entry {
    std::wstring   path;         // the sound's object path (ue_wrap::object_path::Of)
    uint8_t        callers;      // the Caller bits allowed to play it
    const wchar_t* attenuation;  // the attenuation asset's path, or nullptr: none
    float          volumeMin, volumeMax;
    float          pitchMin, pitchMax;
};

// Builds the table when it is not built, and says whether it is. A table that cannot be read yet (the game's own is not
// loaded, or its reflection does not resolve) is tried again by the next call, at most every 3 s: the walk of the engine's
// objects is dear, and the cause is said once. A table whose layout or data is not what the reader expects (a member
// missing, the walk and the engine's own column disagreeing, a sound named twice) is said once and not read again: no
// retry changes it, and this peer relays no sound.
bool Ready();

// The table's digest and size; 0 while it is not built.
uint32_t Digest();
size_t   Size();

// The 1-based index of the sound at `path`, or 0 when the table holds none (or is not built).
uint16_t IndexOf(const std::wstring& path);

// The entry at the 1-based `index`; nullptr for 0, past the table, or while it is not built.
const Entry* Find(uint16_t index);

// The first index whose entry allows `caller`, in table order; 0 when none (or not built).
uint16_t FirstFor(uint8_t caller);

// What a peer's SoundAt names, checked against this table.
enum class Resolve : uint8_t {
    Ok,        // `entry` is the sound
    NotReady,  // this peer's table is not built
    Digest,    // the sender's table is not this one
    Index,     // the index names no entry
};
Resolve Lookup(uint32_t senderDigest, uint16_t index, const Entry*& entry);

// `volume` and `pitch` held to the entry's range; a value that is not a number reads as the range's low end.
void Clamp(const Entry& entry, float& volume, float& pitch);

}  // namespace coop::sound_catalog
