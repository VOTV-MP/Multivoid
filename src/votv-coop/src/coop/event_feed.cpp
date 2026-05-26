#include "coop/event_feed.h"

#include "coop/item_activate.h"
#include "coop/net/session.h"
#include "coop/remote_player.h"
#include "coop/remote_prop.h"
#include "coop/weather_sync.h"
#include "dev/restore_vitals.h"
#include "dev/teleport_client.h"
#include "ue_wrap/game_thread.h"
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

// Nickname sanitizer (2026-05-25, VT-inspired): trust-boundary defense at
// the nameplate / hud-feed display surface. Borrowed from VoidTogether-
// Server's utilityModule.js SimplifyName (regex /^-+|-+$|[^A-Za-z0-9 ]+/g
// + truncate to 20) -- adjusted for our wchar_t pipeline and to ALLOW
// internal spaces (single-space runs collapsed; leading/trailing trimmed).
//
// Why: a peer's Join reliable payload carries an arbitrary UTF-8 byte
// string of arbitrary length. Without sanitization, a malicious or buggy
// peer could inject:
//   - Control chars (newline / null / ANSI escape) that corrupt our
//     hud_feed widget text and could in theory escape out of UMG bindings.
//   - Right-to-left override unicode (U+202E) that visually inverts the
//     subsequent text in the nameplate.
//   - Combining diacritics that render glyphs taller than the widget bg.
//   - Very long strings that overflow the floating nameplate beyond the
//     screen and waste reliable-channel bandwidth on join.
//
// Pattern: keep ASCII alphanumerics + space + the safe punctuation set
// `[-_.]`. Strip everything else. Collapse multi-space runs to single.
// Trim leading/trailing whitespace + dashes. Truncate to 20 wchars max.
// Empty result falls back to "Player" so the nameplate isn't blank.
//
// This applies SYMMETRICALLY to both the inbound Join receive (defense
// against peer-side garbage) AND to our own outbound SetLocalNickname
// (defense against env-var typos / Windows path leakage / our own
// future bugs).
//
// NOT a profanity filter -- VoidTogether uses obscenity.js for that
// (450 KB of regexes + transformers). Out of scope for the standalone
// C++ mod; a strict-character sanitizer + length cap is the trust-
// boundary fix, profanity moderation is a separate moderation feature
// pending Phase 6+ (per the VT adoption findings ranked shortlist).
constexpr size_t kMaxNickLen = 20;
std::wstring SanitizeNickname(const std::wstring& raw) {
    std::wstring out;
    out.reserve(raw.size());
    bool lastWasSpace = true;  // primes the leading-space trim
    for (wchar_t c : raw) {
        const bool isAlnum = (c >= L'0' && c <= L'9') ||
                             (c >= L'A' && c <= L'Z') ||
                             (c >= L'a' && c <= L'z');
        const bool isSafePunct = (c == L'-' || c == L'_' || c == L'.');
        const bool isSpace = (c == L' ');
        if (isAlnum || isSafePunct) {
            out.push_back(c);
            lastWasSpace = false;
        } else if (isSpace && !lastWasSpace) {
            out.push_back(L' ');
            lastWasSpace = true;
        }
        // else: strip silently (control chars, unicode, punctuation, etc.)
        if (out.size() >= kMaxNickLen) break;
    }
    // Trim trailing space.
    while (!out.empty() && (out.back() == L' ' || out.back() == L'-'))
        out.pop_back();
    // Trim leading dashes (the SimplifyName regex's ^-+ rule -- VT
    // didn't trim leading space because their regex stripped all space
    // implicitly; we kept internal spaces so leading-space is already
    // gone via the `lastWasSpace=true` prime).
    size_t start = 0;
    while (start < out.size() && out[start] == L'-') ++start;
    if (start > 0) out.erase(0, start);
    return out.empty() ? std::wstring(L"Player") : out;
}

}  // namespace

void SetLocalNickname(const std::wstring& nick) {
    // VT-inspired sanitize-on-input (2026-05-25): symmetric defense.
    // Sanitizing here too means our env-var setup (VOTVCOOP_NET_NICK)
    // can't accidentally send garbage over the wire that we then
    // sanitize on the OTHER end -- net is cleaner if both ends agree
    // on the displayable form.
    if (!nick.empty()) g_localNick = SanitizeNickname(nick);
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
            // VT-inspired nickname sanitizer (2026-05-25, see
            // SanitizeNickname doc above). Trust-boundary defense: this
            // string CAME FROM A PEER over UDP and is going to land in
            // our floating UMG nameplate + the hud_feed widget. Without
            // sanitization a hostile peer could newline-inject our
            // widget text or insert a RLO unicode override to mirror
            // the rest of the nameplate. Length-cap to 20 wchars caps
            // the worst-case widget overflow.
            nick = SanitizeNickname(nick);
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
            //
            // TRUST BOUNDARY (audit fix #4, 2026-05-25): with bidirectional
            // destroy, CLIENT can command HOST to destroy any prop by
            // wire-Key. Acceptable for LAN coop (trusted peers); review
            // before Internet coop -- a malicious client could replay
            // crafted Keys to destroy host's quest items. Mitigation if
            // needed: authority model (host validates destroy requests
            // against current world state / quest progress).
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
        case net::ReliableKind::EntitySpawn: {
            // Phase 5N1 Inc2 (2026-05-25): NPC spawn dispatch from host.
            // Inc2 scope is DETECTION-ONLY: log the receive, validate the
            // payload shape, but do NOT yet materialize a local NPC mirror.
            // Inc3 will add the spawn path:
            //   void* cls = R::FindClass(payload.className.data);
            //   harness::MarkIncomingNpcSpawn(cls);
            //   engine::BeginDeferredActorSpawnFromClass(cls, transform);
            //   engine::FinishSpawningActor(actor, transform);
            //   harness::TrackNpcMirror(payload.sessionId, actor);
            //
            // The reason for the staged rollout: the existing Inc1 client-
            // side interceptor would block our own spawn unless the
            // MarkIncomingNpcSpawn bypass slot is set. That bypass is
            // wired (Inc1) but never EXERCISED until Inc3 ships. We log
            // here so the user can verify the host actually broadcasts
            // EntitySpawn when they flip VOTVCOOP_NPC_SYNC=1 on their
            // host instance.
            if (msg.payload.size() < sizeof(net::EntitySpawnPayload)) {
                UE_LOGW("event_feed: EntitySpawn payload too short (%zu < %zu)",
                        msg.payload.size(), sizeof(net::EntitySpawnPayload));
                break;
            }
            net::EntitySpawnPayload p{};
            std::memcpy(&p, msg.payload.data(), sizeof(p));
            if (p.className.len > 63) {
                UE_LOGW("event_feed: EntitySpawn className.len=%u > 63 -- dropping",
                        p.className.len);
                break;
            }
            std::wstring cls(p.className.len, L'\0');
            for (size_t i = 0; i < p.className.len; ++i)
                cls[i] = static_cast<wchar_t>(p.className.data[i]);
            UE_LOGI("event_feed[Inc2 detection-only]: received EntitySpawn class='%ls' sessionId=%u loc=(%.0f, %.0f, %.0f) rot=(p=%.1f y=%.1f r=%.1f) -- Inc3 will materialize this",
                    cls.c_str(), p.sessionId, p.locX, p.locY, p.locZ,
                    p.rotPitch, p.rotYaw, p.rotRoll);
            break;
        }
        case net::ReliableKind::EntityDestroy: {
            // Phase 5N1 Inc2 (2026-05-25): NPC destroy dispatch from host.
            // Inc2 scope is detection-only. Inc3 wires the mirror teardown.
            if (msg.payload.size() < sizeof(net::EntityDestroyPayload)) {
                UE_LOGW("event_feed: EntityDestroy payload too short (%zu < %zu)",
                        msg.payload.size(), sizeof(net::EntityDestroyPayload));
                break;
            }
            net::EntityDestroyPayload p{};
            std::memcpy(&p, msg.payload.data(), sizeof(p));
            UE_LOGI("event_feed[Inc2 detection-only]: received EntityDestroy sessionId=%u -- Inc3 will destroy the local mirror",
                    p.sessionId);
            break;
        }
        case net::ReliableKind::RestoreVitals: {
            // 2026-05-25 LATE +5h (F3 dev key): peer pressed F3 to refill
            // food/sleep/health/coffeePower. No payload to validate -- the
            // action is fixed. Idempotent so an echo bounce is harmless.
            ue_wrap::game_thread::Post([] { ::dev::restore_vitals::ApplyLocally(); });
            break;
        }
        case net::ReliableKind::TeleportClient: {
            // 2026-05-25 LATE +5h (F4 dev key): host snapshotted its pose and
            // sent it; client applies to local mainPlayer. Host echo is
            // a no-op below.
            if (msg.payload.size() < sizeof(net::TeleportClientPayload)) {
                UE_LOGW("event_feed: TeleportClient payload too short (%zu < %zu)",
                        msg.payload.size(), sizeof(net::TeleportClientPayload));
                break;
            }
            net::TeleportClientPayload p{};
            std::memcpy(&p, msg.payload.data(), sizeof(p));
            // Trust-boundary validation (same defensive pattern as
            // PropRelease velocity check at line 196 above): reject NaN/Inf
            // before the engine call. UE's K2_TeleportTo with a NaN
            // location asserts inside FSweepData::ClampSweepParameters.
            const float vals[6] = {p.locX, p.locY, p.locZ, p.rotPitch, p.rotYaw, p.rotRoll};
            bool finite = true;
            for (float v : vals) { if (!std::isfinite(v)) { finite = false; break; } }
            if (!finite) {
                UE_LOGW("event_feed: TeleportClient payload non-finite -- dropping");
                break;
            }
            // AABB bound (audit-fix 2026-05-25 LATE +5h): finite-check alone
            // allows extreme-but-finite coords (e.g. 1e30) that would still
            // assert inside the engine's teleport math. Mirror the project's
            // own kMaxCoord = 1.0e6f trust boundary from ValidatePose /
            // PropSpawnPayload receiver -- one consistent magnitude rule for
            // any world-position payload. Rotations don't need a magnitude
            // bound because FRotator components are angles (any value is
            // normalized inside K2_TeleportTo).
            if (std::fabs(p.locX) > net::kMaxCoord ||
                std::fabs(p.locY) > net::kMaxCoord ||
                std::fabs(p.locZ) > net::kMaxCoord) {
                UE_LOGW("event_feed: TeleportClient location out of bounds (%.1f,%.1f,%.1f) -- dropping",
                        p.locX, p.locY, p.locZ);
                break;
            }
            // Host echo gate: if WE are the host, this packet originated from
            // us (broadcast bounced back via the reliable channel). Applying
            // would teleport host to its own pose -- harmless but pointless.
            // Skip explicitly so we don't accidentally collide with whatever
            // host was doing the moment it pressed F4.
            if (session.role() == net::Role::Host) {
                UE_LOGI("event_feed: TeleportClient self-echo on host -- no-op");
                break;
            }
            ::dev::teleport_client::ApplyArgs args{
                p.locX, p.locY, p.locZ,
                p.rotPitch, p.rotYaw, p.rotRoll,
            };
            ue_wrap::game_thread::Post([args] { ::dev::teleport_client::ApplyLocally(args); });
            break;
        }
        case net::ReliableKind::ItemActivate: {
            // Phase 5F flashlight (and future radio/torch/lamp) -- peer's
            // item state changed and produces a WORLD effect both peers
            // must see. For Case (b) flashlight: apply to the puppet's
            // light_R. RE doc: research/findings/votv-flashlight-RE-
            // 2026-05-25.md.
            if (msg.payload.size() < sizeof(net::ItemActivatePayload)) {
                UE_LOGW("event_feed: ItemActivate payload too short (%zu < %zu)",
                        msg.payload.size(), sizeof(net::ItemActivatePayload));
                break;
            }
            net::ItemActivatePayload p{};
            std::memcpy(&p, msg.payload.data(), sizeof(p));
            // Trust-boundary: state is a uint8 but only 0/1 are valid.
            if (p.state != 0 && p.state != 1) {
                UE_LOGW("event_feed: ItemActivate state=%u out of range -- dropping",
                        static_cast<unsigned>(p.state));
                break;
            }
            // Self-echo guard: if peerSessionId says the sender was US, the
            // packet must be a loopback bounce and applying it would toggle
            // the WRONG puppet's light (the only puppet is the remote's).
            // Mirrors TeleportClient self-echo handling above. peerSessionId
            // convention in 1v1: host=0, client=1.
            const uint8_t selfId =
                (session.role() == net::Role::Host) ? 0u : 1u;
            if (p.peerSessionId == selfId) {
                UE_LOGI("event_feed: ItemActivate self-echo (peerSessionId=%u) -- dropping",
                        static_cast<unsigned>(p.peerSessionId));
                break;
            }
            // 1v1: the puppet is the only remote. May be null if this
            // packet beat the first PoseSnapshot (the puppet is spawned
            // lazily on first pose; ItemActivate rides the reliable
            // channel and CAN arrive first under a connect-edge burst).
            // Inc5: hand to ApplyToPuppetOrDefer which stashes the
            // payload if the puppet isn't ready; TickConnect drains it
            // once the puppet appears in the registry.
            void* puppet = (remote && remote->valid()) ? remote->GetActor() : nullptr;
            net::ItemActivatePayload pCopy = p;
            const uint8_t peerId = p.peerSessionId;
            ue_wrap::game_thread::Post([peerId, puppet, pCopy] {
                ::coop::item_activate::ApplyToPuppetOrDefer(peerId, puppet, pCopy);
            });
            break;
        }
        case net::ReliableKind::LightningStrike: {
            // Phase 5W Inc2 (2026-05-27): discrete strike event. Host's
            // POST observer on BeginDeferredActorSpawnFromClass caught
            // an AlightningStrike_C spawn (BP-internal SpawnActor inside
            // AdaynightCycle_C::timerLightning) and broadcast the
            // strike's world location. Client suppressed its own
            // timerLightning via Inc1's interceptor so no local strike
            // happened; this packet drives the visual.
            if (msg.payload.size() < sizeof(net::LightningStrikePayload)) {
                UE_LOGW("event_feed: LightningStrike payload too short (%zu < %zu)",
                        msg.payload.size(), sizeof(net::LightningStrikePayload));
                break;
            }
            net::LightningStrikePayload p{};
            std::memcpy(&p, msg.payload.data(), sizeof(p));
            if (session.role() == net::Role::Host) {
                UE_LOGI("event_feed: LightningStrike received on host -- dropping");
                break;
            }
            // Trust boundary: validate loc finite + within sane bounds.
            if (!std::isfinite(p.locX) || !std::isfinite(p.locY) || !std::isfinite(p.locZ) ||
                std::fabs(p.locX) > coop::net::kMaxCoord ||
                std::fabs(p.locY) > coop::net::kMaxCoord ||
                std::fabs(p.locZ) > coop::net::kMaxCoord) {
                UE_LOGW("event_feed: LightningStrike loc out of bounds (%.0f, %.0f, %.0f) -- dropping",
                        p.locX, p.locY, p.locZ);
                break;
            }
            net::LightningStrikePayload pCopy = p;
            ue_wrap::game_thread::Post([pCopy] {
                ::coop::weather_sync::ApplyLightningStrike(pCopy);
            });
            break;
        }
        case net::ReliableKind::WeatherState: {
            // Phase 5W Inc1 (2026-05-26): host-authoritative weather state.
            // Sender = host. Receiver looks up local AdaynightCycle_C and
            // invokes the cycle's mutator UFunctions to apply each delta.
            // See coop/weather_sync.cpp::ApplyFromHost for the full apply
            // logic + research/findings/votv-weather-DESIGN-2026-05-26.md.
            if (msg.payload.size() < sizeof(net::WeatherStatePayload)) {
                UE_LOGW("event_feed: WeatherState payload too short (%zu < %zu)",
                        msg.payload.size(), sizeof(net::WeatherStatePayload));
                break;
            }
            net::WeatherStatePayload p{};
            std::memcpy(&p, msg.payload.data(), sizeof(p));
            // Self-echo guard: weather is host->client only; if our role
            // says we ARE the host, a WeatherState packet must be a loopback
            // bounce (we'd never send to ourselves but defensive). Drop.
            if (session.role() == net::Role::Host) {
                UE_LOGI("event_feed: WeatherState received on host -- dropping "
                        "(host is the authority; no inbound from client)");
                break;
            }
            // Trust-boundary: validate floats finite + within sane range.
            // Rain scalars are unitless in [0, ~10] in BP usage; allow a
            // generous (-1e3, 1e3) range to catch garbage without
            // legitimately clamping anything.
            const float vals[4] = {
                p.rainStrength, p.rainLightningChance,
                p.rainDeactivateChance, p.rainWindSpeed
            };
            bool bad = false;
            for (float v : vals) {
                if (!std::isfinite(v) || std::fabs(v) > 1.0e3f) { bad = true; break; }
            }
            if (bad) {
                UE_LOGW("event_feed: WeatherState scalars out of bounds (rain=%.2f "
                        "lc=%.2f dc=%.2f ws=%.2f) -- dropping",
                        p.rainStrength, p.rainLightningChance,
                        p.rainDeactivateChance, p.rainWindSpeed);
                break;
            }
            net::WeatherStatePayload pCopy = p;
            ue_wrap::game_thread::Post([pCopy] {
                ::coop::weather_sync::ApplyFromHost(pCopy);
            });
            break;
        }
        default: {
            // Audit-fix 2026-05-25 LATE +5h: log-and-drop unknown ReliableKind
            // values instead of silently discarding. A peer running a newer
            // protocol could send a kind we don't yet handle; this surfaces
            // the gap in the log rather than letting it look like nothing
            // happened. Existing pattern across the project.
            UE_LOGW("event_feed: unknown ReliableKind %u -- dropping",
                    static_cast<unsigned>(msg.kind));
            break;
        }
        }
    }
}

}  // namespace coop::event_feed
