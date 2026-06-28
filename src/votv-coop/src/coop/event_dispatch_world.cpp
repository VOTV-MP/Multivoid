// coop/event_dispatch_world.cpp -- the ambient / world-event reliable-kind case
// bodies (FireflySpawn / InventoryPickup / TimeSync / SkyState / RedSky /
// LightningStrike / WeatherState) + the shared VerifySenderEidRange trust helper,
// extracted VERBATIM from event_feed.cpp's Update switch (2026-06-11 modularity
// extraction; see coop/event_dispatch.h).

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/element/registry.h"

#include "coop/social/chat_sync.h"
#include "coop/world/event_cue_sync.h"
#include "coop/world/firefly_sync.h"
#include "coop/social/inventory_pickup_sync.h"
#include "coop/world/sky_sync.h"
#include "coop/world/time_sync.h"
#include "coop/world/weather_sync.h"

#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"

#include <cmath>
#include <cstring>

namespace coop::event_feed {

// PR-FOUNDATION-1 (2026-05-29): role-range validation for an inbound
// eid-carrying packet. Without this, a malicious client peer can stamp
// ItemActivate/RedSky/Lightning/Weather with the HOST's senderElementId;
// Registry::Get resolves to the host's Player Element and the receiver
// applies the packet's effect under host identity. The range partition
// makes that impersonation detectable at the wire boundary: host-role
// packets MUST carry host-range eids; client-role packets MUST carry
// peer-range eids. Returns true when the eid is in-range OR when no
// compare is possible (0 sentinel / invalid sender slot). Logs +
// returns false on out-of-range.
//
// v14 / v15 also performed an 8-bit syncContext compare here (the
// VerifySenderContext function). v16 PR-FOUNDATION-1b retired that
// layer entirely: per-peer stale-generation defense now lives in
// Session::HandleMessage's senderEpoch latch, applied uniformly to
// EVERY inbound packet by the transport layer (not per-payload by the
// receiver dispatch). This helper kept the role-range half because
// it's a wire-format trust boundary (the eid range is the sender's
// claimed role), independent of the stale-gen defense.
bool VerifySenderEidRange(int senderPeerSlot,
                          uint32_t senderElementId,
                          const char* kind) {
    if (senderElementId == 0u ||
        senderElementId == coop::element::kInvalidId) {
        return true;  // 0 sentinel; no compare. peer-slot fallback applies.
    }
    if (senderPeerSlot < 0) return true;  // unknown sender; skip range check
    const bool senderIsHost = (senderPeerSlot == 0);
    if (!coop::element::Registry::IsAllowedSenderEid(
            senderIsHost, senderElementId)) {
        UE_LOGW("event_feed: %s senderElementId=0x%08x out of allowed %s "
                "range (senderPeerSlot=%d) -- dropping (role impersonation?)",
                kind, senderElementId,
                senderIsHost ? "host" : "peer",
                senderPeerSlot);
        return false;
    }
    return true;
}

bool HandleWorldEvent(net::Session& session,
                      const net::Session::ReliableMessage& msg) {
    switch (msg.kind) {
    case net::ReliableKind::FireflySpawn: {
        // v51 (2026-06-09): PEER-SYMMETRIC ambient firefly. Any peer may originate one
        // (each runs its own spawner near its OWN camera + shares); the host relays a
        // client's spawn to the other clients (IsClientRelayableReliableKind). No trust
        // gate -- a cosmetic transient particle. The origin never receives its own send,
        // so OnReliable always materialises ANOTHER peer's firefly at its world position.
        if (msg.payloadLen < sizeof(net::FireflySpawnPayload)) {
            UE_LOGW("event_feed: FireflySpawn payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::FireflySpawnPayload));
            break;
        }
        net::FireflySpawnPayload fp{};
        std::memcpy(&fp, msg.payload, sizeof(fp));
        coop::firefly_sync::OnReliable(fp);
        break;
    }
    case net::ReliableKind::EventCue: {
        // v79 (B1): HOST-AUTHORITATIVE cosmetic emitter cue. Only the host originates one (it is
        // the sole event producer -- clients run a dormant scheduler), so this only ever runs on
        // a client and always replays ANOTHER (the host's) cue emitter. No trust gate needed --
        // a cosmetic transient particle; the wire-level senderEpoch + the host-only-send topology
        // already bound it. (event_feed trust-gates the eid-carrying state kinds, not this.)
        if (msg.payloadLen < sizeof(net::EventCuePayload)) {
            UE_LOGW("event_feed: EventCue payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::EventCuePayload));
            break;
        }
        net::EventCuePayload ep{};
        std::memcpy(&ep, msg.payload, sizeof(ep));
        coop::event_cue_sync::OnReliable(ep);
        break;
    }
    case net::ReliableKind::InventoryPickup: {
        // v58 (2026-06-11): a peer collected an item into inventory -- play the
        // native inventory_Cue blip at their broadcast position (PEER-SYMMETRIC,
        // host-relayed, cosmetic; the origin never receives its own send).
        if (msg.payloadLen < sizeof(net::InventoryPickupPayload)) {
            UE_LOGW("event_feed: InventoryPickup payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::InventoryPickupPayload));
            break;
        }
        net::InventoryPickupPayload ip{};
        std::memcpy(&ip, msg.payload, sizeof(ip));
        coop::inventory_pickup_sync::OnReliable(ip);
        break;
    }
    case net::ReliableKind::ChatMessage: {
        // v60 (2026-06-11): a peer's T-chat line. PEER-SYMMETRIC + host-relayed.
        // Identity comes from the TRANSPORT slot (a peer cannot speak as someone
        // else); the payload is text only, sanitized to printable ASCII in
        // chat_sync before display.
        if (msg.payloadLen < sizeof(net::ChatMessagePayload)) {
            UE_LOGW("event_feed: ChatMessage payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::ChatMessagePayload));
            break;
        }
        if (msg.senderPeerSlot < 0 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: ChatMessage invalid senderPeerSlot=%d -- dropping",
                    msg.senderPeerSlot);
            break;
        }
        net::ChatMessagePayload cp{};
        std::memcpy(&cp, msg.payload, sizeof(cp));
        coop::chat_sync::OnReliable(cp, static_cast<uint8_t>(msg.senderPeerSlot));
        break;
    }
    case net::ReliableKind::TimeSync: {
        // v36 (2026-06-07): HOST-authoritative world clock (time-of-day). HOST->client; the
        // client applies it to its cycle (OnReliable no-ops on the host defensively).
        // Trust gate (like every other host-only kind): only slot 0 (the host) may set the clock.
        if (msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: TimeSync from non-host senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::TimeSyncPayload)) {
            UE_LOGW("event_feed: TimeSync payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::TimeSyncPayload));
            break;
        }
        net::TimeSyncPayload tp{};
        std::memcpy(&tp, msg.payload, sizeof(tp));
        // Reject NaN/Inf / absurd values before the raw float write into the cycle struct (a NaN
        // clock -> setSunAndMoonRotation(NaN) -> black sky / FRotator assert). totalTime/day are
        // monotonic game counters (O(1e4)s per game-day); timeScale is ~1.
        if (!std::isfinite(tp.totalTime) || !std::isfinite(tp.day) || !std::isfinite(tp.timeScale) ||
            std::fabs(tp.totalTime) > 1.0e7f || std::fabs(tp.day) > 1.0e7f ||
            tp.timeScale < 0.0f || tp.timeScale > 1.0e4f) {
            UE_LOGW("event_feed: TimeSync values out of range (t=%.1f d=%.1f s=%.3f) -- dropping",
                    tp.totalTime, tp.day, tp.timeScale);
            break;
        }
        coop::time_sync::OnReliable(tp);
        break;
    }
    case net::ReliableKind::SkyState: {
        // v44 (2026-06-08): HOST-authoritative night-sky orientation + moon phase (Anewsky_C).
        // HOST->client; trust-gated to slot 0 like TimeSync. The client writes the sky mesh
        // world rotation + moonPhase (sky_sync::OnReliable no-ops on the host defensively).
        if (msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: SkyState from non-host senderPeerSlot=%d -- dropping", msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::SkyStatePayload)) {
            UE_LOGW("event_feed: SkyState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::SkyStatePayload));
            break;
        }
        net::SkyStatePayload sp{};
        std::memcpy(&sp, msg.payload, sizeof(sp));
        // NaN/Inf guard before the raw float writes (SetComponentWorldRotation(NaN) -> FRotator
        // assert / garbage transform). Rotations are bounded angles; moonPhase is a material
        // scalar. (sky_sync::OnReliable re-checks defensively too.)
        if (!std::isfinite(sp.skyPitch) || !std::isfinite(sp.skyYaw) ||
            !std::isfinite(sp.skyRoll) || !std::isfinite(sp.moonPhase)) {
            UE_LOGW("event_feed: SkyState non-finite floats -- dropping");
            break;
        }
        coop::sky_sync::OnReliable(sp);
        break;
    }
    case net::ReliableKind::RedSky: {
        // Phase 5W Inc-fix-2 (2026-05-27): one-shot/toggle red-sky
        // story-event sync. Host's POST observer on spawnRedSky +
        // redSky.set caught the change; broadcast it. Receiver
        // invokes the same chain on its local gamemode.
        if (msg.payloadLen < sizeof(net::RedSkyPayload)) {
            UE_LOGW("event_feed: RedSky payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::RedSkyPayload));
            break;
        }
        net::RedSkyPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (session.role() == net::Role::Host) {
            UE_LOGI("event_feed: RedSky received on host -- dropping");
            break;
        }
        // v13 (A4 2026-05-29): host trust-bound. RedSky is host-only;
        // a non-host senderPeerSlot is a protocol violation.
        if (msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: RedSky from non-host senderPeerSlot=%d "
                    "(senderElementId=0x%08x) -- dropping",
                    msg.senderPeerSlot, p.senderElementId);
            break;
        }
        // PR-FOUNDATION-1: role-range trust on senderElementId.
        // (v14 syncContext compare replaced by Session-layer senderEpoch in v16.)
        if (!VerifySenderEidRange(msg.senderPeerSlot, p.senderElementId,
                                   "RedSky")) {
            break;
        }
        if (p.state != 0 && p.state != 1) {
            UE_LOGW("event_feed: RedSky state=%u out of range -- dropping",
                    static_cast<unsigned>(p.state));
            break;
        }
        net::RedSkyPayload pCopy = p;
        ue_wrap::game_thread::Post([pCopy] {
            ::coop::weather_sync::ApplyRedSky(pCopy);
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
        if (msg.payloadLen < sizeof(net::LightningStrikePayload)) {
            UE_LOGW("event_feed: LightningStrike payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::LightningStrikePayload));
            break;
        }
        net::LightningStrikePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (session.role() == net::Role::Host) {
            UE_LOGI("event_feed: LightningStrike received on host -- dropping");
            break;
        }
        // v13 (A4 2026-05-29): host trust-bound. LightningStrike is
        // host-only; a non-host senderPeerSlot is a protocol violation.
        if (msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: LightningStrike from non-host "
                    "senderPeerSlot=%d (senderElementId=0x%08x) -- dropping",
                    msg.senderPeerSlot, p.senderElementId);
            break;
        }
        // PR-FOUNDATION-1: role-range trust on senderElementId.
        // (v14 syncContext compare replaced by Session-layer senderEpoch in v16.)
        if (!VerifySenderEidRange(msg.senderPeerSlot, p.senderElementId,
                                   "LightningStrike")) {
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
        if (msg.payloadLen < sizeof(net::WeatherStatePayload)) {
            UE_LOGW("event_feed: WeatherState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::WeatherStatePayload));
            break;
        }
        net::WeatherStatePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        // Self-echo guard: weather is host->client only; if our role
        // says we ARE the host, a WeatherState packet must be a loopback
        // bounce (we'd never send to ourselves but defensive). Drop.
        if (session.role() == net::Role::Host) {
            UE_LOGI("event_feed: WeatherState received on host -- dropping "
                    "(host is the authority; no inbound from client)");
            break;
        }
        // v13 (A4 2026-05-29): host trust-bound. WeatherState is
        // host-only; a non-host senderPeerSlot is a protocol violation.
        if (msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: WeatherState from non-host "
                    "senderPeerSlot=%d (senderElementId=0x%08x) -- dropping",
                    msg.senderPeerSlot, p.senderElementId);
            break;
        }
        // PR-FOUNDATION-1: role-range trust on senderElementId.
        // (v14 syncContext compare replaced by Session-layer senderEpoch in v16.)
        if (!VerifySenderEidRange(msg.senderPeerSlot, p.senderElementId,
                                   "WeatherState")) {
            break;
        }
        // Trust-boundary: validate EVERY float the receiver writes into engine memory
        // is finite + within a sane range. Rain scalars are unitless [0, ~10] (lc/dc
        // chance up to ~120 observed); fog density ~[0, 15]; wind ~[0, 50] -- a generous
        // (-1e3, 1e3) catches garbage/NaN without clamping any legit value. v43 added
        // the 4 wind floats (written raw into the wind actor via directionalwind::Write)
        // AND the v24 fog/rain floats that were applied unvalidated before -- same
        // trust boundary, all in one check now (audit 2026-06-08).
        const float vals[] = {
            p.rainStrength, p.rainLightningChance, p.rainDeactivateChance, p.rainWindSpeed,
            p.rain, p.finalFogDensity, p.fogAlpha, p.fogStrength,
            p.windSpeedBg, p.windStrengthBg, p.windSpeedRain, p.windStrengthRain
        };
        bool bad = false;
        for (float v : vals) {
            if (!std::isfinite(v) || std::fabs(v) > 1.0e3f) { bad = true; break; }
        }
        if (bad) {
            UE_LOGW("event_feed: WeatherState floats out of bounds (rain=%.2f lc=%.2f dc=%.2f "
                    "ws=%.2f fog=%.2f windBg=%.2f/%.2f windRain=%.2f/%.2f) -- dropping",
                    p.rainStrength, p.rainLightningChance, p.rainDeactivateChance,
                    p.rainWindSpeed, p.finalFogDensity, p.windSpeedBg, p.windStrengthBg,
                    p.windSpeedRain, p.windStrengthRain);
            break;
        }
        net::WeatherStatePayload pCopy = p;
        ue_wrap::game_thread::Post([pCopy] {
            ::coop::weather_sync::ApplyFromHost(pCopy);
        });
        break;
    }
    default:
        return false;  // not a world-family kind -> event_feed tries the next family
    }
    return true;  // a world-family kind was matched (processed or validation-dropped)
}

}  // namespace coop::event_feed
