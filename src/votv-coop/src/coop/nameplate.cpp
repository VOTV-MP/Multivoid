#include "coop/nameplate.h"

#include "coop/players_registry.h"
#include "coop/remote_player.h"
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
// away so a distant peer's label doesn't clutter the screen. Tuned for the
// observatory interior (peers usually < 25 m) with a long tail outdoors.
float DistanceAlpha(float distCm) {
    constexpr float kFullCm = 2500.f;  // fully opaque within ~25 m
    constexpr float kGoneCm = 9000.f;  // invisible beyond ~90 m
    if (distCm <= kFullCm) return 1.f;
    if (distCm >= kGoneCm) return 0.f;
    return 1.f - (distCm - kFullCm) / (kGoneCm - kFullCm);
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
    void* lp = reg.Local();
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
        pl.onScreen = inFront && pl.alpha > 0.02f;
        pl.x = screen.X;
        pl.y = screen.Y;
        pl.flash = p->IsHurtFlashing();
        const float h = std::clamp(p->GetHealth(), 0.f, 1.f);
        pl.healthPct = static_cast<int>(std::lround(h * 100.f));
        pl.ping = p->GetPing();
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
