// harness/autotest_kwisp_probe.cpp -- killerwisp-vs-peers ACQUISITION PROBE (2026-07-03).
//
// User report: "killer wisp игнорит пиров ... особый kill sequence" -- the June v72 chain
// (SpawnKillerWispOnClient -> wisp acquires the PUPPET as Target -> inRange rising edge ->
// wisp_attack_sync RelayGrab -> WispGrab/WispTear) reportedly never manifests in play.
// READ-ONLY probe (probe-don't-guess): spawn a killerwisp ON the client puppet through the
// existing dev path, teleport the HOST outside the wisp's 5000u acquire radius (so nearest-
// pick cannot mask the question), then sample the wisp's REAL FSM fields every 2 s:
//   kwisp_probe: t=NN target=puppet|host|npc|null|other harmless=X tryGrab=X grab=X
//                killed=X dPuppet=NNNN dTarget=NNNN inRange=X
// Where the chain breaks is read straight off the series:
//   - target never 'puppet'      -> ACQUISITION broken (scanForActors/allow-set/LOS live gap)
//   - target=puppet, no inRange  -> the wisp never closes to 550u (movement/AI gap)
//   - inRange=1, no RELAYED line -> the wisp_attack_sync trigger/relay gap
//   - "wisp_attack: RELAYED grab" in THIS log + client wisp_tear lines -> relay fine,
//     the gap is FIDELITY (no lift/socket -- the special kill sequence's missing half).
// Gated by env VOTVCOOP_RUN_KWISP_PROBE=1 (host-only; client observes via wire).

#include "harness/autotest.h"

#include "coop/creatures/npc_sync.h"
#include "coop/dev/spawn_npc.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/config/config.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/wisp.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <memory>
#include <string>

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;

// Post `body` to the game thread and wait (the autotest_chippile pattern).
template <typename Fn>
int RunGT(Fn&& body) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, body]() mutable { body(*done); });
    while (done->load() == 0) ::Sleep(5);
    return done->load();
}

void* LocalPlayerPawn() {
    for (void* p : R::FindObjectsByClass(L"mainPlayer_C")) {
        if (p && R::IsLive(p) && E::GetController(p)) return p;
    }
    return nullptr;
}

void* FirstLiveKillerWisp() {
    for (void* w : R::FindObjectsByClass(L"killerwisp_C")) {
        if (w && R::IsLive(w)) return w;
    }
    return nullptr;
}

float Dist(void* a, void* b) {
    if (!a || !b) return -1.f;
    const ue_wrap::FVector va = E::GetActorLocation(a);
    const ue_wrap::FVector vb = E::GetActorLocation(b);
    const float dx = va.X - vb.X, dy = va.Y - vb.Y, dz = va.Z - vb.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

void RunAutonomousKwispProbe() {
    const std::string roleEnv = coop::config::ReadEnv("VOTVCOOP_NET_ROLE");
    if (roleEnv == "client") {
        UE_LOGI("kwisp_probe: not host -- client just stands (its puppet is the bait); verify in "
                "THIS log: wisp_tear/WispGrab lines IF the relay fires");
        return;
    }
    UE_LOGI("kwisp_probe: starting on host (waiting 55 s for world + client transport)");
    ::Sleep(55000);

    // 1. Resolve host pawn + slot-1 puppet; teleport the host outside the 5000u acquire
    // radius. POLL up to ~95 s more (first-launch shader compiles / a loaded machine can
    // push the client's world-up well past the 55 s settle -- the 2026-07-03 late-night
    // re-run aborted at exactly this line on a fixed sleep).
    struct Setup { void* host = nullptr; void* puppet = nullptr; bool ok = false; };
    auto setup = std::make_shared<Setup>();
    // 64 x 3 s (~3.2 min past the settle): a cold FRESH-client join = world load + 21 MB
    // save transfer + reload; the 2026-07-03 night runs blew the old ~96 s window twice.
    for (int attempt = 0; attempt < 64 && !setup->ok; ++attempt) {
        RunGT([setup](std::atomic<int>& d) {
            setup->host = LocalPlayerPawn();
            coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(1);
            setup->puppet = rp ? rp->GetActor() : nullptr;
            if (setup->host && setup->puppet && R::IsLive(setup->puppet)) {
                const ue_wrap::FVector ploc = E::GetActorLocation(setup->puppet);
                const ue_wrap::FVector away{ploc.X + 8000.f, ploc.Y, ploc.Z + 100.f};
                const bool tp = E::TeleportTo(setup->host, away, ue_wrap::FRotator{0.f, 180.f, 0.f});
                UE_LOGI("kwisp_probe: host teleported 8000u from the puppet (ok=%d) -- outside the "
                        "5000u acquire radius; the puppet is the ONLY eligible player-class target", tp ? 1 : 0);
                setup->ok = true;
            }
            d.store(1);
        });
        if (!setup->ok) ::Sleep(3000);
    }
    if (!setup->ok) {
        UE_LOGW("kwisp_probe: VERDICT ABORT -- host pawn=%p puppet=%p (client never joined "
                "within the poll window)", setup->host, setup->puppet);
        return;
    }
    ::Sleep(1500);  // let the teleport settle

    // 2. Spawn the killerwisp ON the puppet through the existing dev path (posts to GT itself).
    coop::dev::spawn_npc::SpawnKillerWispOnClient();
    ::Sleep(3000);

    // 3. Sample the FSM every 2 s for 90 s.
    bool acquiredPuppetEver = false, grabEver = false, inRangeEver = false;
    for (int t = 0; t < 45; ++t) {
        auto row = std::make_shared<std::string>();
        auto flags = std::make_shared<int>(0);  // bit0 target==puppet, bit1 grab|tryGrab, bit2 inRange
        RunGT([row, flags, setup](std::atomic<int>& d) {
            void* w = FirstLiveKillerWisp();
            if (!w) { *row = "wisp=NONE (not spawned / despawned)"; d.store(1); return; }
            ue_wrap::wisp::State st{};
            if (!ue_wrap::wisp::ReadState(w, st)) { *row = "wisp state UNREADABLE"; d.store(1); return; }
            const char* tgt = "null";
            if (st.target && R::IsLive(st.target)) {
                if (coop::players::Registry::Get().IsPuppet(st.target))     tgt = "PUPPET";
                else if (coop::players::Registry::Get().IsLocal(st.target)) tgt = "host";
                else if (coop::npc_sync::GetNpcIdForActor(st.target) !=
                         coop::element::kInvalidId)                          tgt = "npc";
                else                                                         tgt = "other";
            }
            const bool inRange = st.target && R::IsLive(st.target) &&
                                 ue_wrap::wisp::InGrabRange(w, st.target);
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "target=%s harmless=%d tryGrab=%d grab=%d killed=%d dPuppet=%.0f "
                          "dTarget=%.0f inRange=%d",
                          tgt, st.harmless ? 1 : 0, st.tryGrab ? 1 : 0, st.grab ? 1 : 0,
                          st.killed ? 1 : 0, Dist(w, setup->puppet),
                          (st.target && R::IsLive(st.target)) ? Dist(w, st.target) : -1.f,
                          inRange ? 1 : 0);
            *row = buf;
            if (tgt[0] == 'P') *flags |= 1;
            if (st.grab || st.tryGrab) *flags |= 2;
            if (inRange) *flags |= 4;
            d.store(1);
        });
        UE_LOGI("kwisp_probe: t=%03ds %s", t * 2, row->c_str());
        acquiredPuppetEver |= (*flags & 1) != 0;
        grabEver           |= (*flags & 2) != 0;
        inRangeEver        |= (*flags & 4) != 0;
        ::Sleep(2000);
    }

    UE_LOGI("kwisp_probe: DONE -- acquiredPuppet=%d inRange=%d grabFlag=%d; cross-check this log "
            "for 'wisp_attack: RELAYED grab' and the CLIENT log for wisp_tear lines. Break point: "
            "no-acquire -> acquisition; acquire-no-range -> movement; range-no-relay -> trigger",
            acquiredPuppetEver ? 1 : 0, inRangeEver ? 1 : 0, grabEver ? 1 : 0);
}

DWORD WINAPI KwispProbeThread(LPVOID /*arg*/) {
    RunAutonomousKwispProbe();
    return 0;
}

}  // namespace harness::autotest
