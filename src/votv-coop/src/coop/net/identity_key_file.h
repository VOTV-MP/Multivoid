// coop/net/identity_key_file.h -- the durable identity's key FILE: where it lives, how it is found,
// read, written and protected, what binds it to a machine, and what happens when it is missing,
// malformed, another account's or another machine's. What the key MEANS (public key, id, signing) is
// peer_identity's; this unit hands it the 32-byte seed and says whether it was read or must be minted.
//
// Boot thread only (the one load); the pieces the selftest drives are stateless.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "coop/atomic_file/atomic_file.h"
#include "coop/net/peer_identity.h"

namespace coop::net::identity_key_file {

// Where the key lives: in the mod's own data folder (ue_wrap::paths::ModDataDir), not beside the game
// executable -- so a game folder copied to another PC carries no identity, and a manager's reinstall
// starts a clean one. It is created under a private access list (this account and the system,
// nothing inherited), re-applied at every load, so another account on the same PC cannot read it.
// An account that cannot read that folder's file -- another account holds it -- keeps its own key
// beside it (`multivoid_identity_<account>.key`), so two accounts stay two players; while the folder
// has no file of its own, an account that has such a file reads it, and one that has none mints the
// folder's. A folder that refuses the write, or no folder at all, leaves the session on a temporary
// identity, as it leaves every file the mod writes. Never in the ini: inis get pasted into bug reports.
using peer_identity::kPrivKeyBytes;

// What a read found. Denied is the case the fallback exists for: the file is there and this
// account may not read it, which is another account's file. Malformed is a file that is not a
// key (a truncated or edited one); it gives way to a new key (written over it, except an account's
// own file while the folder has none, whose new key goes to the folder's path), and the log says
// so. Unreadable is any other failure to open or read a file that may well be there (a sharing
// violation from a scanner holding it, a device error): nothing is written over it, and this
// session runs on a temporary identity, because minting over a durable key that a transient error
// hid is the one loss this module exists to prevent.
enum class ReadResult { Loaded, Missing, Denied, Malformed, Unreadable };

// The file carries `bound=`: a fingerprint of the machine it was made on (identity_crypto
// BindingValue over MachineGuid). A key whose `bound=` is another machine's, malformed or absent is
// set aside as `<file>.foreign-<UTC stamp>` and a new key is minted; a key written while the machine id
// could not be read says `none` and is re-bound in place once it reads. A machine id that cannot be
// read leaves every key as it is. A cloned Windows image copies the id with the files, so it is not
// told apart; the remedy is deleting the key. The binding guards against copies, not against a player
// who deletes the key to become someone new.
//
// What the binding says about a key that was read: Match (bound to this machine), MachineUnknown (this
// machine's id could not be read: the key is used as it is), Rebind (the file says `none` and the id
// reads now: the file is re-bound in place, the key kept) or Foreign (another machine's, malformed or
// absent: set aside).
enum class Binding { Match, MachineUnknown, Rebind, Foreign };

// The mod folder's key file. Empty when the mod has no data folder.
std::wstring KeyFilePath();

// This account's key for a folder whose own key file another account holds: beside it, named by
// a hash of this account's SID. Empty when nothing resolves.
std::wstring AccountKeyFilePath();

// The file name of an account's own key: a hash of the account's SID bytes and nothing else, so two
// accounts on one install stay two players. Empty when the SID is empty or the hash fails.
std::wstring AccountKeyFileName(const uint8_t* sid, size_t n);

// The file's text to its key and its `bound=` value: the first `key=` line decides Loaded or
// Malformed (the seed lands in `priv`), the first `bound=` line is `boundOut` (empty when there is
// none). Pure.
ReadResult ParseKeyText(const std::string& text, uint8_t priv[kPrivKeyBytes], std::string* boundOut);

// The binding's verdict. `fileBound` is the file's `bound=` value ("" = no such line), `want` this
// machine's BindingValue (`none` = its id could not be read). Pure.
Binding JudgeBinding(const std::string& fileBound, const std::string& want);

// Reads `path` and parses it. `errorOut` carries the OS error of a failed open or read.
ReadResult ReadKeyFile(const std::wstring& path, uint8_t priv[kPrivKeyBytes], std::string* boundOut,
                       unsigned long* errorOut);

// Writes the key and its `bound=` value under the private list, which rides the create of the
// temporary and stays with the file through the move, so the file never exists with a wider one, not
// even for the instant between a create and a set. `errorOut` carries the OS error on failure, so the
// caller can tell a folder that refuses a write from anything else. CreateOnly fails with
// ERROR_ALREADY_EXISTS when the file is there (a second copy of the game minted first): the caller
// keeps its own key TEMPORARY and the next launch loads the file's.
bool WriteKeyFile(const std::wstring& path, const uint8_t priv[kPrivKeyBytes], const std::string& bound,
                  unsigned long* errorOut, coop::atomic_file::Mode mode);

// Where the key came from, or where a new one goes.
struct Found {
    std::wstring loadedFrom;  // the file the key was read from
    std::wstring target;      // where a minted key is written; empty = temporary
    bool minted = false;      // no key was read: the caller fills `priv` with fresh randomness
    // True only when the mint's target IS the file that read Malformed: only then may the write
    // replace; every other mint writes a target that read Missing (or was set aside), and
    // create-only keeps a key a second copy of the game minted a moment earlier.
    bool replaceOnMint = false;
    bool temporary = false;   // a file that may exist could not be read: nothing is written
    bool rebind = false;      // the loaded file said `none` and this machine's id reads: re-bind it
    std::string bindValue;    // this machine's BindingValue: what a written key says
};

// The folder's file, then the account's fallback: reads the seed into `priv` when a file holds
// one that passes the binding, else says where a new one goes. Names a key left beside the
// executable by an older build. Logs why whenever the answer is not the plain case.
Found Find(uint8_t priv[kPrivKeyBytes]);

// The end of a load, once the caller has the key's id and dial string: writes a minted key where
// Find said, re-binds a loaded one that asked, and logs the outcome (minted, loaded, or temporary
// and why).
void Conclude(const Found& found, const uint8_t priv[kPrivKeyBytes], const std::string& guid,
              const std::string& dial);

}  // namespace coop::net::identity_key_file
