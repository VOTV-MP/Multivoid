// coop/dev/remover_drill_host.cpp -- the host's half of the remover drill; see coop/dev/remover_drill.h.
// The state and helpers both halves use are declared in remover_drill_detail.h.

#include "remover_drill_detail.h"

#include "coop/comms/chat_log.h"
#include "coop/comms/chat_sync.h"
#include "coop/dev/director/director.h"
#include "coop/dev/hook_plant.h"
#include "coop/dev/remover_drill_layout.h"
#include "coop/element/registry.h"
#include "coop/items/hook_sync.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/props/remover_request.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace coop::dev::remover_drill::detail {
namespace {

namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
using ue_wrap::FVector;
namespace layout = coop::dev::remover_drill::layout;
using layout::kRing;

constexpr uint64_t kAwaitBoundMs = 900000;  // HOST: the spawn to the client's closing line
constexpr float kHostHookReachCm = 300.f;   // HOST: how near the first crowbar its hook is planted from

enum class HostStep : uint8_t { Arm, Await };
HostStep g_hostStep = HostStep::Arm;

// The host's half of the hook steps, in order: its hook planted on the first crowbar (after the walk to it), the line
// said once the claim pass reads that hook, its hook released once the host has answered Held, then the wait for the
// client's hook on the second.
enum class HookStep : uint8_t { Plant, Pulling, Answering, AwaitClient, Done };
HookStep g_hookStep = HookStep::Plant;
ue_wrap::CachedObjRef g_hostHook;
uint32_t g_heldBase = 0;   // the host's Held answers when it said host-hooked
bool g_noEidWarned = false;   // the Pulling step's "no eid yet" warning, once per layout
// The host's walk to the first crowbar: the hook's tail sits on the host's player, so a player far off reels the
// crowbar across the map before the client presses. Started by the Plant step, dropped once it ends or restarts.
std::shared_ptr<coop::director::BackgroundWalk> g_hostWalk;

void DropHostWalk() {
    if (g_hostWalk && g_hostWalk->state.load() == 0) coop::director::CancelBackgroundWalk(*g_hostWalk);
    g_hostWalk.reset();
}

// The host's verdict or abandon: the walk is dropped with it, so the host's player is never driven past the drill's end.
void HostEnd(const char* word, const char* what, bool warn) {
    DropHostWalk();
    End("host", word, what, warn);
}

bool ClientEnded() {
    const std::string end = EndLine();
    bool found = false;
    coop::chat_log::ForEach([&](const coop::chat_log::Row& r) {
        if (r.slot == 1 && r.text == end) found = true;
    });
    return found;
}

// The host's own count of its answers to slot 1, by reason.
uint32_t Answered(coop::net::RemoverReason r) { return coop::remover_request::AnsweredFor(1, r); }

// The prop is gone on the host AND the host performed it: the answers it gave are the host's own word, so a removal
// the client did natively and the host merely applied cannot pass.
void HostJudge() {
    const layout::Laid& laid = layout::Targets();
    using RR = coop::net::RemoverReason;
    char why[240];
    const uint32_t removed = Answered(RR::Removed);
    const uint32_t noRight = Answered(RR::NoRight);
    const uint32_t held = Answered(RR::Held);
    const uint32_t other = Answered(RR::NotReady) + Answered(RR::Unsupported) + Answered(RR::OutOfSight) +
                           Answered(RR::Failed);
    if (PhaseOf() == Phase::Withheld) {
        if (laid.single.Get() && removed == 0 && noRight >= 1)
            HostEnd("PASS", "the prop the host's permissions refuse stands here; NoRight was answered", false);
        else {
            std::snprintf(why, sizeof(why), "refused prop standing=%d, Removed answered %u, NoRight answered %u",
                          laid.single.Get() ? 1 : 0, removed, noRight);
            HostEnd("FAIL", why, true);
        }
        return;
    }
    int ringGone = 0;
    for (const auto& r : laid.ring)
        if (!r.Get()) ++ringGone;
    const bool single = !laid.single.Get(), mash = !laid.mash.Get(), npc = laid.npc.Get() != nullptr;
    // One Removed for the single prop, one for the mashed one (the rest of its presses were answered Gone), one per ring
    // prop, one for the crowbar the client's own hook pulls; the sweep's presses were each judged in sight, the creature
    // was answered Unsupported, and the crowbar the host's hook pulls was answered Held once.
    const uint32_t wantRemoved = 3u + static_cast<uint32_t>(ringGone);
    if (single && mash && ringGone >= kSweepFloor && npc && removed == wantRemoved && held == 1 &&
        other == Answered(RR::Unsupported) && Answered(RR::Unsupported) >= 1) {
        std::snprintf(why, sizeof(why), "the single prop, the mashed one and %d of %d ring props are gone here; the "
                      "host answered Removed %u times and Held once; the creature stands", ringGone, kRing, removed);
        HostEnd("PASS", why, false);
        return;
    }
    std::snprintf(why, sizeof(why),
                  "single gone=%d mash gone=%d ring gone=%d (want at least %d) creature standing=%d; answered Removed %u "
                  "(want %u), OutOfSight %u, Failed %u, NotReady %u, Held %u, Unsupported %u",
                  single ? 1 : 0, mash ? 1 : 0, ringGone, kSweepFloor, npc ? 1 : 0, removed, wantRemoved,
                  Answered(RR::OutOfSight), Answered(RR::Failed), Answered(RR::NotReady), held, Answered(RR::Unsupported));
    HostEnd("FAIL", why, true);
}

// The host's side of the two hook steps, once per check, after the layout stands. A relay re-lay has destroyed the
// crowbars, so a hook planted on the old ones is released and the steps start over on the new ones.
void HostHookTick() {
    const layout::Laid& laid = layout::Targets();
    void* player = coop::players::Registry::Get().Local();
    void* first = laid.hostHooked.Get();
    void* second = laid.selfHooked.Get();
    constexpr uint8_t kHostBit = 1u << 0, kClientBit = 1u << 1;
    switch (g_hookStep) {
    case HookStep::Plant: {
        if (!player || !first) return;
        if (!g_hostWalk) {
            FVector at{};
            if (!E::TryGetActorLocation(first, at)) return;
            g_hostWalk = coop::director::StartBackgroundWalk(at, kHostHookReachCm, kHostHookWalkDeadlineS);
            UE_LOGI("[REMOVER-DRILL] host: walking to the first crowbar before it hooks it");
            return;
        }
        const int walked = g_hostWalk->state.load();
        if (walked == 0) return;
        if (walked == 2) { HostEnd("FAIL", "the host never reached the crowbar it hooks", true); return; }
        g_hostWalk.reset();
        const char* note = "";
        void* hook = coop::dev::hook_plant::Plant(player, first, &note);
        if (!hook) { HostEnd("FAIL", note, true); return; }
        g_hostHook.Set(hook);
        UE_LOGI("[REMOVER-DRILL] host: its own hook is planted on the first crowbar (hook %p)", hook);
        g_hookStep = HookStep::Pulling;
        return;
    }
    case HookStep::Pulling: {
        if (!first || !(coop::hook_sync::PlayerHookHolders(first) & kHostBit)) return;
        // The elements the host's registry holds for both crowbars: the client binds its roles by them.
        const coop::element::ElementId firstEid = coop::element::Registry::Get().EidForActor(first);
        const coop::element::ElementId secondEid = coop::element::Registry::Get().EidForActor(second);
        if (firstEid == coop::element::kInvalidId || secondEid == coop::element::kInvalidId) {
            if (!g_noEidWarned) {
                g_noEidWarned = true;
                UE_LOGW("[REMOVER-DRILL] host: the host's crowbar has no eid yet; host-hooked waits");
            }
            return;
        }
        g_heldBase = Answered(coop::net::RemoverReason::Held);
        coop::chat_sync::QueueSend(HostHookedLine(layout::RelaysDone(), firstEid, secondEid));
        UE_LOGI("[REMOVER-DRILL] host: the claim pass reads its hook on the first crowbar; host-hooked said");
        g_hookStep = HookStep::Answering;
        return;
    }
    case HookStep::Answering:
        if (Answered(coop::net::RemoverReason::Held) < g_heldBase + 1) return;
        ReleaseHook(g_hostHook);
        UE_LOGI("[REMOVER-DRILL] host: the first crowbar was answered Held; its hook is released");
        g_hookStep = HookStep::AwaitClient;
        return;
    case HookStep::AwaitClient:
        if (!second || !(coop::hook_sync::PlayerHookHolders(second) & kClientBit)) return;
        coop::chat_sync::QueueSend(HookedLine(kSelfHookedSaid, layout::RelaysDone()));
        UE_LOGI("[REMOVER-DRILL] host: the claim pass reads the client's hook on the second crowbar; self-hooked said");
        g_hookStep = HookStep::Done;
        return;
    case HookStep::Done:
        return;
    }
}

}  // namespace

void HostTick(coop::net::Session* s) {
    if (g_done) return;
    const uint64_t now = ::GetTickCount64();
    if (now < g_nextCheckMs) return;
    g_nextCheckMs = now + kCheckEveryMs;
    if (g_hostStep == HostStep::Arm) {
        // The client's join and its late window are over, so what is spawned reaches it as a live spawn.
        if (!s->IsSlotWorldReady(1) || !coop::prop_snapshot::IsBracketClosed(1) ||
            coop::join_window_baseline::IsLateWindowOpen(1))
            return;
        // The layout is spawned only once the host holds a posed puppet for the client, and the client presses only
        // after it finds the layout: by then the host can place the client's eye. Before its first pose a puppet
        // stands at its spawn point.
        coop::RemotePlayer& rp = coop::puppet_drive::Puppet(1);
        void* puppet = rp.valid() && rp.HasPose() ? rp.GetActor() : nullptr;
        FVector at{};
        if (!puppet || !R::IsLive(puppet) || !E::TryGetActorLocation(puppet, at)) return;
        if (const char* why = layout::Lay(rp, at, PhaseOf() == Phase::Granted)) { HostEnd("ABANDONED", why, true); return; }
        UE_LOGI("[REMOVER-DRILL] host: the layout is spawned about the client's puppet (%s)", PhaseName());
        g_hostStep = HostStep::Await;
        g_stepMs = now;
        return;
    }
    const layout::Relayed relayed = layout::HandleRelay(PhaseOf() == Phase::Granted);
    if (relayed == layout::Relayed::NoEye) {
        HostEnd("ABANDONED", "the sender's eye did not read for a relay", true);
        return;
    }
    if (relayed != layout::Relayed::None) {
        ReleaseHook(g_hostHook);
        DropHostWalk();
        g_noEidWarned = false;
        g_hookStep = HookStep::Plant;
    }
    if (ClientEnded()) { HostJudge(); return; }
    if (PhaseOf() == Phase::Granted) {
        HostHookTick();
        if (g_done) return;
    }
    if (Expired(kAwaitBoundMs)) HostEnd("ABANDONED", "the client's closing line never came", true);
}

void HostReset() {
    g_hostStep = HostStep::Arm;
    g_hookStep = HookStep::Plant;
    g_heldBase = 0;
    g_noEidWarned = false;
    DropHostWalk();
    if (g_hostHook.Raw()) ReleaseHook(g_hostHook);
}

}  // namespace coop::dev::remover_drill::detail
