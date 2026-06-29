#include "coop/player/nameplate.h"

#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/voice/voice_chat.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/types.h"

#include <algorithm>
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
    constexpr float kRefCm    = 600.f;  // at/under ~6 m the plate is at base (full) size
    constexpr float kMinScale = 0.40f;  // floor ~6.4 px, reached ~15 m as the alpha fade begins
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

    // Iterate ALL peer slots (0..kMaxPeers-1), not 1.. -- the slot a peer's puppet
    // sits in is the PEER's slot, and which slot is "remote" depends on who WE are:
    // on the host the client puppet is slot 1, but on the client the HOST puppet is
    // slot 0. Puppet(ourOwnSlot) returns null (you have no puppet of yourself), so a
    // plain 0.. sweep labels every remote and skips self -- same pattern event_feed
    // uses. (Starting at 1 silently hid the host's nameplate on every client.)
    for (int slot = 0; slot < static_cast<int>(coop::players::kMaxPeers); ++slot) {
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
        pl.x = screen.X;
        pl.y = screen.Y;
        pl.flash = p->IsHurtFlashing();
        const float h = std::clamp(p->GetHealth(), 0.f, 1.f);
        pl.healthPct = static_cast<int>(std::lround(h * 100.f));
        pl.ping = p->GetPing();
        pl.voiceIcon = static_cast<uint8_t>(coop::voice_chat::IconForSlot(slot));  // v66 badge
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

}  // namespace coop::nameplate
