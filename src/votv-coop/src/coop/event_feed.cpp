#include "coop/event_feed.h"

#include "coop/net/session.h"
#include "coop/remote_player.h"
#include "coop/remote_prop.h"
#include "ue_wrap/hud_feed.h"
#include "ue_wrap/log.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <vector>

namespace coop::event_feed {

namespace {

std::wstring g_localNick = L"Player";
// Placeholder shown ONLY in the unusual case the peer drops before its Join
// reliable message lands (Bye before Join completes). Once Join is delivered
// this becomes the real peer nickname.
std::wstring g_remoteNick = L"Remote player";
bool g_lastConnected = false;
bool g_joinSent = false;

std::vector<uint8_t> ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::vector<uint8_t> out(n > 0 ? n : 0);
    if (n > 0)
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                              reinterpret_cast<char*>(out.data()), n, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const uint8_t* p, int len) {
    if (len <= 0) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(p), len, nullptr, 0);
    std::wstring out(n > 0 ? n : 0, L'\0');
    if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(p), len, out.data(), n);
    return out;
}

}  // namespace

void SetLocalNickname(const std::wstring& nick) {
    if (!nick.empty()) g_localNick = nick;
}

void Update(net::Session& session, RemotePlayer* remote) {
    const bool connected = session.connected();

    // Forward the latest RTT into the remote player so the nameplate can show
    // "<nick> (<ping>ms)". Cheap (atomic load + an int store; no allocation).
    if (remote) remote->SetPing(session.lastRttMs());

    // On (re)connect: announce ourselves once via the reliable channel. Join payload
    // is [uint8 len][len bytes UTF-8 nickname]; the nickname is clamped to fit.
    if (connected && !g_joinSent) {
        std::vector<uint8_t> nickUtf8 = ToUtf8(g_localNick);
        if (nickUtf8.size() > 200) nickUtf8.resize(200);
        std::vector<uint8_t> payload;
        payload.push_back(static_cast<uint8_t>(nickUtf8.size()));
        payload.insert(payload.end(), nickUtf8.begin(), nickUtf8.end());
        if (session.SendReliable(net::ReliableKind::Join, payload.data(),
                                 static_cast<int>(payload.size())))
            g_joinSent = true;  // stop-and-wait: retries automatically until acked
    }

    // On disconnect (peer Bye -> session drops to Handshaking): post the departure.
    // Reason is generated locally (MTA pattern); only graceful "left" is detectable
    // for now (timeout/error reasons are a future enhancement).
    if (!connected && g_lastConnected) {
        ue_wrap::hud_feed::Push(g_remoteNick + L" left the game");
        g_joinSent = false;  // re-announce on the next connect
    }
    g_lastConnected = connected;

    // Drain delivered reliable messages.
    net::ReliableChannel::Message msg;
    while (session.TryGetReliable(msg)) {
        switch (msg.kind) {
        case net::ReliableKind::Join: {
            std::wstring nick = g_remoteNick;
            if (!msg.payload.empty()) {
                const int len = msg.payload[0];
                if (1 + len <= static_cast<int>(msg.payload.size()) && len > 0)
                    nick = FromUtf8(msg.payload.data() + 1, len);
            }
            g_remoteNick = nick;
            if (remote) remote->SetNickname(nick);  // label the nameplate too
            ue_wrap::hud_feed::Push(nick + L" joined the game");
            break;
        }
        case net::ReliableKind::PropRelease: {
            // v4: peer released a held prop. Dispatch to remote_prop which
            // re-enables SimulatePhysics + AddImpulse if throw.
            if (msg.payload.size() < sizeof(net::PropReleasePayload)) {
                UE_LOGW("event_feed: PropRelease payload too short (%zu < %zu)",
                        msg.payload.size(), sizeof(net::PropReleasePayload));
                break;
            }
            net::PropReleasePayload p{};
            std::memcpy(&p, msg.payload.data(), sizeof(p));
            // Trust-boundary validation: a NaN/Inf or absurd-magnitude impulse
            // reaches UPrimitiveComponent::AddImpulse -> PhysX UB. Reject
            // before dispatch. Audit fix 2026-05-24.
            if (!std::isfinite(p.impulseX) || !std::isfinite(p.impulseY) || !std::isfinite(p.impulseZ)) {
                UE_LOGW("event_feed: PropRelease impulse non-finite -- dropping");
                break;
            }
            // kMaxImpulse cap: VOTV's throw mechanic uses impulses on the
            // order of 100-1000 cm/s * mass; 1e7 is well above any legitimate
            // throw and below any value that would teleport a body to infinity.
            constexpr float kMaxImpulse = 1.0e7f;
            if (std::fabs(p.impulseX) > kMaxImpulse ||
                std::fabs(p.impulseY) > kMaxImpulse ||
                std::fabs(p.impulseZ) > kMaxImpulse) {
                UE_LOGW("event_feed: PropRelease impulse out of bounds (%.1f, %.1f, %.1f) -- dropping",
                        p.impulseX, p.impulseY, p.impulseZ);
                break;
            }
            remote_prop::OnRelease(p);
            break;
        }
        }
    }
}

}  // namespace coop::event_feed
