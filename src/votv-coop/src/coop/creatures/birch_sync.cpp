// coop/creatures/birch_sync.cpp -- see coop/creatures/birch_sync.h.
//
// 0.9.0n. birch_C::ReceiveTick, angry and looked away from for 3 s: for each actor in its capsule that casts to
// mainPlayer_C, blood and the held item's drop, then that actor's punch; then, on later frames (Delay resumes), the
// stump and the birch's own look, WidgetBlueprintLibrary::Create(blackscreen_C) to the viewport, the save slot's
// player transform through lib getMainPlayer's teleportWoBackrooms, `sav` bound to the gamemode's gameSaved and its
// save(false, true, false). `sav` zeroes the slot's health, pauses and, 10 s on, loads the menu.

#include "coop/creatures/birch_sync.h"

#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <unordered_map>

namespace coop::birch_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowPunch, "birch_sync", L"mainPlayer_C", L"punch", Declared);
UE_FACT_FN(kRowTeleport, "birch_sync", L"mainPlayer_C", L"teleportWoBackrooms", Declared);
UE_FACT_FN(kRowSave, "birch_sync", L"mainGamemode_C", L"save", Declared);
UE_FACT_FN(kRowSav, "birch_sync", L"birch_C", L"sav", Declared);

constexpr int kTagPunch = 0x42525050;   // 'BRPP'
constexpr int kTagKill = 0x4252504B;    // 'BRPK'
constexpr uint64_t kScreenSweepMs = 4000;

std::atomic<N::Session*> g_session{nullptr};
bool g_watched = false;
std::unordered_map<void*, bool> g_hostKill;   // HOST: a birch that punched the host's own player
uint64_t g_sweepUntil = 0;                    // HOST: a body's kill left a black screen to take down

bool IsHost() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    return s && s->role() == N::Role::Host;
}

bool IsBirch(void* obj) { return obj && R::NameEquals(R::NameOf(R::ClassOf(obj)), kBirchClass); }

// HOST: whose player the birch punched. A body's punch is the delivery's (player_damage); the host's own lets the
// rest of the kill run.
sg::Verdict OnPunchPre(const sg::Call& call) {
    if (call.tag != kTagPunch || !IsHost() || !IsBirch(call.callerObject)) return sg::Verdict::Run;
    if (call.object == coop::players::Registry::Get().Local()) {
        g_hostKill[call.callerObject] = true;
        UE_LOGI("birch: the host's birch caught the host's own player -- the game's kill runs");
    } else if (coop::players::Registry::Get().IsPuppet(call.object)) {
        g_sweepUntil = ::GetTickCount64() + kScreenSweepMs;
        UE_LOGI("birch: the host's birch caught slot %u's body -- delivered to that player; the host's own kill "
                "steps are refused", coop::players::Registry::Get().PeerIdOfActor(call.object));
    }
    return sg::Verdict::Run;
}

// HOST: the teleport, the save and the menu a birch asks are the host's only when it punched the host's player.
sg::Verdict OnKillStepPre(const sg::Call& call) {
    if (call.tag != kTagKill || !IsHost()) return sg::Verdict::Run;
    void* const birch = IsBirch(call.object) ? call.object : (IsBirch(call.callerObject) ? call.callerObject : nullptr);
    if (!birch) return sg::Verdict::Run;
    auto it = g_hostKill.find(birch);
    if (it != g_hostKill.end() && it->second) return sg::Verdict::Run;
    static int s_refused = 0;
    if (++s_refused == 1 || (s_refused & (s_refused - 1)) == 0)
        UE_LOGI("birch: a kill step of a birch that caught no host player -- refused on the host (%d)", s_refused);
    return sg::Verdict::Cancel;
}

void SweepBlackScreens() {
    for (void* w : R::FindObjectsByClass(L"blackscreen_C")) {
        if (!w || !R::IsLive(w) || R::NameStartsWith(R::NameOf(w), L"Default__")) continue;
        ue_wrap::ParamFrame pf(R::FindDispatchFunction(R::ClassOf(w), L"RemoveFromParent", nullptr));
        if (pf.valid() && ue_wrap::Call(w, pf)) UE_LOGI("birch: the black screen of a body's kill taken off the host");
    }
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!g_watched) {
        bool all = sg::WatchName(kRowPunch, kTagPunch, &OnPunchPre, nullptr);
        all = sg::WatchName(kRowTeleport, kTagKill, &OnKillStepPre, nullptr) && all;
        all = sg::WatchName(kRowSave, kTagKill, &OnKillStepPre, nullptr) && all;
        all = sg::WatchName(kRowSav, kTagKill, &OnKillStepPre, nullptr) && all;
        if ((g_watched = all)) UE_LOGI("birch: the birch's punch and kill steps are watched");
    }
    if (s->role() != N::Role::Host || !g_sweepUntil) return;
    if (::GetTickCount64() > g_sweepUntil) {
        g_sweepUntil = 0;
        return;
    }
    bool hostKilled = false;
    for (const auto& kv : g_hostKill) hostKilled = hostKilled || kv.second;
    if (!hostKilled) SweepBlackScreens();
}

void OnDisconnect() {
    g_hostKill.clear();
    g_sweepUntil = 0;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::birch_sync
