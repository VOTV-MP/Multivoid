// coop/net/build_share.cpp -- see coop/net/build_share.h.

#include "coop/net/build_share.h"

#include "coop/build_trust/build_trust.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/peer_identity.h"
#include "coop/net/protocol.h"
#include "coop/version.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"

#include <steam/steamnetworkingtypes.h>
#include <steam/isteamnetworkingsockets.h>

#include "miniz.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace coop::net::build_share {
namespace {

namespace BT = coop::build_trust;

// ---- the frames: fixed layouts, little-endian, never changed with the protocol number ----
constexpr uint32_t kFrameMagic = 0x5055564Du;  // "MVUP"
constexpr uint8_t kNotice = 1;   // host: proto u16, sha[32], size u32, official u8, target (u8 length, ASCII)
constexpr uint8_t kHello = 2;    // client: nonce[32]
constexpr uint8_t kOffer = 3;    // host: proto u16, sha[32], size u32, sig[64], sigTextLen u16, sigText, target
constexpr uint8_t kWant = 4;     // client: sha[32]
constexpr uint8_t kData = 5;     // host: offset u32, bytes of the deflated file
constexpr uint8_t kDone = 6;     // host: the deflated size u32
constexpr uint8_t kDecline = 7;  // client
constexpr uint8_t kRefuse = 8;   // host: the build could not be read
constexpr uint8_t kBegin = 9;    // host: the deflated size u32, ahead of the first data
constexpr size_t kHead = 8;      // magic, type, three zero bytes
constexpr size_t kSha = BT::kShaBytes;
constexpr size_t kNonce = 32;
constexpr char kOfferTag[] = "MVUP-offer-v1";

constexpr uint32_t kMinSize = 256u * 1024u;
constexpr uint32_t kMaxSize = 200u * 1024u * 1024u;
constexpr int kChunk = 48 * 1024;
constexpr int kPendingCeiling = 192 * 1024;  // reliable bytes queued on a connection before the next chunk waits
constexpr int kChunksAPass = 6;
// A seated player's connection carries the game: small frames, one a pass, and only into a nearly empty queue.
constexpr int kGentleChunk = 12 * 1024;
constexpr int kGentleCeiling = 16 * 1024;
constexpr uint64_t kStartWaitMs = 20 * 60 * 1000;  // a joiner's world load, before the host sends the first byte
constexpr size_t kMaxSigText = 1024;

constexpr uint64_t kHelloWaitMs = 5000;      // a held connection that never asks is closed for its mismatch
constexpr uint64_t kAnswerWaitMs = 180000;   // the player's time at the prompt
constexpr uint64_t kStallMs = 30000;
constexpr uint64_t kDoneLingerMs = 3000;     // the last frames reach the client before the close
constexpr uint64_t kFileIdleMs = 60000;

void Put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(static_cast<uint8_t>(v)); b.push_back(static_cast<uint8_t>(v >> 8)); }
void Put32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
uint16_t Get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t Get32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

std::vector<uint8_t> Frame(uint8_t type) {
    std::vector<uint8_t> b;
    Put32(b, kFrameMagic);
    b.push_back(type);
    b.insert(b.end(), 3, 0);
    return b;
}

bool Send(uint32_t hConn, const std::vector<uint8_t>& frame) {
    auto* sockets = SteamNetworkingSockets();
    return sockets && sockets->SendMessageToConnection(static_cast<HSteamNetConnection>(hConn), frame.data(),
                                                       static_cast<uint32>(frame.size()),
                                                       k_nSteamNetworkingSend_Reliable, nullptr) == k_EResultOK;
}

// The remote's 32 identity bytes as the transport holds them for the connection, never as a frame says.
bool RemoteKeyOf(uint32_t hConn, peer_identity::PubKey& out) {
    auto* sockets = SteamNetworkingSockets();
    SteamNetConnectionInfo_t info{};
    if (!sockets || !sockets->GetConnectionInfo(static_cast<HSteamNetConnection>(hConn), &info)) return false;
    const SteamNetworkingIdentity& id = info.m_identityRemote;
    if (id.m_eType != k_ESteamNetworkingIdentityType_GenericBytes || id.m_cbSize != peer_identity::kPubKeyBytes) return false;
    std::memcpy(out.data(), id.m_genericBytes, peer_identity::kPubKeyBytes);
    return true;
}

// The game version a build is made for, as a frame carries it: a length byte and the text.
void PutTarget(std::vector<uint8_t>& b, const std::string& target) {
    const size_t n = std::min<size_t>(target.size(), BT::kTargetMax);
    b.push_back(static_cast<uint8_t>(n));
    b.insert(b.end(), target.begin(), target.begin() + static_cast<std::ptrdiff_t>(n));
}

bool GetTarget(const uint8_t* b, size_t n, size_t at, std::string& out) {
    if (at >= n || b[at] > BT::kTargetMax || at + 1 + b[at] > n) return false;
    out.assign(reinterpret_cast<const char*>(b + at + 1), b[at]);
    return true;
}

// What an offer's signature covers: the tag, the asker's nonce, and the build it names with its game version.
std::vector<uint8_t> OfferMessage(const uint8_t* nonce, uint16_t proto, const uint8_t* sha, uint32_t size,
                                  const std::string& target) {
    std::vector<uint8_t> m(kOfferTag, kOfferTag + sizeof(kOfferTag) - 1);
    m.insert(m.end(), nonce, nonce + kNonce);
    Put16(m, proto);
    m.insert(m.end(), sha, sha + kSha);
    Put32(m, size);
    PutTarget(m, target);
    return m;
}

bool Sha256(const uint8_t* data, size_t len, uint8_t out[kSha]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = ::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              ::BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    for (size_t at = 0; ok && at < len;) {
        const ULONG n = static_cast<ULONG>(std::min<size_t>(len - at, 1u << 20));
        ok = ::BCryptHashData(hash, const_cast<PUCHAR>(data + at), n, 0) == 0;
        at += n;
    }
    ok = ok && ::BCryptFinishHash(hash, out, static_cast<ULONG>(kSha), 0) == 0;
    if (hash) ::BCryptDestroyHash(hash);
    if (alg) ::BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

std::string Hex(const uint8_t* p, size_t n) {
    static const char kHex[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s.push_back(kHex[p[i] >> 4]); s.push_back(kHex[p[i] & 0xF]); }
    return s;
}

// This module's own file: the main.dll the process loaded.
std::wstring OwnPath() {
    HMODULE self = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&OwnPath), &self))
        return {};
    wchar_t buf[1024] = {};
    const DWORD n = ::GetModuleFileNameW(self, buf, 1024);
    return (n == 0 || n >= 1024) ? std::wstring() : std::wstring(buf, n);
}

bool ReadWhole(const std::wstring& path, std::vector<uint8_t>& out, size_t cap) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = ::GetFileSizeEx(h, &size) && size.QuadPart > 0 && static_cast<uint64_t>(size.QuadPart) <= cap;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        for (size_t at = 0; ok && at < out.size();) {
            DWORD got = 0;
            ok = ::ReadFile(h, out.data() + at, static_cast<DWORD>(std::min<size_t>(out.size() - at, 1u << 20)), &got,
                            nullptr) && got > 0;
            at += got;
        }
    }
    ::CloseHandle(h);
    if (!ok) out.clear();
    return ok;
}

bool WriteWhole(const std::wstring& path, const uint8_t* data, size_t len) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    for (size_t at = 0; ok && at < len;) {
        DWORD put = 0;
        ok = ::WriteFile(h, data + at, static_cast<DWORD>(std::min<size_t>(len - at, 1u << 20)), &put, nullptr) && put > 0;
        at += put;
    }
    ok = ok && ::FlushFileBuffers(h);
    ::CloseHandle(h);
    return ok;
}

// ================================ host ================================

bool ShareOn() {
    static const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::net_share_build);
    return on;
}

struct Peer {
    uint32_t hConn = 0;
    bool     mismatch = false;
    uint8_t  stage = 0;      // 1 noticed, 2 offered, 3 sending, 4 done, 5 declined
    uint64_t sinceMs = 0;    // the stage began
    uint32_t offset = 0;
    bool     begun = false;  // the deflated size was said
};
constexpr size_t kMaxPeers = 16;
std::array<Peer, kMaxPeers> g_peers;   // net thread only
// This host's main.dll, deflated, while a transfer wants it. A worker reads, checks and deflates it (seconds of
// work the net thread does not stop for); the net thread reads the bytes only once the state says ready.
std::vector<uint8_t> g_file;
std::atomic<int> g_pack{0};            // 0 none, 1 working, 2 ready, 3 failed
uint64_t g_fileUsedMs = 0;

Peer* FindPeer(uint32_t hConn) {
    for (Peer& p : g_peers)
        if (p.hConn == hConn && hConn != 0) return &p;
    return nullptr;
}

Peer* AddPeer(uint32_t hConn) {
    Peer* oldest = &g_peers[0];
    for (Peer& p : g_peers) {
        if (p.hConn == 0) { oldest = &p; break; }
        if (p.sinceMs < oldest->sinceMs) oldest = &p;
    }
    *oldest = Peer{};
    oldest->hConn = hConn;
    return oldest;
}

// The .sig beside this host's main.dll, when the build is a signed one.
std::string OwnSigText() {
    if (BT::Self().step != BT::SelfStep::SigPresent) return {};
    std::vector<uint8_t> raw;
    if (!ReadWhole(OwnPath() + L".sig", raw, kMaxSigText)) return {};
    return std::string(raw.begin(), raw.end());
}

void StartPack() {
    g_fileUsedMs = NowMs();
    int none = 0;
    if (!g_pack.compare_exchange_strong(none, 1)) return;  // working, ready, or failed for this process
    std::thread([] {
        std::vector<uint8_t> bytes;
        uint8_t sha[kSha] = {};
        if (!ReadWhole(OwnPath(), bytes, kMaxSize) || !Sha256(bytes.data(), bytes.size(), sha) ||
            std::memcmp(sha, BT::Self().sha256, kSha) != 0) {
            UE_LOGW("build_share: this host's main.dll could not be read as the build it runs -- it is not shared");
            g_pack.store(3);
            return;
        }
        mz_ulong packed = mz_compressBound(static_cast<mz_ulong>(bytes.size()));
        std::vector<uint8_t> out(packed);
        if (mz_compress2(out.data(), &packed, bytes.data(), static_cast<mz_ulong>(bytes.size()), 6) != MZ_OK) {
            UE_LOGW("build_share: this host's main.dll could not be deflated -- it is not shared");
            g_pack.store(3);
            return;
        }
        out.resize(packed);
        g_file = std::move(out);
        UE_LOGI("build_share: this host's build is ready to send (%zu bytes, %zu deflated)", bytes.size(), g_file.size());
        g_pack.store(2);
    }).detach();
}

uint32_t OwnSize() {
    static uint32_t size = [] {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        return ::GetFileAttributesExW(OwnPath().c_str(), GetFileExInfoStandard, &a) ? a.nFileSizeLow : 0u;
    }();
    return size;
}

// ================================ client ================================

struct Client {
    State    state = State::Idle;
    uint32_t hConn = 0;
    uint8_t  nonce[kNonce] = {};
    uint8_t  sha[kSha] = {};
    uint16_t hostProto = 0;
    uint32_t size = 0;
    uint32_t packed = 0;     // the deflated size, once the host has said it
    bool     official = false;
    std::string hostGuid;
    std::string sigText;
    std::string target;
    std::string error;
    std::vector<uint8_t> buf;
    uint64_t lastMs = 0;
};
std::mutex g_clientMutex;
Client g_client;

std::string Policy() {
    static const std::string p = coop::config::ResolveEnum(::coop::config_registry::rows::net_build_updates);
    return p;
}

std::wstring TrustFile() {
    const std::wstring dir = ue_wrap::paths::ModDataDir();
    return dir.empty() ? std::wstring() : dir + L"\\multivoid_trusted_hosts.txt";
}

bool HostTrusted(const std::string& guid) {
    std::ifstream in(TrustFile());
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!guid.empty() && line == guid) return true;
    }
    return false;
}

void TrustHost(const std::string& guid) {
    if (guid.empty() || HostTrusted(guid)) return;
    std::ofstream out(TrustFile(), std::ios::app);
    out << guid << "\n";
    UE_LOGI("build_share: host %s is trusted for build updates from now on", guid.c_str());
}

void Fail(Client& c, const char* why) {
    c.state = State::Failed;
    c.error = why;
    c.buf.clear();
    c.buf.shrink_to_fit();
    UE_LOGW("build_share: the host's build was not installed -- %s", why);
}

void SendWant(Client& c) {
    std::vector<uint8_t> f = Frame(kWant);
    f.insert(f.end(), c.sha, c.sha + kSha);
    c.buf.clear();
    c.packed = 0;
    c.lastMs = NowMs();
    if (Send(c.hConn, f)) c.state = State::Downloading;
    else Fail(c, "the request could not be sent");
}

// The received file is the offered build: put it where this process's own main.dll is, which stays as main.dll.old.
void Install(Client& c) {
    // Inflated into exactly the offered size, then held to the offered hash: nothing else is written to disk.
    uint8_t sha[kSha] = {};
    std::vector<uint8_t> raw(c.size);
    mz_ulong got = static_cast<mz_ulong>(raw.size());
    if (c.packed == 0 || c.buf.size() != c.packed ||
        mz_uncompress(raw.data(), &got, c.buf.data(), static_cast<mz_ulong>(c.buf.size())) != MZ_OK || got != c.size ||
        !Sha256(raw.data(), raw.size(), sha) || std::memcmp(sha, c.sha, kSha) != 0) {
        Fail(c, "the file that arrived is not the build the host offered");
        return;
    }
    c.buf.swap(raw);
    const std::wstring own = OwnPath();
    if (own.empty()) { Fail(c, "this mod's own file could not be found"); return; }
    const std::wstring fresh = own + L".new", old = own + L".old", sig = own + L".sig";
    if (!WriteWhole(fresh, c.buf.data(), c.buf.size())) { Fail(c, "the new file could not be written"); return; }
    // A loaded module's file can be renamed, not overwritten: the running build steps aside under another name.
    if (!::MoveFileExW(own.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ::DeleteFileW(fresh.c_str());
        Fail(c, "this mod's running file could not be moved aside");
        return;
    }
    if (!::MoveFileExW(fresh.c_str(), own.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ::MoveFileExW(old.c_str(), own.c_str(), MOVEFILE_REPLACE_EXISTING);
        ::DeleteFileW(fresh.c_str());
        Fail(c, "the new file could not be put in place");
        return;
    }
    // The signature belongs to the file it signs: the host's when it sent one, none otherwise.
    if (c.sigText.empty()) ::DeleteFileW(sig.c_str());
    else WriteWhole(sig, reinterpret_cast<const uint8_t*>(c.sigText.data()), c.sigText.size());
    c.buf.clear();
    c.buf.shrink_to_fit();
    c.state = State::Installed;
    UE_LOGI("build_share: installed the host's build sha=%s (%u bytes, b%u, %s); the build that was running is kept as "
            "main.dll.old -- the next start of the game loads the new one",
            Hex(c.sha, 8).c_str(), c.size, static_cast<unsigned>(c.hostProto), c.official ? "official" : "unofficial");
}

bool OfferIsOfficial(const Client& c) {
    BT::SigFile sig;
    if (c.sigText.empty() || !BT::ParseSigFile(c.sigText, &sig)) return false;
    if (sig.build != c.hostProto || sig.target != c.target || std::memcmp(sig.sha256, c.sha, kSha) != 0) return false;
    return BT::SignatureVerifies(sig, coop::config::ResolveFlag(::coop::config_registry::rows::build_trust_test_key));
}

}  // namespace

bool IsFrame(const void* data, int len) {
    return data && len >= static_cast<int>(kHead) && Get32(static_cast<const uint8_t*>(data)) == kFrameMagic;
}

// ================================ host ================================

bool HostOnPendingSpoke(uint32_t hConn, bool mismatch) {
    if (!ShareOn() || hConn == 0) return false;
    Peer* p = FindPeer(hConn);
    if (!p) {
        p = AddPeer(hConn);
        p->stage = 1;
        p->sinceMs = NowMs();
        std::vector<uint8_t> f = Frame(kNotice);
        Put16(f, kProtocolVersion);
        f.insert(f.end(), BT::Self().sha256, BT::Self().sha256 + kSha);
        Put32(f, OwnSize());
        f.push_back(BT::SelfIsOfficial() ? 1 : 0);
        PutTarget(f, coop::version::kGameTarget);
        Send(hConn, f);
    }
    if (mismatch && !p->mismatch) {
        p->mismatch = true;
        UE_LOGI("build_share: a connection on another build number is held for a build exchange (h=0x%08x)",
                static_cast<unsigned>(hConn));
    }
    return p->mismatch && p->stage != 5;
}

void HostOnFrame(uint32_t hConn, const void* data, int len) {
    if (!ShareOn() || !IsFrame(data, len)) return;
    const uint8_t* b = static_cast<const uint8_t*>(data);
    const size_t n = static_cast<size_t>(len);
    Peer* p = FindPeer(hConn);
    if (!p) return;  // a connection the host never noticed asks for nothing
    const uint64_t now = NowMs();
    switch (b[4]) {
    case kHello: {
        if (n < kHead + kNonce || p->stage > 2) return;
        const uint32_t size = OwnSize();
        const std::vector<uint8_t> msg =
            OfferMessage(b + kHead, kProtocolVersion, BT::Self().sha256, size, coop::version::kGameTarget);
        const peer_identity::Sig sig = peer_identity::SignBlob(msg.data(), msg.size());
        const std::string sigText = OwnSigText();
        std::vector<uint8_t> f = Frame(kOffer);
        Put16(f, kProtocolVersion);
        f.insert(f.end(), BT::Self().sha256, BT::Self().sha256 + kSha);
        Put32(f, size);
        f.insert(f.end(), sig.begin(), sig.end());
        Put16(f, static_cast<uint16_t>(sigText.size()));
        f.insert(f.end(), sigText.begin(), sigText.end());
        PutTarget(f, coop::version::kGameTarget);
        if (Send(hConn, f)) {
            p->stage = 2;
            p->sinceMs = now;
        }
        return;
    }
    case kWant: {
        if (n < kHead + kSha || p->stage != 2) return;
        if (std::memcmp(b + kHead, BT::Self().sha256, kSha) != 0) {
            Send(hConn, Frame(kRefuse));
            p->stage = 5;
            return;
        }
        StartPack();
        p->stage = 3;
        p->sinceMs = now;
        p->offset = 0;
        p->begun = false;
        UE_LOGI("build_share: h=0x%08x asked for this host's build", static_cast<unsigned>(hConn));
        return;
    }
    case kDecline:
        p->stage = 5;
        return;
    default:
        return;
    }
}

Hold HostHold(uint32_t hConn) {
    Peer* p = FindPeer(hConn);
    if (!p || !p->mismatch) return Hold::None;
    const uint64_t age = NowMs() - p->sinceMs;
    const bool lapsed = p->stage == 5 || (p->stage == 1 && age > kHelloWaitMs) || (p->stage == 2 && age > kAnswerWaitMs) ||
                        (p->stage == 3 && age > kStallMs) || (p->stage == 4 && age > kDoneLingerMs);
    if (!lapsed) return Hold::Busy;
    *p = Peer{};
    return Hold::Lapsed;
}

void HostPump(int (*pace)(void* ctx, uint32_t hConn), void* ctx) {
    auto* sockets = SteamNetworkingSockets();
    if (!sockets) return;
    const uint64_t now = NowMs();
    bool sending = false;
    for (Peer& p : g_peers) {
        if (p.hConn == 0) continue;
        SteamNetConnectionRealTimeStatus_t st{};
        if (sockets->GetConnectionRealTimeStatus(static_cast<HSteamNetConnection>(p.hConn), &st, 0, nullptr) != k_EResultOK) {
            p = Peer{};  // the connection is gone
            continue;
        }
        if (p.stage != 3) continue;
        sending = true;
        g_fileUsedMs = now;
        const int pack = g_pack.load();
        if (pack == 3) {
            Send(p.hConn, Frame(kRefuse));
            p.stage = 5;
            continue;
        }
        const int how = pace ? pace(ctx, p.hConn) : kFull;
        if (pack != 2 || how == kWait) {
            p.sinceMs = now;  // the worker is still deflating, or the joiner is still loading the world
            continue;
        }
        const int chunk = how == kGentle ? kGentleChunk : kChunk;
        const int ceiling = how == kGentle ? kGentleCeiling : kPendingCeiling;
        const int passes = how == kGentle ? 1 : kChunksAPass;
        if (!p.begun) {
            std::vector<uint8_t> f = Frame(kBegin);
            Put32(f, static_cast<uint32_t>(g_file.size()));
            if (!Send(p.hConn, f)) continue;
            p.begun = true;
        }
        int queued = st.m_cbPendingReliable;
        for (int i = 0; i < passes && queued < ceiling && p.offset < g_file.size(); ++i) {
            const uint32_t take = static_cast<uint32_t>(std::min<size_t>(static_cast<size_t>(chunk), g_file.size() - p.offset));
            std::vector<uint8_t> f = Frame(kData);
            Put32(f, p.offset);
            f.insert(f.end(), g_file.begin() + p.offset, g_file.begin() + p.offset + take);
            if (!Send(p.hConn, f)) break;
            p.offset += take;
            p.sinceMs = now;
            queued += static_cast<int>(take);
        }
        if (p.offset >= g_file.size()) {
            std::vector<uint8_t> f = Frame(kDone);
            Put32(f, static_cast<uint32_t>(g_file.size()));
            Send(p.hConn, f);
            p.stage = 4;
            p.sinceMs = now;
            UE_LOGI("build_share: this host's build was sent whole to h=0x%08x", static_cast<unsigned>(p.hConn));
        }
    }
    if (!sending && g_pack.load() == 2 && now - g_fileUsedMs > kFileIdleMs) {
        g_file.clear();
        g_file.shrink_to_fit();
        g_pack.store(0);
    }
}

// ================================ client ================================

bool Wanted() { return Policy() != "off"; }

State Peek() {
    std::lock_guard<std::mutex> lock(g_clientMutex);
    return g_client.state;
}

void ClientOnFrame(uint32_t hConn, const void* data, int len) {
    if (!IsFrame(data, len)) return;
    const uint8_t* b = static_cast<const uint8_t*>(data);
    const size_t n = static_cast<size_t>(len);
    std::lock_guard<std::mutex> lock(g_clientMutex);
    Client& c = g_client;
    switch (b[4]) {
    case kNotice: {
        if (n < kHead + 2 + kSha + 4 + 1) return;
        const uint16_t proto = Get16(b + kHead);
        const uint8_t* sha = b + kHead + 2;
        if (std::memcmp(sha, BT::Self().sha256, kSha) == 0) return;  // the same build
        std::string target;
        const bool sameGame = GetTarget(b, n, kHead + 2 + kSha + 4 + 1, target) && target == coop::version::kGameTarget;
        if (!Wanted() || !sameGame) {
            if (!sameGame)
                UE_LOGI("build_share: the host's build is for VOTV %s, this game is VOTV %s -- it is not taken",
                        target.c_str(), coop::version::kGameTarget);
            if (proto != kProtocolVersion) Send(hConn, Frame(kDecline));  // the host need not wait to close
            return;
        }
        if (c.state != State::Idle && c.state != State::Failed) return;
        c = Client{};
        c.hConn = hConn;
        if (!peer_identity::RandomBytes(c.nonce, kNonce)) return;
        std::vector<uint8_t> f = Frame(kHello);
        f.insert(f.end(), c.nonce, c.nonce + kNonce);
        if (Send(hConn, f)) {
            c.state = State::Asking;
            c.lastMs = NowMs();
            UE_LOGI("build_share: the host runs another build (b%u sha=%s, ours b%u sha=%s) -- asking for its offer",
                    static_cast<unsigned>(proto), Hex(sha, 8).c_str(), static_cast<unsigned>(kProtocolVersion),
                    Hex(BT::Self().sha256, 8).c_str());
        }
        return;
    }
    case kOffer: {
        if (c.state != State::Asking || hConn != c.hConn || n < kHead + 2 + kSha + 4 + peer_identity::kSigBytes + 2) return;
        const uint8_t* at = b + kHead;
        c.hostProto = Get16(at);
        std::memcpy(c.sha, at + 2, kSha);
        c.size = Get32(at + 2 + kSha);
        peer_identity::Sig sig{};
        std::memcpy(sig.data(), at + 2 + kSha + 4, peer_identity::kSigBytes);
        const size_t textAt = kHead + 2 + kSha + 4 + peer_identity::kSigBytes + 2;
        const size_t textLen = Get16(b + textAt - 2);
        if (textLen > kMaxSigText || n < textAt + textLen) { Fail(c, "the host's offer is malformed"); return; }
        c.sigText.assign(reinterpret_cast<const char*>(b + textAt), textLen);
        if (!GetTarget(b, n, textAt + textLen, c.target) || c.target != coop::version::kGameTarget) {
            Fail(c, "the host's build is made for another version of the game");
            Send(hConn, Frame(kDecline));
            return;
        }
        // The offer is the host's only when the key the transport names for this connection signed it over our nonce.
        peer_identity::PubKey host{};
        const std::vector<uint8_t> msg = OfferMessage(c.nonce, c.hostProto, c.sha, c.size, c.target);
        if (!RemoteKeyOf(hConn, host) || !peer_identity::VerifyBlob(host, msg.data(), msg.size(), sig)) {
            Fail(c, "the host could not prove the offer is its own");
            Send(hConn, Frame(kDecline));
            return;
        }
        if (c.size < kMinSize || c.size > kMaxSize) {
            Fail(c, "the offered file's size is not a build's");
            Send(hConn, Frame(kDecline));
            return;
        }
        c.hostGuid = peer_identity::GuidForPublicKey(host);
        c.official = OfferIsOfficial(c);
        const bool trusted = HostTrusted(c.hostGuid);
        const bool drill = coop::config::ResolveFlag(::coop::config_registry::rows::build_update_auto);
        UE_LOGI("build_share: offer from host %s: b%u sha=%s, %u bytes, %s build, host %s", c.hostGuid.c_str(),
                static_cast<unsigned>(c.hostProto), Hex(c.sha, 8).c_str(), c.size, c.official ? "an official" : "an unofficial",
                trusted ? "trusted" : "not trusted");
        if (c.official || trusted || drill) {
            SendWant(c);
        } else if (Policy() == "trusted") {
            Fail(c, "this host is not on your trusted list, and net.build_updates=trusted takes builds from no other");
            Send(hConn, Frame(kDecline));
        } else {
            c.state = State::Offered;
            c.lastMs = NowMs();
        }
        return;
    }
    case kBegin: {
        if (c.state != State::Downloading || hConn != c.hConn || n < kHead + 4 || c.packed != 0) return;
        c.packed = Get32(b + kHead);
        if (c.packed == 0 || c.packed > kMaxSize) { Fail(c, "the host named a download no build has"); return; }
        c.buf.reserve(c.packed);
        c.lastMs = NowMs();
        return;
    }
    case kData: {
        if (c.state != State::Downloading || hConn != c.hConn || n < kHead + 4) return;
        const uint32_t offset = Get32(b + kHead);
        const size_t take = n - kHead - 4;
        if (c.packed == 0 || offset != c.buf.size() || c.buf.size() + take > c.packed) {
            Fail(c, "the file arrived out of order");
            return;
        }
        c.buf.insert(c.buf.end(), b + kHead + 4, b + n);
        c.lastMs = NowMs();
        return;
    }
    case kDone:
        if (c.state == State::Downloading && hConn == c.hConn) Install(c);
        return;
    case kRefuse:
        if (c.state == State::Downloading && hConn == c.hConn) Fail(c, "the host could not read its own build");
        return;
    default:
        return;
    }
}

void ClientPump() {
    std::lock_guard<std::mutex> lock(g_clientMutex);
    Client& c = g_client;
    // Before the first byte the host may be waiting for this machine to finish loading the world.
    const uint64_t quiet = NowMs() - c.lastMs;
    const bool waitingToStart = c.state == State::Downloading && c.packed == 0;
    if ((c.state == State::Downloading || c.state == State::Asking) && quiet > (waitingToStart ? kStartWaitMs : kStallMs))
        Fail(c, "the host stopped answering");
}

View Snapshot() {
    std::lock_guard<std::mutex> lock(g_clientMutex);
    const Client& c = g_client;
    View v;
    v.state = c.state;
    v.official = c.official;
    v.hostProto = c.hostProto;
    v.size = c.size;
    v.packed = c.packed;
    v.received = c.state == State::Downloading ? static_cast<uint32_t>(c.buf.size()) : 0;
    v.hostGuid = c.hostGuid;
    v.shaHex = Hex(c.sha, 8);
    v.error = c.error;
    return v;
}

void Accept(bool trustHost) {
    std::lock_guard<std::mutex> lock(g_clientMutex);
    Client& c = g_client;
    if (c.state != State::Offered) return;
    if (trustHost) TrustHost(c.hostGuid);
    UE_LOGI("build_share: the player accepted host %s's build%s", c.hostGuid.c_str(), trustHost ? " and trusts the host" : "");
    SendWant(c);
}

void Decline() {
    std::lock_guard<std::mutex> lock(g_clientMutex);
    Client& c = g_client;
    if (c.state != State::Offered) return;
    Send(c.hConn, Frame(kDecline));
    UE_LOGI("build_share: the player declined host %s's build", c.hostGuid.c_str());
    c = Client{};
}

void Dismiss() {
    std::lock_guard<std::mutex> lock(g_clientMutex);
    if (g_client.state == State::Installed || g_client.state == State::Failed) g_client = Client{};
}

void OnDisconnect() {
    std::lock_guard<std::mutex> lock(g_clientMutex);
    Client& c = g_client;
    if (c.state == State::Asking || c.state == State::Offered) c = Client{};
    else if (c.state == State::Downloading) Fail(c, "the connection ended before the file was whole");
}

}  // namespace coop::net::build_share
