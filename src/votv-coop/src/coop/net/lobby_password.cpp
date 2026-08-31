// coop/net/lobby_password.cpp -- see coop/net/lobby_password.h for the rule this
// construction exists to obey, and for why the first design was thrown away.

#include "coop/net/lobby_password.h"

#include "ue_wrap/core/log.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstring>

namespace coop::net::lobby_password {
namespace {

// CNG's own success code. `peer_identity.cpp` compares against a literal 0 for
// the same reason: pulling in ntstatus.h drags a mountain of macro collisions
// into a translation unit that needs exactly one constant.
constexpr NTSTATUS kOk = 0;

// The domain tag. It is in the MAC'd blob, not merely in this comment, so a tag
// minted for a lobby password can never be mistaken for -- or replayed as -- any
// other MAC this project might grow. Bump the digit if the layout changes; a
// changed layout under an unchanged tag is how two builds silently disagree
// about what they are proving.
constexpr char kTag[] = "MVLP1";
constexpr size_t kTagLen = sizeof(kTag) - 1;

}  // namespace

// The derivation, with the round count as a parameter. Production always uses
// `kIterations`; the SELFTEST uses a cheap count for the arms that only need to show
// which INPUTS reach the KDF, and one full-cost run for the constant we ship.
//
// WHY: `RunSelftest` derived four times at 200k rounds, un-gated, on the session-bringup
// thread that also loads the world -- about 400 ms added to EVERY host and EVERY join, on
// both peers, on every smoke. The commit that shipped it listed "16 checks, un-gated" as
// evidence and never priced them (post-ship audit, 2026-08-31). The negatives are worth
// keeping; paying full price four times to learn that a salt is a salt is not.
static bool DeriveKeyAt(const std::string& password, const peer_identity::PubKey& hostPub,
                        uint32_t iterations, std::array<uint8_t, 32>& outKey) {
    outKey.fill(0);
    // AN EMPTY PASSWORD DERIVES NOTHING. PBKDF2 would happily stretch a zero-length
    // input into a perfectly good key, and that key would produce a tag a host
    // requiring a password would ACCEPT -- so "I have no password" would be a valid
    // proof of knowing one. The refusal is the point.
    if (password.empty()) return false;

    BCRYPT_ALG_HANDLE alg = nullptr;
    NTSTATUS st = ::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                                BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (st != kOk) {
        UE_LOGE("lobby_password: BCryptOpenAlgorithmProvider failed (0x%08lX)",
                static_cast<unsigned long>(st));
        return false;
    }
    st = ::BCryptDeriveKeyPBKDF2(
        alg,
        reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
        static_cast<ULONG>(password.size()),
        // THE SALT IS THE HOST'S PUBLIC KEY. Per-host by construction, so one
        // table is worth nothing against a second host and two hosts choosing the
        // same password do not share a derived key.
        reinterpret_cast<PUCHAR>(const_cast<uint8_t*>(hostPub.data())),
        static_cast<ULONG>(hostPub.size()),
        iterations,
        outKey.data(), static_cast<ULONG>(outKey.size()), 0);
    ::BCryptCloseAlgorithmProvider(alg, 0);
    if (st != kOk) {
        UE_LOGE("lobby_password: BCryptDeriveKeyPBKDF2 failed (0x%08lX)",
                static_cast<unsigned long>(st));
        outKey.fill(0);
        return false;
    }
    return true;
}

bool DeriveKey(const std::string& password, const peer_identity::PubKey& hostPub,
               std::array<uint8_t, 32>& outKey) {
    return DeriveKeyAt(password, hostPub, kIterations, outKey);
}

bool ComputeTag(const std::array<uint8_t, 32>& key, const peer_identity::PubKey& hostPub,
                const peer_identity::PubKey& clientPub, const uint8_t nonce[32],
                Tag& outTag) {
    outTag.fill(0);

    // The blob, laid out once and in one place. BOTH identities are in it so a tag
    // harvested from one session cannot be aimed at a third party, and the host's
    // nonce is in it so it is fresh per attempt -- the same two properties, for the
    // same two reasons, as the identity blob this travels beside.
    uint8_t blob[kTagLen + peer_identity::kPubKeyBytes * 2 + 32];
    size_t o = 0;
    std::memcpy(blob + o, kTag, kTagLen);                            o += kTagLen;
    std::memcpy(blob + o, hostPub.data(), hostPub.size());           o += hostPub.size();
    std::memcpy(blob + o, clientPub.data(), clientPub.size());       o += clientPub.size();
    std::memcpy(blob + o, nonce, 32);                                o += 32;
    if (o != sizeof(blob)) return false;   // unreachable; states the layout invariant

    BCRYPT_ALG_HANDLE alg = nullptr;
    NTSTATUS st = ::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                                BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (st != kOk) return false;

    BCRYPT_HASH_HANDLE h = nullptr;
    st = ::BCryptCreateHash(alg, &h, nullptr, 0,
                            reinterpret_cast<PUCHAR>(const_cast<uint8_t*>(key.data())),
                            static_cast<ULONG>(key.size()), 0);
    if (st == kOk) {
        st = ::BCryptHashData(h, blob, static_cast<ULONG>(sizeof(blob)), 0);
        if (st == kOk)
            st = ::BCryptFinishHash(h, outTag.data(), static_cast<ULONG>(outTag.size()), 0);
        ::BCryptDestroyHash(h);
    }
    ::BCryptCloseAlgorithmProvider(alg, 0);
    if (st != kOk) {
        UE_LOGE("lobby_password: HMAC failed (0x%08lX)", static_cast<unsigned long>(st));
        outTag.fill(0);
        return false;
    }
    return true;
}

bool TagsEqual(const Tag& a, const Tag& b) {
    // OR the differences and test ONCE. No early exit, no branch on the data --
    // the loop runs the full length whatever the input, so the time it takes says
    // nothing about how many bytes matched. `volatile` on the accumulator keeps an
    // optimiser from noticing it may stop early on our behalf.
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
    return diff == 0;
}

bool RunSelftest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("lobby_password selftest FAIL: %s", what);
    };

    peer_identity::PubKey hostA{}, hostB{}, client{};
    for (int i = 0; i < peer_identity::kPubKeyBytes; ++i) {
        hostA[static_cast<size_t>(i)]  = static_cast<uint8_t>(i);
        hostB[static_cast<size_t>(i)]  = static_cast<uint8_t>(i ^ 0xFF);
        client[static_cast<size_t>(i)] = static_cast<uint8_t>(0xA0 + i);
    }
    uint8_t nonce1[32], nonce2[32];
    for (int i = 0; i < 32; ++i) {
        nonce1[i] = static_cast<uint8_t>(i);
        nonce2[i] = static_cast<uint8_t>(i + 1);
    }

    // CHEAP ROUNDS FOR THE INPUT ARMS. What these prove is WHICH INPUTS reach the KDF --
    // that the salt is the host key, that a one-character change lands, that an empty
    // password refuses. None of that depends on the round count, and paying the shipped
    // 200k four times cost ~400 ms on every session start, on the thread that also loads
    // the world (post-ship audit). ONE full-cost derivation below keeps the production
    // constant itself exercised.
    constexpr uint32_t kCheap = 1;
    std::array<uint8_t, 32> kA{}, kA2{}, kB{}, kOther{}, kEmpty{}, kReal{};
    check(DeriveKeyAt("hunter2", hostA, kCheap, kA), "the derivation must succeed");
    check(DeriveKeyAt("hunter2", hostA, kCheap, kA2), "the derivation must be deterministic");
    check(kA == kA2, "the same password and host must derive the same key");

    // THE EMPTY PASSWORD. It must REFUSE rather than derive, or "no password" is a
    // valid proof of knowing one.
    check(!DeriveKeyAt("", hostA, kCheap, kEmpty), "an empty password must not derive a key");
    check(kEmpty == std::array<uint8_t, 32>{}, "a refused derivation must leave no key behind");

    // THE SALT MUST ACTUALLY BE THE HOST KEY. If this passed while the salt were
    // ignored, one table would open every locked lobby in the world.
    check(DeriveKeyAt("hunter2", hostB, kCheap, kB), "derivation under a second host");
    check(kA != kB, "the same password under two hosts must NOT collide");

    check(DeriveKeyAt("hunter3", hostA, kCheap, kOther), "derivation of a near-miss password");
    check(kA != kOther, "a one-character difference must not collide");

    // THE SHIPPED CONSTANT, once. A round count that had drifted to something absurd (or
    // to zero) would sail through every arm above, since they all pass kCheap -- so the
    // production path owes one real run, and the round count must MATTER.
    check(DeriveKey("hunter2", hostA, kReal), "the production derivation must succeed");
    check(kReal != kA, "kIterations is not reaching the KDF -- 200k rounds produced the "
                       "same key as 1");

    Tag t1{}, t1b{}, t2{}, tOtherKey{};
    check(ComputeTag(kA, hostA, client, nonce1, t1), "the tag must compute");
    check(ComputeTag(kA, hostA, client, nonce1, t1b), "the tag must be deterministic");
    check(TagsEqual(t1, t1b), "the same inputs must give the same tag");

    // FRESHNESS: a tag is for ONE challenge. If this failed, a captured tag would
    // open the lobby forever.
    check(ComputeTag(kA, hostA, client, nonce2, t2), "the tag under a second nonce");
    check(!TagsEqual(t1, t2), "a tag must NOT verify against a different nonce");

    check(ComputeTag(kOther, hostA, client, nonce1, tOtherKey), "the tag under a wrong key");
    check(!TagsEqual(t1, tOtherKey), "a wrong password must NOT produce the right tag");

    if (pass == total)
        UE_LOGI("lobby_password selftest: ALL PASS (%d checks)", total);
    else
        UE_LOGE("lobby_password selftest: %d/%d passed", pass, total);
    return pass == total;
}

}  // namespace coop::net::lobby_password
