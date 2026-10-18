// coop/net/peer_identity.cpp -- see coop/net/peer_identity.h for WHY.

#include "coop/net/peer_identity.h"

#include "identity_crypto.h"     // co-located: Sha256, hex, the key file's access list
#include "identity_key_file.h"   // co-located: where the key is found, read and written

#include "coop/atomic_file/atomic_file.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingtypes.h>
#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>  // SteamNetworkingIdentity::ParseString is inline HERE under
                                          // STEAMNETWORKINGSOCKETS_STATIC_LINK; without this header the
                                          // call links against a member the static library never defines.
#pragma warning(pop)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstring>
#include <mutex>
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
std::mutex  g_guidMu;  // takes Load()'s write and LocalGuid()'s read
std::string g_identityString;
bool        g_loaded = false;

using identity_crypto::FromHex;
using identity_crypto::KeyFileAcl;
using identity_crypto::kScratchKeyPrefix;
using identity_crypto::ReadKeyDacl;
using identity_crypto::Sha256;
using identity_crypto::SweepDeadScratchKeys;
using identity_crypto::ToHex;
using identity_key_file::AccountKeyFileName;
using identity_key_file::AccountKeyFilePath;
using identity_key_file::KeyFilePath;
using identity_key_file::ReadKeyFile;
using identity_key_file::ReadResult;
using identity_key_file::WriteKeyFile;

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
    // Where the seed comes from (or where a new one goes) is the key file's; what it means is ours.
    const identity_key_file::Found found = identity_key_file::Find(g_priv);
    if (found.minted) {
        if (!RandomBytes(g_priv, kPrivKeyBytes)) {
            UE_LOGE("peer_identity: BCryptGenRandom failed -- no identity can be established");
            return false;
        }
    }
    ed25519_publickey(g_priv, g_pub.data());
    {
        const std::string guid = GuidForPublicKey(g_pub);
        std::lock_guard<std::mutex> lk(g_guidMu);
        g_guid = guid;
    }
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
    identity_key_file::Conclude(found, g_priv, g_guid, g_identityString);
    g_loaded = true;
    return true;
}

const PubKey& LocalPublicKey() { return g_pub; }
std::string LocalGuid() {
    std::lock_guard<std::mutex> lk(g_guidMu);
    return g_guid;
}
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

static bool RunSelftestBody() {
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

    // 4-13: Ed25519 against RFC 8032 section 7.1 vectors 1 and 2 -- the primitive the
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

    // 14-17: the decision this module actually exports -- sign with our own key, verify against the
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

        // 18-20: the ROUTING form is the same value as the signing form. This is
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

        // 21-24: this account's key path resolves exactly when its SID does, is stable, and sits
        // beside the folder's key under a 16-hex id; 24 pins the name to the SID alone with a known
        // answer (the SYSTEM SID), since two accounts on one install are two players only while
        // the name is derived from the account and from nothing else.
        const std::wstring a = AccountKeyFilePath();
        check((KeyFileAcl().userSid.empty() || ue_wrap::paths::ModDataDir().empty()) == a.empty(),
              "the account key path does not resolve exactly when the SID and the data folder do");
        if (!a.empty()) {
            const std::wstring dir = ue_wrap::paths::ModDataDir();
            check(a == AccountKeyFilePath() && a != KeyFilePath() &&
                  a == dir + L"\\" + AccountKeyFileName(KeyFileAcl().userSid.data(),
                                                      KeyFileAcl().userSid.size()) &&
                  a.rfind(dir + L"\\multivoid_identity_", 0) == 0 &&
                  a.size() == dir.size() + 20 + 16 + 4 &&
                  a.compare(a.size() - 4, 4, L".key") == 0,
                  "the account key path is not stable, or not beside the folder's key");
            bool hex = true;
            for (size_t i = a.size() - 4 - 16; i < a.size() - 4; ++i) {
                const wchar_t c = a[i];
                if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) hex = false;
            }
            check(hex, "the account key path's id is not 16 lower-case hex");
        }
        uint8_t systemSid[SECURITY_MAX_SID_SIZE];
        DWORD systemSidLen = sizeof(systemSid);
        check(::CreateWellKnownSid(WinLocalSystemSid, nullptr, systemSid, &systemSidLen) != 0 &&
              AccountKeyFileName(systemSid, systemSidLen) ==
                  L"multivoid_identity_f53c7fa26c1476dc.key",
              "the account key name is not the hash of the SID bytes alone (SYSTEM known answer)");
    }

    // 25-29: the key file is written whole through coop/atomic_file, with the access list on the
    // result: a malformed file replaced, a second create-only write refused, the list protected
    // with the two entries. The file is a scratch one, never the real key, and no g_ state is
    // touched.
    {
        const bool breakIt =
            coop::config::ResolveFlag(coop::config_registry::rows::selftest_break_identity);
        const std::wstring dir = ue_wrap::paths::ModDataDir();
        if (dir.empty()) {
            UE_LOGI("peer_identity selftest: the mod has no data folder this session -- the key-write cases are skipped");
        } else {
            SweepDeadScratchKeys(dir);
            const std::wstring scratch =
                dir + L"\\" + kScratchKeyPrefix + std::to_wstring(::GetCurrentProcessId()) + L".key";
            ::DeleteFileW(scratch.c_str());
            const bool seeded =
                coop::atomic_file::Write(scratch, "malformed", coop::atomic_file::Mode::Replace,
                                         coop::atomic_file::Sync::Cached).ok();
            uint8_t want[kPrivKeyBytes];
            uint8_t expect[kPrivKeyBytes];
            for (size_t i = 0; i < kPrivKeyBytes; ++i) {
                want[i] = static_cast<uint8_t>(i + 1);
                expect[i] = static_cast<uint8_t>(i + 1 + (breakIt ? 1 : 0));
            }
            DWORD werr = 0;
            // The seed's own failure is named here, and adds a check only when it fails.
            if (!seeded)
                check(false, "the key-write case's seed (a malformed file) was not written");
            check(seeded && WriteKeyFile(scratch, want, "selftest", &werr, coop::atomic_file::Mode::Replace),
                  "the key file was not written over a malformed one (Replace)");
            uint8_t got[kPrivKeyBytes]{};
            std::string gotBound;
            DWORD rerr = 0;
            check(ReadKeyFile(scratch, got, &gotBound, &rerr) == ReadResult::Loaded && gotBound == "selftest" &&
                      std::memcmp(got, expect, kPrivKeyBytes) == 0,
                  "the key file does not read back as the key that was written");
            DWORD cerr = 0;
            check(!WriteKeyFile(scratch, want, "selftest", &cerr, coop::atomic_file::Mode::CreateOnly) &&
                      (cerr == ERROR_ALREADY_EXISTS || cerr == ERROR_FILE_EXISTS),
                  "a create-only key write over an existing file was not refused");
            if (coop::atomic_file::VolumeKeepsAcls(scratch) && KeyFileAcl().ok) {
                bool isProtected = false;
                DWORD aces = 0;
                const bool read = ReadKeyDacl(scratch, isProtected, aces);
                check(read && isProtected, "the written key file's access list is not protected");
                check(read && aces == 2, "the written key file's access list is not two entries");
            }
            ::DeleteFileW(scratch.c_str());
        }
    }

    if (pass == total) {
        UE_LOGI("peer_identity selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("peer_identity selftest: %d/%d checks passed", pass, total);
    return false;
}

// Once per process: the key-file cases do file operations and flushes, and a session Stop/Start
// would redo them; a later call returns the first run's verdict without output.
bool RunSelftest() {
    static std::once_flag ran;
    static bool verdict = false;
    std::call_once(ran, [] { verdict = RunSelftestBody(); });
    return verdict;
}

static bool RunBindingSelftestBody() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("identity binding selftest FAIL: %s", what);
    };
    using identity_key_file::Binding;
    using identity_key_file::JudgeBinding;
    using identity_key_file::ParseKeyText;

    // The fingerprint against a vector fixed outside this code (sha256 of the tag, a newline and
    // "abc"): the machine id is lower-cased first, an unreadable one binds `none`, and a fake id
    // stands in for the registry value.
    const std::string abc = "65007aa6fa75a9b6d93789c5c7a6c16c7bc1dc5aa0c87538083aa6435c515b0d";
    check(identity_crypto::BindingValue("abc") == abc, "the binding of \"abc\" is not the vector");
    check(identity_crypto::BindingValue("ABC") == abc, "the machine id is not lower-cased");
    check(identity_crypto::BindingValue("") == "none", "an empty machine id does not bind `none`");
    check(identity_crypto::BindingValue("abd") != abc, "two machine ids bind the same");

    // The verdict, over every state of the file against every state of the machine.
    const std::string other = identity_crypto::BindingValue("other");
    check(JudgeBinding(abc, abc) == Binding::Match, "a matching bound is not accepted");
    check(JudgeBinding(other, abc) == Binding::Foreign, "another machine's bound is not foreign");
    check(JudgeBinding("", abc) == Binding::Foreign, "a file with no bound line is not foreign");
    check(JudgeBinding("not hex", abc) == Binding::Foreign, "a malformed bound is not foreign");
    check(JudgeBinding("none", abc) == Binding::Rebind, "a bound of none is not re-bound");
    check(JudgeBinding(abc, "none") == Binding::MachineUnknown &&
          JudgeBinding("", "none") == Binding::MachineUnknown &&
          JudgeBinding("none", "none") == Binding::MachineUnknown,
          "an unreadable machine id does not leave the key as it is");

    // The file text to its key and its bound value.
    const std::string hex(64, 'a');
    uint8_t seed[kPrivKeyBytes]{};
    std::string bound = "stale";
    check(ParseKeyText("# h\nkey=" + hex + "\nbound=" + abc + "\n", seed, &bound) ==
              identity_key_file::ReadResult::Loaded && bound == abc && seed[0] == 0xaa,
          "a key and its bound do not parse");
    check(ParseKeyText("key=" + hex + "\r\nbound=none\r\n", seed, &bound) ==
              identity_key_file::ReadResult::Loaded && bound == "none",
          "a CRLF file does not parse");
    check(ParseKeyText("bound=" + abc + "\nkey=" + hex + "\n", seed, &bound) ==
              identity_key_file::ReadResult::Loaded && bound == abc,
          "a bound line before the key is not found");
    check(ParseKeyText("key=" + hex + "\n", seed, &bound) == identity_key_file::ReadResult::Loaded &&
              bound.empty(),
          "a file with no bound line does not read as empty");
    check(ParseKeyText("key=" + hex.substr(2) + "\nbound=" + abc + "\n", seed, &bound) ==
              identity_key_file::ReadResult::Malformed,
          "a short key is not malformed");
    check(ParseKeyText("bound=" + abc + "\n", seed, &bound) == identity_key_file::ReadResult::Malformed,
          "a file with no key line is not malformed");

    if (pass == total) {
        UE_LOGI("identity binding selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("identity binding selftest: %d/%d checks passed", pass, total);
    return false;
}

// Pure and fast, run once at boot beside the key's load: what decides whether a key file is this
// machine's must not wait for a session.
bool RunBindingSelftest() {
    static std::once_flag ran;
    static bool verdict = false;
    std::call_once(ran, [] { verdict = RunBindingSelftestBody(); });
    return verdict;
}

}  // namespace coop::net::peer_identity
