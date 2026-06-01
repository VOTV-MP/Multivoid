#include "coop/nameplate.h"

#include "coop/players_registry.h"
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
    void* nickBlock = nullptr;     // line 1 (white): nick + ping -- re-driven on nick/ping change
    void* barBlock  = nullptr;     // line 2 (dark red): health bar -- re-driven on health change (null in single-block fallback)
    std::wstring lastNick;         // last nickname value we composed (change-gate input)
    int          lastPing = -1;    // last ping value we composed (-1 = never composed)
    int          lastHealthPct = -1;  // last health % we composed (-1 = never composed)
};

// Resting colours (2026-06-01, user): nick stays translucent white; the health
// bar is its own DARK RED line (a single UTextBlock can't two-tone, so the
// nameplate is a UVerticalBox of two blocks). The damage flash pulses BOTH lines
// bright red, then each restores to its resting colour. Alpha = per-line opacity.
const ue_wrap::FLinearColor kNickColor{1.f, 1.f, 1.f, 0.22f};      // translucent white (unchanged nick look)
const ue_wrap::FLinearColor kBarColor{0.5f, 0.02f, 0.02f, 0.80f};  // dark red; a touch more opaque so the bars read
const ue_wrap::FLinearColor kFlashColor{1.f, 0.08f, 0.08f, 0.85f}; // bright damage pop (both lines)

// The remote player's streamed health (0..1 fraction, v19 PoseSnapshot) as an
// integer percent 0..100. Gating the label on the integer percent (not the raw
// float) avoids re-composing on sub-percent jitter from quantization.
int HealthPct(const RemotePlayer* p) {
    const float h = std::clamp(p->GetHealth(), 0.f, 1.f);
    return static_cast<int>(std::lround(h * 100.f));
}

// Line 1 (nick block, white): "<nick>" or "<nick> (<ping>ms)" if ping > 0.
std::wstring ComposeNick(const std::wstring& nick, int ping) {
    std::wstring out = nick;
    if (ping > 0) {
        wchar_t suffix[24] = {};
        std::swprintf(suffix, sizeof(suffix) / sizeof(suffix[0]), L" (%dms)", ping);
        out += suffix;
    }
    return out;
}

// Line 2 (bar block, dark red): a 10-segment bar -- filled '|' for current
// health, faint '.' for the rest. NO "HP" label and NO percent (user 2026-06-01:
// "only the bars, dark red"); ASCII '|'/'.' so it renders in any font. The
// streamed health is display-only (see the v19 PoseSnapshot doc). Callers gate on
// healthPct change so this only allocates when the integer percent actually moves.
std::wstring ComposeBar(int healthPct) {
    const int pct = std::clamp(healthPct, 0, 100);
    const int filled = (pct + 5) / 10;  // round to the nearest of 10 segments
    wchar_t bar[11] = {};
    for (int i = 0; i < 10; ++i) bar[i] = (i < filled) ? L'|' : L'.';
    return std::wstring(bar);
}
std::vector<Entry> g_entries;
void* g_viewer = nullptr;  // cached local player (avoid a GUObjectArray walk every Update)

}  // namespace

void Register(RemotePlayer* player) {
    if (!player) return;
    if (std::any_of(g_entries.begin(), g_entries.end(),
                    [&](const Entry& e) { return e.player == player; }))
        return;

    const ue_wrap::FVector at = player->GetHeadPosition();  // actor pivot + 30 cm Z (see RemotePlayer::GetHeadPosition)
    // Two-line translucent nameplate via a world-space UWidgetComponent (we build
    // our own UMG; the text alpha gives the translucency). Line 1 = nick (white),
    // line 2 = health bar (dark red) -- two UTextBlocks in a UVerticalBox so each
    // carries its own colour.
    const std::wstring& nick = player->GetNickname();
    const int           ping = player->GetPing();
    const int           healthPct = HealthPct(player);
    const std::wstring  nickLine = ComposeNick(nick, ping);
    const std::wstring  barLine  = ComposeBar(healthPct);
    void* nickTb = nullptr;
    void* barTb  = nullptr;
    void* label = E::SpawnNameplateWidget(at, nickLine.c_str(), kNickColor,
                                          barLine.c_str(), kBarColor, &nickTb, &barTb);
    g_entries.push_back({player, label, nickTb, barTb, nick, ping, healthPct});
    UE_LOGI("nameplate: label nick='%ls' bar='%ls' actor=%p nick_tb=%p bar_tb=%p for player %p (now %zu)",
            nickLine.c_str(), barLine.c_str(), label, nickTb, barTb, player, g_entries.size());
    Update();  // place + face immediately
}

// Vitals Inc3 damage flash. The nameplate is just translucent text (no
// background), so "the whole nameplate red" = tinting the UTextBlock color. The
// flash uses a brighter alpha than the resting 0.22 so the hit reads clearly;
// the restore returns to the spawn opacity. SetWidgetText (health-bar refresh)
// only touches the text string, never the color, so the two never fight.
void SetFlash(RemotePlayer* player, bool on) {
    // Damage pop: both lines flash bright red, then each restores to its RESTING
    // colour (nick -> translucent white, bar -> dark red). On = bright red on
    // both; off = per-line resting colour.
    const ue_wrap::FLinearColor nickC = on ? kFlashColor : kNickColor;
    const ue_wrap::FLinearColor barC  = on ? kFlashColor : kBarColor;
    for (auto& e : g_entries) {
        if (e.player != player) continue;
        // Mirror Update()/Unregister()'s liveness guard: the nameplate actor (and
        // its UTextBlock subobjects, built into the actor's widget tree) can be
        // GC-freed on a level change WITHOUT our Unregister running, while the
        // puppet actor_ that gates SetFlash via RemotePlayer::valid() is still live.
        // SetTextBlockColor is a RAW memory store, so a freed block would be a
        // use-after-free -- guard on the actor's liveness (audit 2026-05-31).
        if (e.textActor && R::IsLive(e.textActor)) {
            if (e.nickBlock) E::SetTextBlockColor(e.nickBlock, nickC);
            if (e.barBlock)  E::SetTextBlockColor(e.barBlock,  barC);
        }
        return;
    }
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
    // 2026-05-26 (RULE 1 unification): use the central
    // coop::players::Registry to identify the local player. The
    // registry's Local() is filtered + cached; we no longer walk
    // GUObjectArray here. Identity comes from ONE place across the
    // codebase. The viewer for billboarding nameplates is always the
    // LOCAL player.
    g_viewer = coop::players::Registry::Get().Local();
    const ue_wrap::FVector viewer =
        g_viewer ? E::GetActorLocation(g_viewer) : ue_wrap::FVector{};

    for (auto& e : g_entries) {
        if (!e.player || !e.player->valid() || !e.textActor) continue;
        if (!R::IsLive(e.textActor)) continue;  // label destroyed (level change)
        const ue_wrap::FVector at = e.player->GetHeadPosition();  // actor pivot + 30 cm Z
        E::SetActorLocation(e.textActor, at);

        // Face the viewer: point the WidgetComponent quad's +X toward the
        // viewer (its normal is +X, IDA-confirmed). 2026-05-25 hands-on fix:
        // use the puppet's ACTOR pivot (stable; coincides with head crown per
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

        // Refresh the label if any input changed since last push:
        //   1) Nick: spawn-time placeholder "..." -> real nick once the Join
        //      reliable msg lands (event_feed::Update -> SetNickname).
        //   2) Ping: event_feed forwards Session::lastRttMs each tick; the
        //      displayed value tracks RTT (updates ~1 Hz).
        //   3) Health %: v19 vitals stream in on every PoseSnapshot
        //      (RemotePlayer::SetVitals); the integer percent drives the bar.
        // We gate on (nick, ping, healthPct) compare BEFORE composing -- so a
        // steady label costs only one wstring compare + two int compares per
        // entry per tick, ZERO allocations (the alloc-heavy Compose* only run
        // when something actually changed). The two lines are driven on SEPARATE
        // UTextBlocks (nick + bar) so each keeps its own colour.
        if (e.nickBlock) {
            const int curPing = e.player->GetPing();
            const std::wstring& curNick = e.player->GetNickname();
            const int curHealthPct = HealthPct(e.player);
            // Nick line. In the single-block fallback (barBlock null) the nick
            // block holds BOTH lines, so re-compose the combined string and gate
            // on health too (else a nick refresh would wipe the bar).
            const bool nickChanged = (curPing != e.lastPing || curNick != e.lastNick);
            if (nickChanged || (!e.barBlock && curHealthPct != e.lastHealthPct)) {
                std::wstring nickLine = ComposeNick(curNick, curPing);
                if (!e.barBlock) { nickLine += L"\n"; nickLine += ComposeBar(curHealthPct); }
                E::SetWidgetText(e.nickBlock, nickLine.c_str());
                e.lastNick = curNick;
                e.lastPing = curPing;
                if (!e.barBlock) e.lastHealthPct = curHealthPct;
            }
            // Bar line (its own dark-red block).
            if (e.barBlock && curHealthPct != e.lastHealthPct) {
                const std::wstring barLine = ComposeBar(curHealthPct);
                E::SetWidgetText(e.barBlock, barLine.c_str());
                e.lastHealthPct = curHealthPct;
            }
        }
    }
}

}  // namespace coop::nameplate
