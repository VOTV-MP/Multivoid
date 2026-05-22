#include "coop/event_feed.h"

#include "coop/net/session.h"
#include "coop/remote_player.h"
#include "ue_wrap/hud_feed.h"
#include "ue_wrap/log.h"

#include <windows.h>

#include <vector>

namespace coop::event_feed {

namespace {

std::wstring g_localNick = L"Player";
std::wstring g_remoteNick = L"Player 2";  // until a Join tells us otherwise
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
        }
    }
}

}  // namespace coop::event_feed
