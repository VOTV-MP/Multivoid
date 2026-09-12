// coop/net/peer_identity.cpp -- see coop/net/peer_identity.h for WHY.

#include "coop/net/peer_identity.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingtypes.h>
#include <steam/isteamnetworkingsockets.h>
#pragma warning(pop)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>   // SetEntriesInAclW, SetNamedSecurityInfoW
#include <bcrypt.h>

#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

// Ed25519, from the donna translation unit GNS already compiles into the static library we link
// (`third_party/GameNetworkingSockets/src/CMakeLists.txt:73-74` builds
// `third_party/GameNetworkingSockets/src/external/ed25519-donna/ed25519_VALVE.c`;
// `crypto_25519_donna.cpp` is its other caller). Declared here rather than including the vendored
// header so this TU pulls in nothing from GNS's internal `src/` tree -- three prototypes with C
// linkage are the whole dependency.
extern "C" {
void ed25519_publickey(const unsigned char sk[32], unsigned char pk[32]);
void ed25519_sign(const unsigned char* m, size_t mlen, const unsigned char sk[32],
                  const unsigned char pk[32], unsigned char RS[64]);
int  ed25519_sign_open(const unsigned char* m, size_t mlen, const unsigned char pk[32],
                       const unsigned char RS[64]);
}

namespace coop::net::peer_identity {

namespace {

PubKey      g_pub{};
uint8_t     g_priv[kPrivKeyBytes]{};
std::string g_guid;
std::string g_identityString;
bool        g_loaded = false;

// --- primitives -------------------------------------------------------------

// SHA-256 via the Windows CNG provider. bcrypt is already linked (GNS's
// USE_CRYPTO=BCrypt backend), so this costs no new dependency and -- unlike
// GNS's own CCrypto -- needs no header from the vendored tree's internal `src/`.
bool Sha256(const void* data, size_t len, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return false;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = ::BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    if (ok) ok = ::BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)),
                                  static_cast<ULONG>(len), 0) == 0;
    if (ok) ok = ::BCryptFinishHash(hash, out, 32, 0) == 0;
    if (hash) ::BCryptDestroyHash(hash);
    ::BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

std::string ToHex(const uint8_t* p, size_t n) {
    static const char kHex[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(kHex[p[i] >> 4]);
        s.push_back(kHex[p[i] & 0xF]);
    }
    return s;
}

bool FromHex(const std::string& hex, uint8_t* out, size_t n) {
    if (hex.size() != n * 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; ++i) {
        const int hi = nib(hex[i * 2]), lo = nib(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

// --- the key file -----------------------------------------------------------
// Where the key lives: beside the game executable, with the install, where every build has kept
// it -- so an update changes nobody's identity, a Steam library move carries it, and a tester's
// two installs are two players. It is created under a private access list (this account and the
// system, nothing inherited), re-applied at every load, so another account on the same PC cannot
// read it. An account that cannot own the install's file -- it belongs to another account, or the
// install folder refuses the write -- keeps its own key for that install under its profile
// (%LOCALAPPDATA%\Multivoid\installs\<install id>\), the install id naming the executable's folder,
// so two installs stay two players there too. Never in the ini: inis get pasted into bug reports.

const char* kKeyFileName = "multivoid_identity.key";

std::wstring KeyFileName() {
    std::wstring n;
    for (const char* c = kKeyFileName; *c; ++c) n.push_back(static_cast<wchar_t>(*c));
    return n;
}

// The install's own file. Empty only when the executable's folder cannot be resolved.
std::wstring InstallKeyFilePath() {
    const std::wstring dir = ue_wrap::paths::ExeDir();
    if (dir.empty()) return {};
    return dir + L"\\" + KeyFileName();
}

// This account's file for this install, under the profile: the install id is the first eight
// bytes of SHA-256 over the executable folder's path, lower-cased, so a copy in another folder is
// another install and the same folder is the same one. Empty when nothing resolves; the folders
// are created only when the file is written.
std::wstring ProfileKeyFilePath() {
    std::wstring dir = ue_wrap::paths::ExeDir();
    const std::wstring base = ue_wrap::paths::ProfileDir();
    if (dir.empty() || base.empty()) return {};
    for (auto& c : dir) c = static_cast<wchar_t>(std::towlower(c));
    uint8_t digest[32];
    if (!Sha256(dir.data(), dir.size() * sizeof(wchar_t), digest)) return {};
    std::wstring id;
    for (char c : ToHex(digest, 8)) id.push_back(static_cast<wchar_t>(c));
    return base + L"\\installs\\" + id + L"\\" + KeyFileName();
}

// The parent folders of a profile key file, created on the way to the first write.
bool EnsureParentDirs(const std::wstring& path) {
    size_t pos = 0;
    const std::wstring base = ue_wrap::paths::ProfileDir();
    if (base.empty() || path.compare(0, base.size(), base) != 0) return false;
    pos = base.size();
    while ((pos = path.find(L'\\', pos + 1)) != std::wstring::npos) {
        const std::wstring dir = path.substr(0, pos);
        if (!::CreateDirectoryW(dir.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS)
            return false;
    }
    return true;
}

// The file's own access list: this account and the system, full control, nothing inherited, so
// another account on the PC cannot read the key even from a folder that would let it. Built once
// per process from the process token; not ok when the OS refused, and the file then keeps its
// folder's inherited list, which the log says.
struct PrivateAcl {
    std::vector<uint8_t> userSid;
    uint8_t              systemSid[SECURITY_MAX_SID_SIZE]{};
    PACL                 dacl = nullptr;  // allocated once, kept for the process
    bool                 ok = false;
};

const PrivateAcl& KeyFileAcl() {
    static const PrivateAcl acl = [] {
        PrivateAcl a;
        HANDLE tok = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tok)) return a;
        DWORD need = 0;
        ::GetTokenInformation(tok, TokenUser, nullptr, 0, &need);
        std::vector<uint8_t> buf(need ? need : 1);
        const bool gotUser =
            need != 0 && ::GetTokenInformation(tok, TokenUser, buf.data(), need, &need);
        ::CloseHandle(tok);
        if (!gotUser) return a;
        PSID user = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
        const auto* userBytes = static_cast<const uint8_t*>(user);
        a.userSid.assign(userBytes, userBytes + ::GetLengthSid(user));
        DWORD cb = sizeof(a.systemSid);
        if (!::CreateWellKnownSid(WinLocalSystemSid, nullptr, a.systemSid, &cb)) return a;
        EXPLICIT_ACCESS_W ea[2] = {};
        for (auto& e : ea) {
            e.grfAccessPermissions = FILE_ALL_ACCESS;
            e.grfAccessMode = SET_ACCESS;
            e.grfInheritance = NO_INHERITANCE;
            e.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        }
        ea[0].Trustee.TrusteeType = TRUSTEE_IS_USER;
        ea[0].Trustee.ptstrName = reinterpret_cast<LPWSTR>(a.userSid.data());
        ea[1].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ea[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(a.systemSid);
        if (::SetEntriesInAclW(2, ea, nullptr, &a.dacl) != ERROR_SUCCESS) {
            a.dacl = nullptr;
            return a;
        }
        a.ok = true;
        return a;
    }();
    return acl;
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

// What a read found. Denied is the case the fallback exists for: the file is there and this
// account may not read it, which is another account's file. Malformed is a file that is not a
// key (a truncated or edited one); it is minted over, and the log says so. Unreadable is any
// other failure to open or read a file that may well be there (a sharing violation from a scanner
// holding it, a device error): nothing is written over it, and this session runs on a temporary
// identity, because minting over a durable key that a transient error hid is the one loss this
// module exists to prevent.
enum class ReadResult { Loaded, Missing, Denied, Malformed, Unreadable };

ReadResult ReadKeyFile(const std::wstring& path, uint8_t priv[kPrivKeyBytes], DWORD* errorOut) {
    if (errorOut) *errorOut = 0;
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
    std::string text(buf, n);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (line.compare(0, eq, "key") != 0) continue;
        std::string hex = line.substr(eq + 1);
        while (!hex.empty() && (hex.back() == '\r' || hex.back() == '\n' ||
                                hex.back() == ' ' || hex.back() == '\t'))
            hex.pop_back();
        return FromHex(hex, priv, kPrivKeyBytes) ? ReadResult::Loaded : ReadResult::Malformed;
    }
    return ReadResult::Malformed;
}

// Writes the key under the private list, which rides the create so the file never exists with a
// wider one, not even for the instant between a create and a set. `errorOut` carries the OS
// error on failure, so the caller can tell a folder that refuses a write from anything else.
bool WriteKeyFile(const std::wstring& path, const uint8_t priv[kPrivKeyBytes], DWORD* errorOut) {
    if (errorOut) *errorOut = 0;
    if (path.empty()) return false;
    std::string text =
        "# Multivoid durable player identity -- KEEP THIS FILE SECRET.\n"
        "# Anyone who has this key can play as you: it is what proves your identity\n"
        "# to every host you join, and it is what your stored inventory is named by.\n"
        "# Copy it to another PC to take your identity with you; never paste it into\n"
        "# a bug report, a screenshot or a Discord message.\n"
        "key=";
    text += ToHex(priv, kPrivKeyBytes);
    text += "\n";
    SECURITY_DESCRIPTOR sd{};
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    const PrivateAcl& acl = KeyFileAcl();
    if (acl.ok && ::InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) &&
        ::SetSecurityDescriptorDacl(&sd, TRUE, acl.dacl, FALSE)) {
        ::SetSecurityDescriptorControl(&sd, SE_DACL_PROTECTED, SE_DACL_PROTECTED);
        sa.lpSecurityDescriptor = &sd;
    }
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0,
                             sa.lpSecurityDescriptor ? &sa : nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (errorOut) *errorOut = ::GetLastError();
        return false;
    }
    DWORD written = 0;
    const bool ok = ::WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written,
                                nullptr) && written == text.size();
    if (!ok && errorOut) *errorOut = ::GetLastError();
    ::CloseHandle(h);
    return ok;
}

}  // namespace

// --- public API -------------------------------------------------------------

bool RandomBytes(void* out, size_t len) {
    const NTSTATUS st = ::BCryptGenRandom(nullptr, static_cast<PUCHAR>(out),
                                          static_cast<ULONG>(len),
                                          BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return st == 0;  // STATUS_SUCCESS
}

bool Load() {
    if (g_loaded) return true;
    const std::wstring installPath = InstallKeyFilePath();
    std::wstring loadedFrom;   // the file the key was read from
    std::wstring target;       // where a minted key is written; empty = temporary
    bool minted = false;
    bool temporary = false;    // a file that may exist could not be read: nothing is written
    DWORD readErr = 0;
    switch (ReadKeyFile(installPath, g_priv, &readErr)) {
    case ReadResult::Loaded:
        loadedFrom = installPath;
        break;
    case ReadResult::Denied: {
        // Another account's file: this account keeps its own key for this install.
        const std::wstring mine = ProfileKeyFilePath();
        UE_LOGI("peer_identity: the install's key file (%ls) belongs to another account on this "
                "PC -- using this account's own key for this install (%ls)",
                installPath.c_str(), mine.c_str());
        const ReadResult r2 = ReadKeyFile(mine, g_priv, &readErr);
        if (r2 == ReadResult::Loaded) {
            loadedFrom = mine;
        } else if (r2 == ReadResult::Unreadable) {
            UE_LOGW("peer_identity: could not read this account's key file %ls (error %lu) -- "
                    "a TEMPORARY identity for this session, the file left untouched",
                    mine.c_str(), static_cast<unsigned long>(readErr));
            minted = true;
            temporary = true;
        } else {
            minted = true;
            target = mine;
        }
        break;
    }
    case ReadResult::Unreadable:
        UE_LOGW("peer_identity: could not read the key file %ls (error %lu; a scanner holding it, "
                "or a device error) -- a TEMPORARY identity for this session, the file left "
                "untouched: your stored inventory will be found again once it reads",
                installPath.c_str(), static_cast<unsigned long>(readErr));
        minted = true;
        temporary = true;
        break;
    case ReadResult::Malformed:
        UE_LOGW("peer_identity: %ls is not a key file this build can read -- minting a new "
                "identity over it", installPath.c_str());
        minted = true;
        target = installPath;
        break;
    case ReadResult::Missing:
        minted = true;
        target = installPath;
        break;
    }
    if (minted) {
        if (!RandomBytes(g_priv, kPrivKeyBytes)) {
            UE_LOGE("peer_identity: BCryptGenRandom failed -- no identity can be established");
            return false;
        }
    }
    ed25519_publickey(g_priv, g_pub.data());
    g_guid = GuidForPublicKey(g_pub);
    if (g_guid.empty()) {
        UE_LOGE("peer_identity: could not derive the guid from our own key");
        return false;
    }
    // The routing form, built HERE rather than at each use so the P2P announce, the dial and the
    // logs cannot drift from the bytes we sign with. Built by hand rather than through
    // SteamNetworkingIdentityRender because Load() runs at boot, long before GNS is initialised;
    // the format is fixed at `steamnetworkingsockets_shared.cpp:234-247` and ParseString
    // round-trips it at `:335-357`.
    g_identityString = "gen:" + ToHex(g_pub.data(), g_pub.size());
    if (minted && temporary) {
        // The read said why; this names the identity the session runs on, with the dial half a
        // joiner would need, so the host's log is complete even on the bad day.
        UE_LOGW("peer_identity: TEMPORARY identity %s for this session (the key file could "
                "not be read, see above) -- dial=%s", g_guid.c_str(), g_identityString.c_str());
    } else if (minted) {
        DWORD err = 0;
        if (target == installPath && !WriteKeyFile(target, g_priv, &err)) {
            // The install folder refused the write (an install under a folder this account may
            // only read): the key goes under the profile, for this install.
            const std::wstring mine = ProfileKeyFilePath();
            UE_LOGI("peer_identity: the install folder refused the key file (%ls, error %lu) -- "
                    "saving this account's key for this install under the profile (%ls)",
                    installPath.c_str(), static_cast<unsigned long>(err), mine.c_str());
            target = mine;
        }
        // The install's file was written above; a profile file is written here, its folders first.
        if (target != installPath) EnsureParentDirs(target);
        if (!target.empty() && (target == installPath || WriteKeyFile(target, g_priv, &err))) {
            ApplyKeyFileAcl(target);
            UE_LOGI("peer_identity: minted a new durable identity %s (saved to %ls) -- dial=%s",
                    g_guid.c_str(), target.c_str(), g_identityString.c_str());
        } else {
            // Same shape as the retired player_guid's unreadable-ini path: the session still
            // works, the identity just does not survive a restart, and saying so is the
            // difference between a puzzle and a known state.
            UE_LOGW("peer_identity: minted identity %s but could NOT write %ls (error %lu) -- "
                    "this identity is TEMPORARY and your stored inventory will not be found "
                    "again next launch", g_guid.c_str(),
                    target.empty() ? L"the key file (no folder resolved)" : target.c_str(),
                    static_cast<unsigned long>(err));
        }
    } else {
        // Re-applied at every load, so a file an older build wrote, or one copied here from
        // another PC, is tightened at the next boot.
        ApplyKeyFileAcl(loadedFrom);
        // The `dial=` half is the value a joiner needs when there is no master in the loop
        // (net.host_identity), so it is printed on the ordinary path and not only on the mint.
        UE_LOGI("peer_identity: loaded durable identity %s from %ls -- dial=%s",
                g_guid.c_str(), loadedFrom.c_str(), g_identityString.c_str());
    }
    g_loaded = true;
    return true;
}

const PubKey& LocalPublicKey() { return g_pub; }
const std::string& LocalIdentityString() { return g_identityString; }

bool PublicKeyFromIdentityString(const std::string& identity, PubKey& out) {
    // The prefix is checked rather than skipped: `<64 hex>` on its own is not an
    // identity this build ever renders, and accepting it would make the caller's
    // comparison depend on a spelling nothing produces.
    static constexpr char kPrefix[] = "gen:";
    static constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (identity.size() != kPrefixLen + kPubKeyBytes * 2) return false;
    if (identity.compare(0, kPrefixLen, kPrefix) != 0) return false;
    return FromHex(identity.substr(kPrefixLen), out.data(), out.size());
}

std::string GuidForPublicKey(const PubKey& pub) {
    uint8_t digest[32];
    if (!Sha256(pub.data(), pub.size(), digest)) return {};
    // 16 bytes -> 32 lowercase hex chars, which is EXACTLY the shape
    // `IsValidGuid` has always required, so every store keeps its format and the
    // value simply stops being something a peer can choose.
    return ToHex(digest, 16);
}

bool InstallInto(ISteamNetworkingSockets* sockets) {
    if (!sockets) return false;
    if (!g_loaded && !Load()) return false;
    // WHY THIS IS A ResetIdentity AND NOT A SetCertificate. `SetCertificate` on a SELF-ISSUED
    // UNSIGNED cert cannot work: `CertStore_CheckCert` returns at its first line for a cert with
    // no CA signature (`steamnetworkingsockets_certstore.cpp:603`, "No signature") and never
    // reaches `outMsgCert.ParseFromString`, so `InternalSetCertificate` reads an EMPTY message and
    // rejects it with "Cert has invalid public key" (`csteamnetworkingsockets.cpp:752-755`).
    // Signing it would need a trusted CA key in the store, which is the whole apparatus this
    // design removes.
    //
    // Nothing is lost by dropping it. GNS mints its own ephemeral session key and stamps our
    // identity into the cert it sends (`steamnetworkingsockets_connections.cpp:1303-1319`,
    // `SetLocalCertUnsigned`), so the remote's identity comparison still passes -- and the durable
    // key was never what GNS checked anyway: a cert's identity and its key are unbound (see the
    // header), which is why the admission challenge, not the certificate, is what proves who we
    // are.
    SteamNetworkingIdentity self;
    self.Clear();
    if (!self.SetGenericBytes(g_pub.data(), g_pub.size())) {
        UE_LOGE("peer_identity: SetGenericBytes refused a %d-byte key", kPubKeyBytes);
        return false;
    }
    sockets->ResetIdentity(&self);
    UE_LOGI("peer_identity: identity installed -- guid %s (GNS reports the connection "
            "'unauthenticated' by design: we run no certificate authority, and what binds "
            "the identity is the admission challenge)", g_guid.c_str());
    return true;
}

Sig SignBlob(const uint8_t* data, size_t len) {
    Sig sig{};
    ed25519_sign(data, len, g_priv, g_pub.data(), sig.data());
    return sig;
}

bool VerifyBlob(const PubKey& pub, const uint8_t* data, size_t len, const Sig& sig) {
    return ed25519_sign_open(data, len, pub.data(), sig.data()) == 0;
}

bool RunSelftest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("peer_identity selftest FAIL: %s", what);
    };

    // 1-3: SHA-256 against a published vector, because the guid every store is
    // keyed by is derived from it -- a silently wrong digest would rename every
    // player at once and read as "the inventory feature broke".
    {
        uint8_t d[32]{};
        const bool ok = Sha256("abc", 3, d);
        check(ok, "SHA-256 provider unavailable");
        check(ok && ToHex(d, 32) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "SHA-256(\"abc\") != the published digest");
        uint8_t d2[32]{};
        check(Sha256("", 0, d2) && ToHex(d2, 32) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              "SHA-256(\"\") != the published digest");
    }

    // 4-9: Ed25519 against RFC 8032 section 7.1 vectors 1 and 2 -- the primitive the
    // whole admission decision rests on. A tamper arm follows each, because a
    // verifier that accepts everything passes every positive test there is.
    struct Kat { const char* sk; const char* pk; const char* msg; const char* sig; };
    static const Kat kats[] = {
        { "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
          "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
          "",
          "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a"
          "33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
        { "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
          "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
          "72",
          "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15"
          "996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
    };
    for (const Kat& k : kats) {
        uint8_t sk[32]{}, pkExpect[32]{}, pkGot[32]{}, sigExpect[64]{}, sigGot[64]{};
        std::vector<uint8_t> msg(std::strlen(k.msg) / 2);
        const bool parsed = FromHex(k.sk, sk, 32) && FromHex(k.pk, pkExpect, 32) &&
                            FromHex(k.sig, sigExpect, 64) &&
                            (msg.empty() || FromHex(k.msg, msg.data(), msg.size()));
        check(parsed, "selftest vector failed to parse");
        if (!parsed) continue;
        ed25519_publickey(sk, pkGot);
        check(std::memcmp(pkGot, pkExpect, 32) == 0,
              "ed25519_publickey != the RFC 8032 vector");
        ed25519_sign(msg.data(), msg.size(), sk, pkGot, sigGot);
        check(std::memcmp(sigGot, sigExpect, 64) == 0,
              "ed25519_sign != the RFC 8032 vector");
        check(ed25519_sign_open(msg.data(), msg.size(), pkExpect, sigExpect) == 0,
              "ed25519_sign_open rejected a valid RFC 8032 signature");
        uint8_t tampered[64];
        std::memcpy(tampered, sigExpect, 64);
        tampered[0] ^= 0x01;
        check(ed25519_sign_open(msg.data(), msg.size(), pkExpect, tampered) != 0,
              "ed25519_sign_open ACCEPTED a tampered signature");
    }

    // 10-13: the decision this module actually exports -- sign with our own key, verify against the
    // identity bytes a receiver would read off a connection, and refuse both a flipped signature
    // and a different signer's key. That last one is the attack: GNS binds a cert's identity to
    // nothing, so a peer CAN claim a victim's key -- and this is the check that refuses it.
    if (g_loaded) {
        static const uint8_t kBlob[] = "multivoid-peer-identity-selftest";
        const Sig sig = SignBlob(kBlob, sizeof(kBlob) - 1);
        check(VerifyBlob(g_pub, kBlob, sizeof(kBlob) - 1, sig),
              "our own signature did not verify against our own identity");
        Sig bad = sig; bad[0] ^= 0x01;
        check(!VerifyBlob(g_pub, kBlob, sizeof(kBlob) - 1, bad),
              "a tampered signature verified against our own identity");
        uint8_t otherSk[32]{}; PubKey otherPk{};
        if (RandomBytes(otherSk, sizeof(otherSk))) {
            ed25519_publickey(otherSk, otherPk.data());
            check(!VerifyBlob(otherPk, kBlob, sizeof(kBlob) - 1, sig),
                  "a signature verified against SOMEONE ELSE'S identity");
        }
        check(GuidForPublicKey(g_pub) == g_guid && g_guid.size() == 32,
              "the derived guid is not stable / not 32 chars");

        // 14-16: the ROUTING form is the same value as the signing form. This is
        // asserted because the two are produced by different code -- our own hex
        // in Load(), GNS's in ToString() -- and a divergence would not fail
        // anything visibly: the joiner would simply dial an identity nobody has
        // registered, and the lobby would read as "P2P is down".
        SteamNetworkingIdentity parsed;
        parsed.Clear();
        check(!g_identityString.empty() && g_identityString.size() == 68,
              "the rendered identity is not 68 chars (`gen:` + 64 hex)");
        const bool round = parsed.ParseString(g_identityString.c_str());
        check(round, "GNS refused to parse our own rendered identity");
        check(round && parsed.m_eType == k_ESteamNetworkingIdentityType_GenericBytes &&
              parsed.m_cbSize == kPubKeyBytes &&
              std::memcmp(parsed.m_genericBytes, g_pub.data(), kPubKeyBytes) == 0,
              "our rendered identity does not parse back to our own public key");
    }

    if (pass == total) {
        UE_LOGI("peer_identity selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("peer_identity selftest: %d/%d checks passed", pass, total);
    return false;
}

}  // namespace coop::net::peer_identity
