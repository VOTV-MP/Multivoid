// coop/net/identity_crypto.h -- the primitives the durable identity is built from, shared by the
// key's meaning (peer_identity) and the key's file (identity_key_file): SHA-256 through the Windows
// CNG provider, lower-case hex, the key file's private access list, and the sweep of the selftest's
// scratch keys. Pure Win32, no engine and no state beyond the access list built once per process;
// any thread.
//
// Co-located with the two units that use it: nothing else builds an identity's bytes or its file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct _ACL;

namespace coop::net::identity_crypto {

// SHA-256 via the Windows CNG provider. False only when the provider refused.
bool Sha256(const void* data, size_t len, uint8_t out[32]);

// `n` bytes as 2n lower-case hex chars; and the inverse, false unless `hex` is exactly 2n hex
// digits (either case).
std::string ToHex(const uint8_t* p, size_t n);
bool FromHex(const std::string& hex, uint8_t* out, size_t n);

// The binding of a key file to a machine: SHA-256 over the UTF-8 bytes of the tag
// "multivoid-identity-binding-v1" plus a newline, then the machine id lower-cased (ASCII), as 64
// lower-case hex; `none` when the machine id is empty (unreadable) or the hash fails. A pure
// function of its argument: the caller reads the machine id.
std::string BindingValue(const std::string& machineGuid);

// The key file's own access list: this account and the system, full control, nothing inherited.
// Built once per process from the process token; `ok` is false when the OS refused, and a file
// then keeps its folder's inherited list. `userSid` is this account's SID bytes, empty when the
// token could not be read.
struct PrivateAcl {
    std::vector<uint8_t> userSid;
    _ACL*                dacl = nullptr;  // allocated once, kept for the process
    bool                 ok = false;
};
const PrivateAcl& KeyFileAcl();

// The access list of `path` as read back: protected, and its entry count. False when unreadable.
bool ReadKeyDacl(const std::wstring& path, bool& isProtected, unsigned long& aceCount);

// The selftest's scratch keys: `<kScratchKeyPrefix><pid>.key` in the mod's data folder. A run killed
// inside the selftest leaves its file (and a temporary of it); the sweep removes every one whose
// process is gone.
inline constexpr const wchar_t* kScratchKeyPrefix = L"multivoid.selftest-identity.";
void SweepDeadScratchKeys(const std::wstring& exeDir);

}  // namespace coop::net::identity_crypto
