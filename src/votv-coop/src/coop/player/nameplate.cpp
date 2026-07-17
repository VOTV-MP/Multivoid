#include "coop/player/nameplate.h"

#include "coop/comms/chat_bubbles.h"
#include "coop/comms/chat_feed.h"
#include "coop/player/nick_color.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/player_handshake.h"
#include "coop/voice/voice_chat.h"
#include "coop/config/config.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/trace.h"   // LineBlockedStatDyn -- the occlusion (gray plate) check
#include "ue_wrap/core/types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace coop::nameplate {

namespace E = ue_wrap::engine;

namespace {

// Published snapshot (written by Update on the game thread, read by ui::hud on the
// render thread). g_count mirrors g_snap.count as a lock-free HasAny() fast-path so
// the overlay's per-frame "should I draw the HUD?" check never takes the mutex.
std::mutex        g_mu;
Snapshot          g_snap;
std::atomic<int>  g_count{0};

// v94 per-player plate visibility. ALL of it is atomic (any-thread by
// construction): the local pref is read by the render-thread F1 checkbox; the
// per-slot store is written by game-thread wire handlers AND by the bringup-
// thread session-start reset (player_handshake::Reset runs on the
// TimelineThread -- a game-thread assert here would trip on every session
// start, the hot_path_guard.h "one-shot session-start helper" trap; audit
// 2026-07-02). Stored INVERTED as hidden-by-slot so the zero-init default is
// the correct one -- absent info never hides a plate.
std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool>                g_localVisible{true};
std::array<std::atomic<bool>, coop::players::kMaxPeers> g_hiddenBySlot{};

// Distance fade (MTA nametag shape): fully opaque close up, fading to nothing far
// away so a distant peer's label doesn't clutter the screen. Pulled in 2026-06-08
// (user: "make it disappear at less distance") -- opaque within a room, gone just
// beyond it instead of trailing ~90 m across the whole base/outdoors.
float DistanceAlpha(float distCm) {
    constexpr float kFullCm = 1500.f;  // fully opaque within ~15 m
    constexpr float kGoneCm = 4000.f;  // invisible beyond ~40 m
    if (distCm <= kFullCm) return 1.f;
    if (distCm >= kGoneCm) return 0.f;
    return 1.f - (distCm - kFullCm) / (kGoneCm - kFullCm);
}

// Distance SIZE scale (MTA nametag billboard shape). The whole plate scales like a
// world-anchored billboard (~1/distance) so it stays proportional to the peer's
// shrinking on-screen body instead of looming ever larger as they recede -- the
// fixed-size plate "grew" relative to a far, tiny character (user 2026-06-08). Capped
// at 1.0 up close so a peer right next to you never gets a screen-filling label (the
// earlier 22->16 "huge up close" complaint), floored so a mid-range plate stays legible
// while DistanceAlpha fades it out.
float DistanceScale(float distCm) {
    // kRefCm 600 -> 900 (2026-07-04, user: "ники становятся плохо читаемыми
    // слишком рано -- отодвинь эту дистанцию в 1.5 раза"): full size holds to
    // ~9 m, every farther plate is 1.5x its old size, and the legibility floor
    // is reached at ~22.5 m instead of ~15 m.
    constexpr float kRefCm    = 900.f;  // at/under ~9 m the plate is at base (full) size
    constexpr float kMinScale = 0.40f;  // floor, reached ~22.5 m as the alpha fade progresses
    if (distCm <= kRefCm) return 1.f;
    return std::max(kRefCm / distCm, kMinScale);
}

void CopyNickAscii(char (&dst)[24], const std::wstring& nick) {
    size_t i = 0;
    for (; i + 1 < sizeof(dst) && i < nick.size(); ++i) {
        const wchar_t c = nick[i];
        dst[i] = (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
    }
    dst[i] = '\0';
}

void Publish(const Snapshot& s) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_snap = s;
    }
    g_count.store(s.count, std::memory_order_relaxed);
}

}  // namespace

void Update() {
    Snapshot snap;  // default count=0; built locally then published (auto-clears at menu)
    auto& reg = coop::players::Registry::Get();
    void* lp = nullptr;                 // local player -- resolved lazily WITH pc below: at the
                                        // menu (no puppets) this never touches Registry::Local(),
                                        // so the per-tick composite can't trigger rescans there
                                        // (menu-window balloon fix; the TTL bounds the rest)
    void* pc = nullptr;                 // local PlayerController -- resolved lazily (only if a puppet exists)
    ue_wrap::FVector viewer{};
    ue_wrap::FVector camera{};          // view camera world point -- the occlusion-trace origin

    // Iterate ALL peer slots (0..kMaxPeers-1), not 1.. -- the slot a peer's puppet
    // sits in is the PEER's slot, and which slot is "remote" depends on who WE are:
    // on the host the client puppet is slot 1, but on the client the HOST puppet is
    // slot 0. Puppet(ourOwnSlot) returns null (you have no puppet of yourself), so a
    // plain 0.. sweep labels every remote and skips self -- same pattern event_feed
    // uses. (Starting at 1 silently hid the host's nameplate on every client.)
    for (int slot = 0; slot < static_cast<int>(coop::players::kMaxPeers); ++slot) {
        if (g_hiddenBySlot[slot].load(std::memory_order_relaxed))
            continue;  // v94: that peer hid its own plate (synced pref)
        RemotePlayer* p = reg.Puppet(static_cast<uint8_t>(slot));
        if (!p || !p->valid()) continue;

        if (!pc) {
            // First live puppet: we need the local controller to project. No local
            // player (e.g. at the menu) -> nothing to draw; bail to an empty snapshot.
            lp = reg.Local();
            if (!lp) break;
            pc = E::GetController(lp);
            if (!pc) break;
            viewer = E::GetActorLocation(lp);
            // The occlusion trace starts at the actual VIEW camera (correct in freecam /
            // vehicle cams too); the actor's eye height is the fallback if no camera
            // manager is live yet (a zeroed read this early is the menu/possess window).
            camera = E::GetCameraLocation();
            if (camera.X == 0.f && camera.Y == 0.f && camera.Z == 0.f)
                camera = ue_wrap::FVector{viewer.X, viewer.Y, viewer.Z + 60.f};
        }

        const ue_wrap::FVector head = p->GetHeadPosition();
        ue_wrap::FVector2D screen{};
        const bool inFront = E::ProjectWorldToScreen(pc, head, screen, false);

        Plate& pl = snap.plates[snap.count];
        const float dx = head.X - viewer.X, dy = head.Y - viewer.Y, dz = head.Z - viewer.Z;
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        pl.alpha = DistanceAlpha(dist);
        pl.scale = DistanceScale(dist);
        pl.onScreen = inFront && pl.alpha > 0.02f;
        // Occlusion (minecraft nametag shape, user 2026-07-04): world geometry between
        // the camera and the head grays the plate. The {WorldStatic, WorldDynamic} set
        // means level geometry / closed doors / props block; pawns are NEITHER type, so
        // the local body and the puppet itself never self-block. Unresolved (-1) -> not
        // occluded (a missing primitive must never gray every plate). Traced only while
        // the plate is actually drawable -- 0..3 one-line traces per tick.
        pl.occluded = pl.onScreen &&
                      ue_wrap::trace::LineBlockedStatDyn(lp, camera, head) == 1;
        pl.x = screen.X;
        pl.y = screen.Y;
        pl.flash = p->IsHurtFlashing();
        const float h = std::clamp(p->GetHealth(), 0.f, 1.f);
        pl.healthPct = static_cast<int>(std::lround(h * 100.f));
        pl.ping = p->GetPing();
        pl.voiceIcon = static_cast<uint8_t>(coop::voice_chat::IconForSlot(slot));  // v66 badge
        pl.colorRGB = coop::nick_color::PackedForSlot(slot);  // v103 (12f): custom nick color
        pl.bubbleAlpha = coop::chat_bubbles::BubbleForSlot(slot, pl.bubble);  // 12g overhead bubble
        CopyNickAscii(pl.nick, p->GetNickname());

        ++snap.count;
        if (snap.count >= static_cast<int>(coop::players::kMaxPeers)) break;
    }

    Publish(snap);
}

void GetSnapshot(Snapshot& out) {
    std::lock_guard<std::mutex> lk(g_mu);
    out = g_snap;
}

bool HasAny() {
    return g_count.load(std::memory_order_relaxed) > 0;
}

// ---- v94 per-player visibility pref (see header) ----

void SetInitialLocalVisible(bool visible) {
    g_localVisible.store(visible, std::memory_order_relaxed);
}

bool LocalVisible() {
    return g_localVisible.load(std::memory_order_relaxed);
}

void RequestLocalVisible(bool visible) {
    // Render thread (the F1 checkbox). Persist NOW (WriteIniValue is
    // thread-safe/atomic-swap); state + announce hop to the game thread --
    // the RequestSkin discipline.
    coop::config::WriteIniValue("nameplate", visible ? "1" : "0");
    ue_wrap::game_thread::Post([visible] {
        g_localVisible.store(visible, std::memory_order_relaxed);
        UE_LOGI("nameplate: local plate -> %s (persisted; announcing)",
                visible ? "VISIBLE" : "HIDDEN");
        if (coop::net::Session* s = g_session.load(std::memory_order_acquire))
            coop::player_handshake::AnnounceLocalNameplate(*s, visible);
        coop::chat_feed::Push(visible ? L"Nameplate: shown to other players"
                                      : L"Nameplate: hidden from other players");
    });
}

void StoreVisibleForSlot(int slot, bool visible) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    const bool hidden = !visible;
    if (g_hiddenBySlot[slot].exchange(hidden, std::memory_order_relaxed) != hidden)
        UE_LOGI("nameplate: slot %d plate -> %s", slot, visible ? "VISIBLE" : "HIDDEN");
}

bool VisibleForSlot(int slot) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return true;
    return !g_hiddenBySlot[slot].load(std::memory_order_relaxed);
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void ResetSlots() {
    // Session-start reset -- runs on the BRINGUP thread (TimelineThread) via
    // player_handshake::Reset; the store is atomic, so no game-thread hop needed.
    for (auto& h : g_hiddenBySlot) h.store(false, std::memory_order_relaxed);
}

void OnSlotDisconnected(int slot) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    // A reused slot must not inherit the departed peer's pref.
    g_hiddenBySlot[slot].store(false, std::memory_order_relaxed);
}

}  // namespace coop::nameplate
