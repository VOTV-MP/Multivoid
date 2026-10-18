// coop/net/identity_key_file.cpp -- see coop/net/identity_key_file.h for WHY.

#include "identity_key_file.h"

#include "identity_crypto.h"
#include "coop/mod_data/legacy_note.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>   // SetNamedSecurityInfoW

#include <cwchar>
#include <vector>

namespace coop::net::identity_key_file {

namespace {

using identity_crypto::FromHex;
using identity_crypto::KeyFileAcl;
using identity_crypto::PrivateAcl;
using identity_crypto::Sha256;
using identity_crypto::ToHex;

const char* kKeyFileName = "multivoid_identity.key";

std::wstring KeyFileName() {
    std::wstring n;
    for (const char* c = kKeyFileName; *c; ++c) n.push_back(static_cast<wchar_t>(*c));
    return n;
}

std::wstring FolderOf(const std::wstring& path) {
    const size_t sep = path.find_last_of(L"\\/");
    return sep == std::wstring::npos ? path : path.substr(0, sep);
}

std::wstring LeafOf(const std::wstring& path) {
    const size_t sep = path.find_last_of(L"\\/");
    return sep == std::wstring::npos ? path : path.substr(sep + 1);
}

// HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid through the 64-bit registry view, as ASCII. Empty
// when it cannot be read (or is not ASCII, which a GUID never is).
std::string ReadMachineGuid() {
    HKEY k = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", 0,
                        KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS)
        return {};
    wchar_t buf[128] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t);  // the last wchar stays 0
    DWORD type = 0;
    const LONG rc = ::RegQueryValueExW(k, L"MachineGuid", nullptr, &type,
                                       reinterpret_cast<LPBYTE>(buf), &size);
    ::RegCloseKey(k);
    if (rc != ERROR_SUCCESS || type != REG_SZ) return {};
    std::string out;
    for (size_t i = 0; i < size / sizeof(wchar_t) && buf[i] != L'\0'; ++i) {
        if (buf[i] > 0x7F) return {};
        out.push_back(static_cast<char>(buf[i]));
    }
    return out;
}

// Sets the private list on the file; reports a volume that keeps none. False says only that the
// list is not on the file: the caller keeps the file regardless, since an identity that does not
// persist is the worse outcome.
bool ApplyKeyFileAcl(const std::wstring& path) {
    const PrivateAcl& acl = KeyFileAcl();
    if (!acl.ok) {
        UE_LOGW("peer_identity: could not build the key file's access list (this account's SID "
                "or the system's was unavailable) -- %ls keeps its folder's inherited list",
                path.c_str());
        return false;
    }
    const DWORD rc = ::SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr,
        acl.dacl, nullptr);
    if (rc == ERROR_SUCCESS) return true;
    if (rc == ERROR_NOT_SUPPORTED || rc == ERROR_INVALID_FUNCTION) {
        UE_LOGW("peer_identity: the volume holding %ls keeps no access lists (FAT or exFAT), so "
                "every account on this PC can read the key file; an NTFS volume protects it",
                path.c_str());
        return false;
    }
    UE_LOGW("peer_identity: could not set the key file's access list on %ls (error %lu) -- it "
            "keeps its folder's inherited list", path.c_str(), static_cast<unsigned long>(rc));
    return false;
}

// Renames `path` to `<path>.foreign-<UTC yyyymmddThhmmssmmmZ>`, `-<n>` appended while that name is
// taken. Returns the new file's name (no folder), or empty with `errorOut` set when the rename failed:
// the key stays where it was, so a failed set-aside never leaves two keys.
std::wstring SetAside(const std::wstring& path, unsigned long* errorOut) {
    SYSTEMTIME t{};
    ::GetSystemTime(&t);
    wchar_t stamp[32];
    std::swprintf(stamp, 32, L"%04u%02u%02uT%02u%02u%02u%03uZ", static_cast<unsigned>(t.wYear),
                  static_cast<unsigned>(t.wMonth), static_cast<unsigned>(t.wDay),
                  static_cast<unsigned>(t.wHour), static_cast<unsigned>(t.wMinute),
                  static_cast<unsigned>(t.wSecond), static_cast<unsigned>(t.wMilliseconds));
    const std::wstring base = path + L".foreign-" + stamp;
    *errorOut = ERROR_ALREADY_EXISTS;
    for (unsigned n = 0; n < 1000; ++n) {
        const std::wstring to = n == 0 ? base : base + L"-" + std::to_wstring(n);
        if (::MoveFileExW(path.c_str(), to.c_str(), 0)) return LeafOf(to);
        const DWORD e = ::GetLastError();
        if (e != ERROR_ALREADY_EXISTS && e != ERROR_FILE_EXISTS) {
            *errorOut = e;
            return {};
        }
    }
    return {};
}

// A key left beside the executable by a build from before the key moved into the mod's folder is read
// by nothing now; say so once, so a player who looks for it finds the answer.
void NoteLegacy() {
    const std::vector<uint8_t>& sid = KeyFileAcl().userSid;
    const std::wstring names[2] = {KeyFileName(), AccountKeyFileName(sid.data(), sid.size())};
    for (const std::wstring& name : names) {
        if (name.empty()) continue;
        coop::legacy_note::Once(coop::legacy_note::LegacyFile::IdentityKey, name.c_str());
    }
}

}  // namespace

std::wstring KeyFilePath() {
    const std::wstring dir = ue_wrap::paths::ModDataDir();
    if (dir.empty()) return {};
    return dir + L"\\" + KeyFileName();
}

std::wstring AccountKeyFileName(const uint8_t* sid, size_t n) {
    if (sid == nullptr || n == 0) return {};
    uint8_t digest[32];
    if (!Sha256(sid, n, digest)) return {};
    std::wstring name = L"multivoid_identity_";
    for (char c : ToHex(digest, 8)) name.push_back(static_cast<wchar_t>(c));
    return name + L".key";
}

std::wstring AccountKeyFilePath() {
    const std::wstring dir = ue_wrap::paths::ModDataDir();
    const std::vector<uint8_t>& sid = KeyFileAcl().userSid;
    if (dir.empty()) return {};
    const std::wstring name = AccountKeyFileName(sid.data(), sid.size());
    if (name.empty()) return {};
    return dir + L"\\" + name;
}

ReadResult ParseKeyText(const std::string& text, uint8_t priv[kPrivKeyBytes], std::string* boundOut) {
    if (boundOut) boundOut->clear();
    auto trimmed = [](std::string s) {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
            s.pop_back();
        return s;
    };
    ReadResult result = ReadResult::Malformed;
    bool haveKey = false, haveBound = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (!haveKey && line.compare(0, eq, "key") == 0) {
            haveKey = true;
            result = FromHex(trimmed(line.substr(eq + 1)), priv, kPrivKeyBytes) ? ReadResult::Loaded
                                                                                 : ReadResult::Malformed;
        } else if (!haveBound && line.compare(0, eq, "bound") == 0) {
            haveBound = true;
            if (boundOut) *boundOut = trimmed(line.substr(eq + 1));
        }
    }
    return result;
}

Binding JudgeBinding(const std::string& fileBound, const std::string& want) {
    if (want == "none") return Binding::MachineUnknown;
    if (fileBound == want) return Binding::Match;
    if (fileBound == "none") return Binding::Rebind;
    return Binding::Foreign;
}

ReadResult ReadKeyFile(const std::wstring& path, uint8_t priv[kPrivKeyBytes], std::string* boundOut,
                       unsigned long* errorOut) {
    if (errorOut) *errorOut = 0;
    if (boundOut) boundOut->clear();
    if (path.empty()) return ReadResult::Missing;
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = ::GetLastError();
        if (errorOut) *errorOut = e;
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return ReadResult::Missing;
        if (e == ERROR_ACCESS_DENIED) return ReadResult::Denied;
        return ReadResult::Unreadable;
    }
    char buf[4096];
    DWORD n = 0;
    const bool readOk = ::ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr) != 0;
    if (!readOk && errorOut) *errorOut = ::GetLastError();
    ::CloseHandle(h);
    if (!readOk) return ReadResult::Unreadable;
    return ParseKeyText(std::string(buf, n), priv, boundOut);
}

bool WriteKeyFile(const std::wstring& path, const uint8_t priv[kPrivKeyBytes], const std::string& bound,
                  unsigned long* errorOut, coop::atomic_file::Mode mode) {
    if (errorOut) *errorOut = 0;
    if (path.empty()) return false;
    std::string text =
        "# Multivoid durable player identity -- KEEP THIS FILE SECRET.\n"
        "# Anyone who has this key can play as you: it is what proves your identity\n"
        "# to every host you join, and it is what your stored inventory is named by.\n"
        "# The bound= line ties it to this PC: a copy on another PC is set aside there\n"
        "# and that PC starts a new identity. Never paste it into a bug report, a\n"
        "# screenshot or a Discord message.\n"
        "key=";
    text += ToHex(priv, kPrivKeyBytes);
    text += "\nbound=" + bound + "\n";
    SECURITY_DESCRIPTOR sd{};
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    const PrivateAcl& acl = KeyFileAcl();
    if (acl.ok && ::InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) &&
        ::SetSecurityDescriptorDacl(&sd, TRUE, acl.dacl, FALSE)) {
        ::SetSecurityDescriptorControl(&sd, SE_DACL_PROTECTED, SE_DACL_PROTECTED);
        sa.lpSecurityDescriptor = &sd;
    }
    const coop::atomic_file::Result r = coop::atomic_file::Write(
        path, text, mode, coop::atomic_file::Sync::ToDisk,
        sa.lpSecurityDescriptor ? &sa : nullptr);
    if (errorOut) *errorOut = r.error;
    return r.ok();
}

Found Find(uint8_t priv[kPrivKeyBytes]) {
    NoteLegacy();
    Found f;
    f.bindValue = identity_crypto::BindingValue(ReadMachineGuid());
    if (f.bindValue == "none")
        UE_LOGW("identity: this machine's id (MachineGuid) could not be read -- keys are used and "
                "written unbound until it reads");
    const std::wstring keyPath = KeyFilePath();
    if (keyPath.empty()) {
        UE_LOGW("peer_identity: the mod has no data folder this session (see the paths line above) -- "
                "a TEMPORARY identity, nothing is written");
        f.minted = true;
        f.temporary = true;
        return f;
    }
    const std::wstring account = AccountKeyFilePath();
    unsigned long readErr = 0;
    std::string fileBound;
    // A key that was read passes the binding or is set aside. True: use it. False: `f` already says
    // what follows instead (a new key written to `mintTarget`, or a temporary identity when the
    // set-aside failed, since a second key beside the first is the one outcome never allowed).
    auto accepted = [&](const std::wstring& path, const std::wstring& mintTarget) {
        switch (JudgeBinding(fileBound, f.bindValue)) {
        case Binding::Match:
        case Binding::MachineUnknown:
            return true;
        case Binding::Rebind:
            f.rebind = true;
            return true;
        case Binding::Foreign:
            break;
        }
        unsigned long err = 0;
        const std::wstring aside = SetAside(path, &err);
        if (aside.empty()) {
            UE_LOGW("identity: a key from another machine is in %ls but could not be set aside (error "
                    "%lu) -- a TEMPORARY identity for this session, the file left untouched",
                    path.c_str(), err);
            f.minted = true;
            f.temporary = true;
            return false;
        }
        UE_LOGW("identity: a key from another machine was set aside as %ls -- a new identity is made "
                "here", aside.c_str());
        f.minted = true;
        f.target = mintTarget;
        return false;
    };
    // This account's own file: used when it loads; an unreadable one ends TEMPORARY with nothing
    // written (never mint over a durable key a transient error hid); a malformed or missing one
    // gives way to a new key written to `mintTarget` (over it only when `mintTarget` is this file).
    auto readAccountKey = [&](const std::wstring& mintTarget) {
        switch (ReadKeyFile(account, priv, &fileBound, &readErr)) {
        case ReadResult::Loaded:
            if (accepted(account, mintTarget)) f.loadedFrom = account;
            break;
        case ReadResult::Unreadable:
        case ReadResult::Denied:
            UE_LOGW("peer_identity: could not read this account's key file %ls (error %lu) -- "
                    "a TEMPORARY identity for this session, the file left untouched",
                    account.c_str(), readErr);
            f.minted = true;
            f.temporary = true;
            break;
        case ReadResult::Malformed:
            UE_LOGW("peer_identity: %ls is not a key file this build can read -- minting a new "
                    "identity into %ls", account.c_str(), mintTarget.c_str());
            f.minted = true;
            f.target = mintTarget;
            f.replaceOnMint = mintTarget == account;
            break;
        case ReadResult::Missing:
            f.minted = true;
            f.target = mintTarget;
            break;
        }
    };
    switch (ReadKeyFile(keyPath, priv, &fileBound, &readErr)) {
    case ReadResult::Loaded:
        if (accepted(keyPath, keyPath)) f.loadedFrom = keyPath;
        break;
    case ReadResult::Denied:
        // Another account's file: this account keeps its own key beside it.
        if (account.empty()) {
            UE_LOGW("peer_identity: the mod folder's key file (%ls) belongs to another account and "
                    "this account's own key file cannot be named -- a TEMPORARY identity for this "
                    "session", keyPath.c_str());
            f.minted = true;
            f.temporary = true;
            break;
        }
        UE_LOGI("peer_identity: the mod folder's key file (%ls) belongs to another account on this "
                "PC -- using this account's own key beside it (%ls)",
                keyPath.c_str(), account.c_str());
        readAccountKey(account);
        break;
    case ReadResult::Unreadable:
        UE_LOGW("peer_identity: could not read the key file %ls (error %lu; a scanner holding it, "
                "or a device error) -- a TEMPORARY identity for this session, the file left "
                "untouched: your stored inventory will be found again once it reads",
                keyPath.c_str(), readErr);
        f.minted = true;
        f.temporary = true;
        break;
    case ReadResult::Malformed:
        UE_LOGW("peer_identity: %ls is not a key file this build can read -- minting a new "
                "identity over it", keyPath.c_str());
        f.minted = true;
        f.target = keyPath;
        f.replaceOnMint = true;
        break;
    case ReadResult::Missing:
        // No folder key: an account whose owner deleted it keeps the identity it already has.
        readAccountKey(keyPath);
        break;
    }
    return f;
}

void Conclude(const Found& f, const uint8_t priv[kPrivKeyBytes], const std::string& guid,
              const std::string& dial) {
    if (f.minted && f.temporary) {
        // The read said why; this names the identity the session runs on, with the dial half a
        // joiner would need, so the host's log is complete even on the bad day.
        UE_LOGW("peer_identity: TEMPORARY identity %s for this session (the key file could "
                "not be read, see above) -- dial=%s", guid.c_str(), dial.c_str());
    } else if (f.minted) {
        unsigned long err = 0;
        const coop::atomic_file::Mode mode = f.replaceOnMint ? coop::atomic_file::Mode::Replace
                                                             : coop::atomic_file::Mode::CreateOnly;
        if (!f.target.empty() && WriteKeyFile(f.target, priv, f.bindValue, &err, mode)) {
            ApplyKeyFileAcl(f.target);
            UE_LOGI("identity: minted %s in %ls -- dial=%s", guid.c_str(),
                    FolderOf(f.target).c_str(), dial.c_str());
        } else {
            // Same shape as the retired player_guid's unreadable-ini path: the session still
            // works, the identity just does not survive a restart, and saying so is the
            // difference between a puzzle and a known state. MTA refuses to start when its
            // server-id.keys cannot be read or created
            // (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:758-762); a client of a
            // single-player game still plays, on a temporary identity.
            if (!f.target.empty() && !f.replaceOnMint &&
                (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS)) {
                // The create-only write found the file: the one failure that is a race, not a
                // folder that refuses a write.
                UE_LOGW("peer_identity: minted identity %s but another copy of the game minted "
                        "the key first (%ls); this session runs on a TEMPORARY identity, the next "
                        "launch loads that key", guid.c_str(), f.target.c_str());
            } else {
                UE_LOGW("peer_identity: minted identity %s but could NOT write %ls (error %lu) "
                        "-- this identity is TEMPORARY and your stored inventory will not be "
                        "found again next launch", guid.c_str(),
                        f.target.empty() ? L"the key file (no folder resolved)" : f.target.c_str(),
                        err);
            }
        }
    } else {
        if (f.rebind) {
            // The file said `none` (written while this machine's id could not be read) and the id
            // reads now: the same key is written back with the real binding, through the identity's
            // own writer. A failure leaves the file as it was and the key in use.
            unsigned long err = 0;
            if (WriteKeyFile(f.loadedFrom, priv, f.bindValue, &err, coop::atomic_file::Mode::Replace))
                UE_LOGI("identity: bound the key in %ls to this machine", f.loadedFrom.c_str());
            else
                UE_LOGW("identity: could not bind the key in %ls to this machine (error %lu) -- it "
                        "is used as it is", f.loadedFrom.c_str(), err);
        }
        // Re-applied at every load, so a file an older build wrote, or one copied here from
        // another PC, is tightened at the next boot.
        ApplyKeyFileAcl(f.loadedFrom);
        // The `dial=` half is the value a joiner needs when there is no master in the loop
        // (net.host_identity), so it is printed on the ordinary path and not only on the mint.
        UE_LOGI("identity: loaded %s from %ls -- dial=%s", guid.c_str(),
                FolderOf(f.loadedFrom).c_str(), dial.c_str());
    }
}

}  // namespace coop::net::identity_key_file
