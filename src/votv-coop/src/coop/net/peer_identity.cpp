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
#include <bcrypt.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// Ed25519, from the donna translation unit GNS already compiles into the static
// library we link (`third_party/GameNetworkingSockets/src/CMakeLists.txt:73-74`
// builds `external/ed25519-donna/ed25519_VALVE.c`; `crypto_25519_donna.cpp` is
// its other caller). Declared here rather than including the vendored header so
// this TU pulls in nothing from GNS's internal `src/` tree -- three prototypes
// with C linkage are the whole dependency.
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

const char* kKeyFileName = "multivoid_identity.key";

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

std::wstring KeyFilePath() {
    // The same directory multivoid.ini uses (`config.cpp:177-180` derives it the
    // same way), so our artifacts stay together where a player can find them. The
    // key is a SEPARATE file on purpose -- see the header.
    const std::wstring dir = ue_wrap::paths::ExeDir();
    if (dir.empty()) return {};
    std::wstring p = dir + L"\\";
    for (const char* c = kKeyFileName; *c; ++c) p.push_back(static_cast<wchar_t>(*c));
    return p;
}

bool ReadKeyFile(uint8_t priv[kPrivKeyBytes]) {
    const std::wstring path = KeyFilePath();
    if (path.empty()) return false;
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (line.compare(0, eq, "key") != 0) continue;
        std::string hex = line.substr(eq + 1);
        while (!hex.empty() && (hex.back() == '\r' || hex.back() == '\n' ||
                                hex.back() == ' ' || hex.back() == '\t'))
            hex.pop_back();
        return FromHex(hex, priv, kPrivKeyBytes);
    }
    return false;
}

bool WriteKeyFile(const uint8_t priv[kPrivKeyBytes]) {
    const std::wstring path = KeyFilePath();
    if (path.empty()) return false;
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "# Multivoid durable player identity -- KEEP THIS FILE SECRET.\n"
         "# Anyone who has this key can play as you: it is what proves your identity\n"
         "# to every host you join, and it is what your stored inventory is named by.\n"
         "# Copy it to another PC to take your identity with you; never paste it into\n"
         "# a bug report, a screenshot or a Discord message.\n"
         "key=" << ToHex(priv, kPrivKeyBytes) << "\n";
    return f.good();
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
    bool minted = false;
    if (!ReadKeyFile(g_priv)) {
        if (!RandomBytes(g_priv, kPrivKeyBytes)) {
            UE_LOGE("peer_identity: BCryptGenRandom failed -- no identity can be established");
            return false;
        }
        minted = true;
    }
    ed25519_publickey(g_priv, g_pub.data());
    g_guid = GuidForPublicKey(g_pub);
    if (g_guid.empty()) {
        UE_LOGE("peer_identity: could not derive the guid from our own key");
        return false;
    }
    // The routing form, built HERE rather than at each use so the P2P announce,
    // the dial and the logs cannot drift from the bytes we sign with. Built by
    // hand rather than through SteamNetworkingIdentityRender because Load() runs
    // at boot, long before GNS is initialised; `[V]` the format is fixed at
    // `steamnetworkingsockets_shared.cpp:234-247` and ParseString round-trips it
    // at `:335-357`.
    g_identityString = "gen:" + ToHex(g_pub.data(), g_pub.size());
    if (minted) {
        if (WriteKeyFile(g_priv)) {
            UE_LOGI("peer_identity: minted a new durable identity %s (saved to %s) "
                    "-- dial=%s",
                    g_guid.c_str(), kKeyFileName, g_identityString.c_str());
        } else {
            // Same shape as the retired player_guid's unreadable-ini path: the
            // session still works, the identity just does not survive a restart,
            // and saying so is the difference between a puzzle and a known state.
            UE_LOGW("peer_identity: minted identity %s but could NOT write %s -- this "
                    "identity is TEMPORARY and your stored inventory will not be found "
                    "again next launch", g_guid.c_str(), kKeyFileName);
        }
    } else {
        // The `dial=` half is the value a joiner needs when there is no master in
        // the loop (net.host_identity), so it is printed on the ordinary path and
        // not only on the mint.
        UE_LOGI("peer_identity: loaded durable identity %s -- dial=%s",
                g_guid.c_str(), g_identityString.c_str());
    }
    g_loaded = true;
    return true;
}

const PubKey& LocalPublicKey() { return g_pub; }
const std::string& LocalGuid()  { return g_guid; }
const std::string& LocalIdentityString() { return g_identityString; }

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
    // WHY THIS IS A ResetIdentity AND NOT A SetCertificate -- measured 2026-08-29,
    // after the first build tried the cert route and the host refused to start.
    // `SetCertificate` on a SELF-ISSUED UNSIGNED cert cannot work: `[V]`
    // `CertStore_CheckCert` returns at its FIRST line for a cert with no CA
    // signature (`certstore.cpp:600-605`, "No signature") and therefore never
    // reaches `outMsgCert.ParseFromString` -- so `InternalSetCertificate` goes on
    // to read an EMPTY message and rejects it with "Cert has invalid public key"
    // (`csteamnetworkingsockets.cpp:748-752`). Signing it would need a trusted CA
    // key in the store, which is the whole apparatus this design removed.
    //
    // Nothing is lost by dropping it. GNS mints its own ephemeral session key and
    // stamps our identity into the cert it sends (`connections.cpp:1316-1319`
    // `SetLocalCertUnsigned`), so the remote's identity comparison still passes --
    // and the durable key was never what GNS checked anyway: `[V]` a cert's
    // identity and its key are unbound (see the header), which is exactly why the
    // admission challenge, not the certificate, is what proves who we are.
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

    // 10-13: the decision this module actually exports -- sign with our own key,
    // verify against the identity bytes a receiver would read off a connection,
    // and refuse both a flipped signature and a different signer's key. That last
    // one is the attack: `[V]` GNS binds a cert's identity to nothing, so a peer
    // CAN claim a victim's key -- and this is the check that refuses it.
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
