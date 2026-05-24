#include "coop/event_feed.h"

#include "coop/net/session.h"
#include "coop/remote_player.h"
#include "coop/remote_prop.h"
#include "ue_wrap/hud_feed.h"
#include "ue_wrap/log.h"
#include "ue_wrap/sdk_profile.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <string>
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

void Update(net::Session& session, RemotePlayer* remote, void* localPlayer) {
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
            // v5: peer released a held prop. Dispatch to remote_prop which
            // re-enables SimulatePhysics + sets linear/angular velocity, and
            // fires Aprop_C.thrown if the launch crosses the throw threshold.
            if (msg.payload.size() < sizeof(net::PropReleasePayload)) {
                UE_LOGW("event_feed: PropRelease payload too short (%zu < %zu)",
                        msg.payload.size(), sizeof(net::PropReleasePayload));
                break;
            }
            net::PropReleasePayload p{};
            std::memcpy(&p, msg.payload.data(), sizeof(p));
            // Trust-boundary validation: a NaN/Inf or absurd-magnitude velocity
            // reaches UPrimitiveComponent::SetPhysicsLinearVelocity ->
            // SetPhysicsAngularVelocityInDegrees -> PhysX UB. Reject before
            // dispatch.
            const float vals[6] = {p.linVelX, p.linVelY, p.linVelZ,
                                   p.angVelX, p.angVelY, p.angVelZ};
            bool finite = true;
            for (float v : vals) {
                if (!std::isfinite(v)) { finite = false; break; }
            }
            if (!finite) {
                UE_LOGW("event_feed: PropRelease velocity non-finite -- dropping");
                break;
            }
            // Linear velocity bound: realistic throws peak at a few thousand
            // cm/s. 1e6 cm/s = 10 km/s -- well beyond any legitimate throw and
            // below any value that would teleport a body to infinity in one
            // tick. Angular velocity bound: a fast tumble is ~3600 deg/s
            // (10 rps); 1e6 is generous headroom.
            constexpr float kMaxLinVel = 1.0e6f;
            constexpr float kMaxAngVel = 1.0e6f;
            if (std::fabs(p.linVelX) > kMaxLinVel ||
                std::fabs(p.linVelY) > kMaxLinVel ||
                std::fabs(p.linVelZ) > kMaxLinVel ||
                std::fabs(p.angVelX) > kMaxAngVel ||
                std::fabs(p.angVelY) > kMaxAngVel ||
                std::fabs(p.angVelZ) > kMaxAngVel) {
                UE_LOGW("event_feed: PropRelease velocity out of bounds (lin=(%.1f,%.1f,%.1f) ang=(%.1f,%.1f,%.1f)) -- dropping",
                        p.linVelX, p.linVelY, p.linVelZ,
                        p.angVelX, p.angVelY, p.angVelZ);
                break;
            }
            remote_prop::OnRelease(p, localPlayer);
            break;
        }
        case net::ReliableKind::PropSpawn: {
            // v5 Bug C: peer dropped an inventory item -- spawn a matching
            // Aprop_X_C locally so subsequent PropPose updates resolve.
            if (msg.payload.size() < sizeof(net::PropSpawnPayload)) {
                UE_LOGW("event_feed: PropSpawn payload too short (%zu < %zu)",
                        msg.payload.size(), sizeof(net::PropSpawnPayload));
                break;
            }
            net::PropSpawnPayload p{};
            std::memcpy(&p, msg.payload.data(), sizeof(p));
            // Trust-boundary validation: any of the 18 floats (loc/rot/scale +
            // 2 vel vectors) NaN/Inf or out-of-bound -> SpawnActor +
            // SetPhysics* could crash PhysX. Reject before dispatch.
            const float vals[18] = {
                p.locX, p.locY, p.locZ,
                p.rotPitch, p.rotYaw, p.rotRoll,
                p.scaleX, p.scaleY, p.scaleZ,
                p.initLinVelX, p.initLinVelY, p.initLinVelZ,
                p.initAngVelX, p.initAngVelY, p.initAngVelZ,
                0.f, 0.f, 0.f  // pad
            };
            bool finite = true;
            for (int i = 0; i < 15; ++i) {  // skip the 3 pad slots
                if (!std::isfinite(vals[i])) { finite = false; break; }
            }
            if (!finite) {
                UE_LOGW("event_feed: PropSpawn floats non-finite -- dropping");
                break;
            }
            constexpr float kMaxCoord = 1.0e6f;
            constexpr float kMaxVel   = 1.0e6f;
            if (std::fabs(p.locX) > kMaxCoord || std::fabs(p.locY) > kMaxCoord ||
                std::fabs(p.locZ) > kMaxCoord) {
                UE_LOGW("event_feed: PropSpawn location out of bounds (%.1f, %.1f, %.1f)",
                        p.locX, p.locY, p.locZ);
                break;
            }
            if (std::fabs(p.initLinVelX) > kMaxVel || std::fabs(p.initLinVelY) > kMaxVel ||
                std::fabs(p.initLinVelZ) > kMaxVel ||
                std::fabs(p.initAngVelX) > kMaxVel || std::fabs(p.initAngVelY) > kMaxVel ||
                std::fabs(p.initAngVelZ) > kMaxVel) {
                UE_LOGW("event_feed: PropSpawn velocity out of bounds");
                break;
            }
            // Clamp class/key lengths defensively (they're uint8 but the
            // sender could lie). 63/31 are the struct caps.
            if (p.className.len > 63) {
                UE_LOGW("event_feed: PropSpawn className.len=%u > 63 -- dropping", p.className.len);
                break;
            }
            if (p.key.len > 31) {
                UE_LOGW("event_feed: PropSpawn key.len=%u > 31 -- dropping", p.key.len);
                break;
            }
            // Phase 5N Stream B (2026-05-24): drop wire-spawns of
            // intermediate-variant classes that the receiver doesn't want
            // (mushroom7_C growing state). Host-authoritative growth
            // pipeline -- the mature variant (mushroom_C) will arrive when
            // host's transform-timer fires. Mirrors the role==Client +
            // IsClientSuppressedPropClass check in harness.cpp::
            // GrabObserver_Aprop_Init_POST so the suppression is symmetric:
            // never spawn locally AND never accept wire spawns of these.
            {
                std::wstring cls;
                cls.reserve(p.className.len);
                for (uint8_t i = 0; i < p.className.len; ++i) {
                    cls.push_back(static_cast<wchar_t>(static_cast<unsigned char>(p.className.data[i])));
                }
                if (cls == ue_wrap::profile::name::PropMushroomGrowingClass) {
                    UE_LOGI("event_feed: PropSpawn drop -- intermediate-variant class '%ls' suppressed on this peer (host-authoritative; mature variant will arrive when host transforms)",
                            cls.c_str());
                    break;
                }
            }
            remote_prop::OnSpawn(p);
            break;
        }
        case net::ReliableKind::PropDestroy: {
            // v5 Inc2: peer destroyed a prop -- delete the matching local
            // actor (resolved via FindByKeyString). Receiver-side
            // K2_DestroyActor is echo-suppressed via the incoming-destroy
            // set so it doesn't bounce back to the sender.
            if (msg.payload.size() < sizeof(net::PropDestroyPayload)) {
                UE_LOGW("event_feed: PropDestroy payload too short (%zu < %zu)",
                        msg.payload.size(), sizeof(net::PropDestroyPayload));
                break;
            }
            net::PropDestroyPayload p{};
            std::memcpy(&p, msg.payload.data(), sizeof(p));
            if (p.key.len > 31) {
                UE_LOGW("event_feed: PropDestroy key.len=%u > 31 -- dropping", p.key.len);
                break;
            }
            // 2026-05-25 cross-peer destroy: pass localPlayer so OnDestroy
            // can release a held PHC grab (mainPlayer.grabbing_actor ==
            // doomed) before K2_DestroyActor. Prevents UPhysicsHandle
            // Component::TickComponent reading a dangling GrabbedComponent
            // ptr next frame.
            remote_prop::OnDestroy(p, localPlayer);
            break;
        }
        }
    }
}

}  // namespace coop::event_feed
