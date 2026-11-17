// coop/dev/oven_progress_drill.cpp -- see coop/dev/oven_progress_drill.h.

#include "coop/dev/oven_progress_drill.h"

#include "coop/config/config.h"
#include "coop/dev/drill_chat.h"
#include "coop/interactables/oven_progress_sync.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/oven_screen.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::oven_progress_drill {
namespace {

namespace OS = ue_wrap::oven_screen;
namespace DC = coop::dev::drill_chat;

enum class Arm { Off, Host, Client, Join };

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::oven_progress_drill);
        return v == "host" ? Arm::Host : v == "client" ? Arm::Client : v == "join" ? Arm::Join : Arm::Off;
    }();
    return a;
}

bool Host() { return coop::roster::LocalIsHost(); }
char Who() { return Host() ? 'H' : 'C'; }
bool LocalActs() { return (ArmOf() == Arm::Client) != Host(); }

// The steps the acting peer raises on its screen, one at a time: a screw, the other three, the lid.
constexpr uint8_t  kSteps[]     = {OS::kScrewTL, OS::kScrewTR | OS::kScrewBL | OS::kScrewBR, OS::kLid};
constexpr int      kStepCount   = 3;
constexpr uint64_t kStepBoundMs = 10000;
constexpr uint64_t kJoinJudgeMs = 6000;
const char* const  kSayMask     = "oven mask ";

ue_wrap::CachedObjRef g_oven;
int      g_step = 0;        // the next step to raise, or the judge's own count
uint64_t g_stepMs = 0;
bool     g_raisedStep = false;
bool     g_done = false;
int      g_saidMask = -1;

void* Oven() {
    if (void* o = g_oven.Get()) return o;
    const std::vector<void*> ovens = OS::FindOvens();
    if (ovens.empty()) return nullptr;
    g_oven.Set(ovens.front());
    return ovens.front();
}

void Verdict(bool pass, const char* fmt, ...) {
    char msg[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (pass) UE_LOGI("[OVEN-DRILL] [%c] PASS %s", Who(), msg);
    else      UE_LOGW("[OVEN-DRILL] [%c] FAIL %s", Who(), msg);
    g_done = true;
}

bool JoinOver(coop::net::Session& s) {
    if (!Host())
        return coop::net_pump::HasAnnouncedWorldReady() &&
               coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot)
        if (s.IsSlotWorldReady(slot) && coop::prop_snapshot::IsBracketClosed(slot) &&
            !coop::join_window_baseline::IsLateWindowOpen(slot))
            return true;
    return false;
}

// The watching peer says its screen's mask whenever it changes; -1 for an oven with no screen.
void SayMaskOnChange() {
    if (!DC::PollDue()) return;
    uint8_t m = 0;
    void* oven = Oven();
    const int now = (oven && OS::TryReadProgress(oven, m)) ? m : -1;
    if (now == g_saidMask) return;
    g_saidMask = now;
    char line[40];
    std::snprintf(line, sizeof(line), "%s%d", kSayMask, now);
    DC::Say(line);
    UE_LOGI("[OVEN-DRILL] [%c] screen mask %d", Who(), now);
}

bool TheirMask(int& out) {
    std::string rest;
    return DC::SeenRestOf(kSayMask, &rest) && std::sscanf(rest.c_str(), "%d", &out) == 1;
}

// host and client: a step is raised on the acting peer's screen as a screw's turn leaves it, and must
// stand on the watching peer's before the next.
void TickActor() {
    void* oven = Oven();
    uint8_t mine = 0;
    if (!oven || !OS::TryReadProgress(oven, mine))
        return Verdict(false, "the acting peer's oven has no repair screen (repaired in this save, or not found)");
    int theirs = -2;
    if (!TheirMask(theirs)) return;
    if (!g_stepMs) {
        if (mine != 0 || theirs != 0)
            return Verdict(false, "fixture: both screens must start with every screw in; here 0x%02X, there %d", mine,
                           theirs);
        g_stepMs = coop::net::NowMs();
    }
    if (g_step >= kStepCount)
        return Verdict(true, "three steps each stood on both screens; both read 0x%02X", mine);
    if (!g_raisedStep) {
        g_raisedStep = true;
        g_stepMs = coop::net::NowMs();
        const bool ok = OS::RaiseProgress(oven, kSteps[g_step]);
        UE_LOGI("[OVEN-DRILL] [%c] step %d: raised 0x%02X on this screen (%d)", Who(), g_step, kSteps[g_step],
                ok ? 1 : 0);
        return;
    }
    if (theirs == mine) {
        ++g_step;
        g_raisedStep = false;
        return;
    }
    if (coop::net::NowMs() - g_stepMs > kStepBoundMs)
        Verdict(false, "step %d: this screen reads 0x%02X, the other %d after %llu ms", g_step, mine, theirs,
                static_cast<unsigned long long>(kStepBoundMs));
}

}  // namespace

bool IsEnabled() { return ArmOf() != Arm::Off; }

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || !session || g_done) return;
    if (ArmOf() == Arm::Join) {
        // The host's screen takes every step before anyone joins; the joiner's must hold them all a
        // moment after its join, from the connect replay alone.
        if (Host()) {
            void* oven = Oven();
            uint8_t m = 0;
            if (g_raisedStep || !oven || !OS::TryReadProgress(oven, m)) return;
            g_raisedStep = true;
            UE_LOGI("[OVEN-DRILL] [H] raised 0x%02X on the host's screen ahead of the join (%d)", OS::kAll,
                    OS::RaiseProgress(oven, OS::kAll) ? 1 : 0);
            return;
        }
        if (!session->connected() || !JoinOver(*session)) return;
        if (!g_stepMs) g_stepMs = coop::net::NowMs();
        if (coop::net::NowMs() - g_stepMs < kJoinJudgeMs) return;
        void* oven = Oven();
        uint8_t m = 0;
        if (!oven || !OS::TryReadProgress(oven, m)) return Verdict(false, "the joiner's oven has no repair screen");
        return Verdict(m == OS::kAll, "the joiner's screen reads 0x%02X (0x%02X wanted), %llu bit(s) raised from the wire",
                       m, OS::kAll, static_cast<unsigned long long>(coop::oven_progress_sync::BitsRaised()));
    }
    if (!session->connected()) return;
    DC::SeenHas(kSayMask);  // the chat history records from its first read on
    if (!JoinOver(*session)) return;
    if (LocalActs()) TickActor();
    else SayMaskOnChange();
}

void OnDisconnect() {
    g_oven.Reset();
    g_step = 0;
    g_stepMs = 0;
    g_raisedStep = false;
    g_done = false;
    g_saidMask = -1;
}

}  // namespace coop::dev::oven_progress_drill
