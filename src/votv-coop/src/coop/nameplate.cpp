#include "coop/nameplate.h"

#include "coop/remote_player.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <vector>

namespace coop::nameplate {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

namespace {

// VOTV does not run the stock HUD canvas (ReceiveDrawHUD never fires), so a
// screen-space nametag is impossible here. Instead each player gets a 3D
// ATextRenderActor floating above its head, billboarded to face the local player.
struct Entry {
    RemotePlayer* player = nullptr;
    void* textActor = nullptr;
    void* textBlock = nullptr;     // inner UTextBlock -- re-driven on nick/ping change
    std::wstring lastNick;         // last nickname value we composed (change-gate input)
    int          lastPing = -1;    // last ping value we composed (-1 = never composed)
};

// Compose the displayed label: "<nick>" or "<nick> (<ping>ms)" if ping > 0.
// Allocates -- callers should gate on (nick, ping) change to avoid per-tick
// allocations (Update only invokes this when one of those actually moved).
std::wstring ComposeLabel(const std::wstring& nick, int ping) {
    if (ping <= 0) return nick;
    wchar_t suffix[24] = {};
    std::swprintf(suffix, sizeof(suffix) / sizeof(suffix[0]), L" (%dms)", ping);
    return nick + suffix;
}
std::vector<Entry> g_entries;
void* g_viewer = nullptr;  // cached local player (avoid a GUObjectArray walk every Update)

}  // namespace

void Register(RemotePlayer* player) {
    if (!player) return;
    if (std::any_of(g_entries.begin(), g_entries.end(),
                    [&](const Entry& e) { return e.player == player; }))
        return;

    const ue_wrap::FVector at = player->GetHeadPosition();  // actor pivot + 200 cm Z (see RemotePlayer::GetHeadPosition)
    // Translucent nameplate via a world-space UWidgetComponent (we build our own
    // UMG; the text alpha gives the translucency). 0.22 = quite see-through.
    const std::wstring& nick = player->GetNickname();
    const int           ping = player->GetPing();
    const std::wstring  initial = ComposeLabel(nick, ping);
    void* tb = nullptr;
    void* label = E::SpawnNameplateWidget(at, initial.c_str(), 0.22f, &tb);
    g_entries.push_back({player, label, tb, nick, ping});
    UE_LOGI("nameplate: label '%ls' actor=%p text=%p for player %p (now %zu)",
            initial.c_str(), label, tb, player, g_entries.size());
    Update();  // place + face immediately
}

void Unregister(RemotePlayer* player) {
    for (auto it = g_entries.begin(); it != g_entries.end(); ++it) {
        if (it->player == player) {
            // Only destroy if still live: now that DestroyActor actually fires
            // K2_DestroyActor, calling it on a label the engine already killed on a
            // level change would be a use-after-free.
            if (it->textActor && R::IsLive(it->textActor)) E::DestroyActor(it->textActor);
            g_entries.erase(it);
            return;
        }
    }
}

void Update() {
    if (g_entries.empty()) return;
    // Billboard target = the LOCAL player. Cache the pointer (FindObjectByClass
    // walks the whole GUObjectArray, too costly per frame), but drop the cache if
    // the actor was destroyed (level change) so we never read freed memory.
    //
    // 2026-05-25 hands-on fix: with the new mainPlayer_C puppet path
    // (vs the old ASkeletalMeshActor backup), the FindObjectByClass call
    // could return the PUPPET instead of the local player -- both are
    // now AmainPlayer_C. If g_viewer ends up pointing at a puppet, the
    // nameplate's "face viewer" yaw is computed against the puppet's
    // own position -> yaw is meaningless / oscillates wildly when the
    // puppet walks (= the "horizontally flipped" symptom user reported).
    //
    // Filter: exclude any actor that's registered as a puppet entry.
    if (g_viewer && !R::IsLive(g_viewer)) g_viewer = nullptr;
    // Also drop the cache if the cached actor turns out to be one of OUR
    // puppets (could happen after a puppet spawns into a previously-
    // valid slot, or the first cache happened before the puppet existed
    // and the wrong actor was selected).
    if (g_viewer) {
        for (const auto& e : g_entries) {
            if (e.player && e.player->valid() &&
                e.player->GetActor() == g_viewer) {
                g_viewer = nullptr;
                break;
            }
        }
    }
    if (!g_viewer) {
        // Walk GUObjectArray, find a mainPlayer_C that is NOT one of our
        // puppet actors. The local player spawns at game-load time and is
        // typically the first mainPlayer_C in GUObjectArray; the puppet
        // spawns later. But after a level reload or slot reuse the order
        // can flip, so we filter explicitly.
        const int32_t n = R::NumObjects();
        for (int32_t i = 0; i < n; ++i) {
            void* obj = R::ObjectAt(i);
            if (!obj) continue;
            if (R::ClassNameOf(obj) != P::name::MainPlayerClass) continue;
            const std::wstring nm = R::ToString(R::NameOf(obj));
            if (nm.rfind(L"Default__", 0) == 0) continue;
            if (!R::IsLive(obj)) continue;
            bool isPuppet = false;
            for (const auto& e : g_entries) {
                if (e.player && e.player->valid() &&
                    e.player->GetActor() == obj) { isPuppet = true; break; }
            }
            if (isPuppet) continue;
            g_viewer = obj;
            break;
        }
    }
    const ue_wrap::FVector viewer =
        g_viewer ? E::GetActorLocation(g_viewer) : ue_wrap::FVector{};

    for (auto& e : g_entries) {
        if (!e.player || !e.player->valid() || !e.textActor) continue;
        if (!R::IsLive(e.textActor)) continue;  // label destroyed (level change)
        const ue_wrap::FVector at = e.player->GetHeadPosition();  // actor pivot + 200 cm Z
        E::SetActorLocation(e.textActor, at);

        // Face the viewer: point the WidgetComponent quad's +X toward the
        // viewer (its normal is +X, IDA-confirmed). 2026-05-25 hands-on fix:
        // use the puppet's ACTOR pivot (stable, capsule center) for the
        // yaw calculation instead of the head BONE position. The head bone
        // oscillates with the AnimBP walk cycle (BlendSpace + IK perturb
        // bone world transforms each tick), which made the yaw jitter and
        // occasionally flip 180 degrees when the puppet was walking
        // (user-visible as "nameplate horizontally flipped on S press").
        // The actor pivot doesn't move with the skeleton -- yaw computed
        // from it is rock-steady regardless of animation.
        const ue_wrap::FVector puppetPivot = e.player->GetLocation();
        const float dx = viewer.X - puppetPivot.X;
        const float dy = viewer.Y - puppetPivot.Y;
        const float yaw = std::atan2(dy, dx) * 57.29578f;
        E::SetActorRotation(e.textActor, ue_wrap::FRotator{0.f, yaw, 0.f});

        // Refresh the label if either input changed since last push:
        //   1) Nick: spawn-time placeholder "..." -> real nick once the Join
        //      reliable msg lands (event_feed::Update -> SetNickname).
        //   2) Ping: event_feed forwards Session::lastRttMs each tick; the
        //      displayed value tracks RTT (updates ~1 Hz).
        // We gate on (nick, ping) integer/string compare BEFORE composing -- so
        // a steady label costs only one wstring compare + one int compare per
        // entry per tick, ZERO allocations (the alloc-heavy ComposeLabel only
        // runs when something actually changed).
        if (e.textBlock) {
            const int curPing = e.player->GetPing();
            const std::wstring& curNick = e.player->GetNickname();
            if (curPing != e.lastPing || curNick != e.lastNick) {
                const std::wstring composed = ComposeLabel(curNick, curPing);
                E::SetWidgetText(e.textBlock, composed.c_str());
                e.lastNick = curNick;
                e.lastPing = curPing;
            }
        }
    }
}

}  // namespace coop::nameplate
