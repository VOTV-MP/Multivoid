// coop/net/lobby_proof.cpp -- see coop/net/lobby_proof.h.

#include "coop/net/lobby_proof.h"

#include "coop/net/http_client.h"
#include "coop/net/peer_identity.h"
#include "identity_crypto.h"  // co-located: the one hex codec the identity is built with
#include "ue_wrap/core/log.h"

#include <mutex>

namespace coop::net::lobby_proof {
namespace {

// Must equal ANNOUNCE_TAG in server/src/lobby_proof.rs, and its first bytes differ from the
// signaling relay's and the admission's tags, so one signature cannot serve as another. The pair
// is covered by the selftest below, which checks a vector the server's tests check too.
constexpr char kAnnounceTag[] = "multivoid-lobby-announce-v1";
constexpr size_t kNonceHexLen = 64;
// gen: plus 64 hex; the master accepts exactly this width, which is what makes the un-delimited
// blob unambiguous.
constexpr size_t kIdentityLen = 4 + 64;

bool IsLowerHex(const std::string& s) {
    for (char c : s)
        if (!(('0' <= c && c <= '9') || ('a' <= c && c <= 'f'))) return false;
    return true;
}

// Tag, identity, nonce, master name -- no separators. The caller has checked the shapes.
std::string BuildBlob(const std::string& identity, const std::string& nonceHex,
                      const std::string& masterName) {
    std::string blob;
    blob.reserve(sizeof(kAnnounceTag) - 1 + identity.size() + nonceHex.size() + masterName.size());
    blob.append(kAnnounceTag, sizeof(kAnnounceTag) - 1);
    blob.append(identity);
    blob.append(nonceHex);
    blob.append(masterName);
    return blob;
}

bool ShapesOk(const std::string& identity, const std::string& nonceHex,
              const std::string& masterName) {
    return identity.size() == kIdentityLen && identity.compare(0, 4, "gen:") == 0 &&
           IsLowerHex(identity.substr(4)) && nonceHex.size() == kNonceHexLen &&
           IsLowerHex(nonceHex) && !masterName.empty();
}

bool VerifyHex(const std::string& identity, const std::string& blob, const std::string& sigHex) {
    peer_identity::PubKey pub{};
    peer_identity::Sig sig{};
    if (!peer_identity::PublicKeyFromIdentityString(identity, pub) ||
        !identity_crypto::FromHex(sigHex, sig.data(), sig.size()))
        return false;
    return peer_identity::VerifyBlob(pub, reinterpret_cast<const uint8_t*>(blob.data()),
                                     blob.size(), sig);
}

}  // namespace

std::string ProofHex(const std::string& identity, const std::string& nonceHex,
                     const std::string& masterName) {
    // The shapes are asserted, not assumed: nothing upstream checks them, and a signature over
    // bytes the master cannot rebuild proves nothing, so the honest failure is here.
    if (!ShapesOk(identity, nonceHex, masterName)) {
        UE_LOGE("lobby: cannot sign the announce -- the identity, the nonce or the master's name "
                "is not the shape the master rebuilds the proof from");
        return std::string();
    }
    const std::string blob = BuildBlob(identity, nonceHex, masterName);
    const peer_identity::Sig sig = peer_identity::SignBlob(
        reinterpret_cast<const uint8_t*>(blob.data()), blob.size());
    return identity_crypto::ToHex(sig.data(), sig.size());
}

namespace {

bool RunSelftestBody() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("lobby_proof selftest FAIL: %s", what);
    };

    // The vector is a copy of server/tests/vectors/lobby_proof.json (the server's tests check the
    // same one); the two must stay equal, and neither is allowed to change without the other.
    const std::string identity =
        "gen:29acbae141bccaf0b22e1a94d34d0bc7361e526d0bfe12c89794bc9322966dd7";
    const std::string nonce = "030a11181f262d343b424950575e656c737a81888f969da4abb2b9c0c7ced5dc";
    const std::string masterName = "master.multivoid.dev";
    const std::string blobHex =
        "6d756c7469766f69642d6c6f6262792d616e6e6f756e63652d763167656e3a3239616362616531343162636361"
        "663062323265316139346433346430626337333631653532366430626665313263383937393462633933323239"
        "363664643730333061313131383166323632643334336234323439353035373565363536633733376138313838"
        "3866393639646134616262326239633063376365643564636d61737465722e6d756c7469766f69642e646576";
    const std::string sigHex =
        "4d76c9db55d9a5fa53b81543babe2e896cdd907f4da66a25b83c8a895c87e3c3ccf8a31903b99389b08ad48975"
        "e0f8480813218f79b45c7f255a7aa4d78a1507";

    const std::string blob = BuildBlob(identity, nonce, masterName);
    check(identity_crypto::ToHex(reinterpret_cast<const uint8_t*>(blob.data()), blob.size()) ==
              blobHex,
          "the built blob is not the vector's blob");
    check(ShapesOk(identity, nonce, masterName), "the vector's own inputs fail the shape check");
    check(VerifyHex(identity, blob, sigHex), "the vector's signature does not verify");

    std::string flipped = sigHex;
    flipped[0] = (flipped[0] == '0') ? '1' : '0';
    check(!VerifyHex(identity, blob, flipped), "a signature with one bit changed still verifies");
    check(!VerifyHex(identity, BuildBlob(identity, nonce, "rogue.example"), sigHex),
          "a proof made for another master name verifies");

    check(!ShapesOk(identity, nonce.substr(1), masterName) &&
              !ShapesOk(identity, nonce, std::string()) &&
              !ShapesOk(identity.substr(1), nonce, masterName),
          "a malformed input passes the shape check");

    // The URL -> signed-name rows: a copy of server/tests/vectors/master_names.json, whose twin test
    // in the server checks the same rows against canon_host_name. The two must stay equal.
    struct NameRow { const char* url; const char* name; };
    static const NameRow kNameRows[] = {
        {"master.multivoid.dev:10443", "master.multivoid.dev"},
        {"https://master.multivoid.dev:10443", "master.multivoid.dev"},
        {"http://203.0.113.7:10443", "203.0.113.7"},
        {"HTTPS://Master.Multivoid.DEV:10443", "master.multivoid.dev"},
        {"https://master.multivoid.dev:10443/v1/lobbies", "master.multivoid.dev"},
        {"https://master.multivoid.dev.:10443", "master.multivoid.dev"},
        {"https://alice:pw@master.multivoid.dev:10443", "master.multivoid.dev"},
        {"[::1]:10443", "[::1]"},
        {"https://[2001:db8::1]:10443/", "[2001:db8::1]"},
        {"https://b\xC3\xBC" "cher.example:10443", ""},
    };
    for (const NameRow& row : kNameRows) {
        const std::string got = http::HostName(row.url);
        if (got != row.name)
            UE_LOGE("lobby_proof selftest: HostName(%s) = '%s', want '%s'", row.url, got.c_str(), row.name);
        check(got == row.name, "a URL does not give the name the master compares");
    }

    // Our own key, when it is loaded: the proof ProofHex makes verifies, and not for another name.
    const std::string& self = peer_identity::LocalIdentityString();
    if (self.size() == kIdentityLen) {
        const std::string proof = ProofHex(self, nonce, masterName);
        check(proof.size() == 128 && VerifyHex(self, BuildBlob(self, nonce, masterName), proof),
              "our own proof does not verify against our own key");
        check(!VerifyHex(self, BuildBlob(self, nonce, "rogue.example"), proof),
              "our own proof verifies for another master name");
    } else {
        UE_LOGI("lobby_proof selftest: no identity loaded -- the own-key round trip is skipped");
    }

    if (pass == total) {
        UE_LOGI("lobby_proof selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("lobby_proof selftest: %d/%d checks passed", pass, total);
    return false;
}

}  // namespace

// Once per process: a later call returns the first run's verdict without output.
bool RunSelftest() {
    static std::once_flag ran;
    static bool verdict = false;
    std::call_once(ran, [] { verdict = RunSelftestBody(); });
    return verdict;
}

}  // namespace coop::net::lobby_proof
