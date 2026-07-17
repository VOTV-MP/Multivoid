// coop/desk_input_sync.cpp -- see coop/interactables/desk_input_sync.h.

#include "coop/interactables/desk_input_sync.h"

#include "coop/interactables/desk_snd_fx.h"
#include "coop/net/session.h"

#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <chrono>
#include <cmath>

namespace coop::desk_input_sync {
namespace {

namespace CD = ue_wrap::console_desk;
using coop::net::DeskInputField;

using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr auto kPoll = std::chrono::milliseconds(250);
Clock::time_point g_nextPoll{};

// The per-field poll baseline. Primed on first sight / adopt / every wire
// apply (field-granular) -- a wire-caused change never reads as a local edge.
CD::Scalars g_baseline;
bool g_primed = false;

// HOST: the slot whose ping FSM is currently running (0 = the host itself,
// 0xFF = none). v116 root fix: coord_isPing is a RUN-FLAG -- the native ping
// FSM is a latent tick machine gated on it (analogd uber @82980 IFNOT(
// coord_isPing) -> the @80105 stage engine), so a wire apply into the machine
// WAKES a phantom parallel sim on the receiver (measured 2026-07-17: the
// host's phantom armed at 14:47:38 while the client's real ping failed).
// The wire delta is BOOKKEEPING ONLY now: rising arms this attribution +
// the desk-claim deny (device_occupancy consults PingActiveSlot()); falling
// clears it. The machine field is written ONLY by the local native FSM.
uint8_t g_pingSetterSlot = 0xFF;

bool g_scanWidgetWarned = false;  // log-once for the spawnDirs null-guard

void SendDelta(coop::net::Session* s, DeskInputField field,
               float fv, int32_t iv, bool bv) {
    coop::net::DeskInputPayload p{};
    p.field = static_cast<uint8_t>(field);
    p.boolVal = bv ? 1 : 0;
    p.floatVal = fv;
    p.intVal = iv;
    if (s->role() == coop::net::Role::Host)
        s->SendReliable(coop::net::ReliableKind::DeskInput, &p, sizeof(p));
    else
        s->SendReliableToSlot(0, coop::net::ReliableKind::DeskInput, &p, sizeof(p));
}

void SendScan(coop::net::Session* s, float observed) {
    coop::net::DeskScanEventPayload p{};
    p.observedCooldown = observed;
    if (s->role() == coop::net::Role::Host)
        s->SendReliable(coop::net::ReliableKind::DeskScanEvent, &p, sizeof(p));
    else
        s->SendReliableToSlot(0, coop::net::ReliableKind::DeskScanEvent, &p, sizeof(p));
}

// Which unit index (0..3) an active_* field id maps to (ApplyActiveToggleEffects).
int ActiveUnitOf(DeskInputField f) {
    switch (f) {
    case DeskInputField::ActivePlay:     return 0;
    case DeskInputField::ActiveDownload: return 1;
    case DeskInputField::ActiveCoords:   return 2;
    case DeskInputField::ActiveComp:     return 3;
    default:                             return -1;
    }
}

// Patch ONE field's value into a Scalars struct (pure; no engine access).
// Shared by the wire apply and the echo-prime baseline advance.
bool PatchScalar(const coop::net::DeskInputPayload& p, CD::Scalars& sc) {
    switch (static_cast<DeskInputField>(p.field)) {
    case DeskInputField::FrFilterSpeed:   sc.dlFrFilterSpeed = p.floatVal; break;
    case DeskInputField::PoFilterSpeed:   sc.dlPoFilterSpeed = p.floatVal; break;
    case DeskInputField::FrFilterActive:  sc.dlActiveFrFilter = p.boolVal != 0; break;
    case DeskInputField::PoFilterActive:  sc.dlActivePoFilter = p.boolVal != 0; break;
    case DeskInputField::PolarityDir:     sc.dlPolarityDir = p.intVal; break;
    case DeskInputField::PlayVolume:      sc.playVolume = p.intVal; break;
    case DeskInputField::PlaySelectIndex: sc.playSelectIndex = p.intVal; break;
    case DeskInputField::CompMaxLevel:    sc.compMaxLevel = p.intVal; break;
    case DeskInputField::ActivePlay:      sc.activePlay = p.boolVal != 0; break;
    case DeskInputField::ActiveDownload:  sc.activeDownload = p.boolVal != 0; break;
    case DeskInputField::ActiveCoords:    sc.activeCoords = p.boolVal != 0; break;
    case DeskInputField::ActiveComp:      sc.activeComp = p.boolVal != 0; break;
    // CoordIsPing deliberately ABSENT (v116): it is the ping FSM's run-flag --
    // patching it into any scalar set that reaches WriteScalars would wake the
    // phantom sim. OnDeskInput intercepts it as bookkeeping before ApplyField.
    case DeskInputField::CooldownCharge:  sc.coordCooldown = p.floatVal; break;
    default: return false;
    }
    return true;
}

// Apply ONE field onto the local desk: patch the scalar set + run the proven
// WriteScalars upd* chain, then the field's native setter side effects where
// the chain doesn't cover them (hums/lights/live volume; measured [1113-1156]).
bool ApplyField(const coop::net::DeskInputPayload& p, CD::Scalars& sc) {
    if (!PatchScalar(p, sc)) return false;
    if (!CD::WriteScalars(sc)) return false;
    const int unit = ActiveUnitOf(static_cast<DeskInputField>(p.field));
    if (unit >= 0) CD::ApplyActiveToggleEffects(unit, p.boolVal != 0);
    if (static_cast<DeskInputField>(p.field) == DeskInputField::PlayVolume)
        CD::ApplyPlayVolumeEffects(p.intVal);
    return true;
}

// The one poll: diff the INPUT fields against the baseline; ship deltas.
void PollOnce(coop::net::Session* s) {
    CD::Scalars cur;
    if (!CD::ReadScalars(cur)) return;
    if (!g_primed) {  // first sight: prime only, never send (join-window safety)
        g_baseline = cur;
        g_primed = true;
        return;
    }

    // Discrete/int/float INPUT fields (one delta per changed field).
    if (cur.dlFrFilterSpeed != g_baseline.dlFrFilterSpeed && std::isfinite(cur.dlFrFilterSpeed))
        SendDelta(s, DeskInputField::FrFilterSpeed, cur.dlFrFilterSpeed, 0, false);
    if (cur.dlPoFilterSpeed != g_baseline.dlPoFilterSpeed && std::isfinite(cur.dlPoFilterSpeed))
        SendDelta(s, DeskInputField::PoFilterSpeed, cur.dlPoFilterSpeed, 0, false);
    if (cur.dlActiveFrFilter != g_baseline.dlActiveFrFilter)
        SendDelta(s, DeskInputField::FrFilterActive, 0, 0, cur.dlActiveFrFilter);
    if (cur.dlActivePoFilter != g_baseline.dlActivePoFilter)
        SendDelta(s, DeskInputField::PoFilterActive, 0, 0, cur.dlActivePoFilter);
    if (cur.dlPolarityDir != g_baseline.dlPolarityDir)
        SendDelta(s, DeskInputField::PolarityDir, 0, cur.dlPolarityDir, false);
    if (cur.playVolume != g_baseline.playVolume)
        SendDelta(s, DeskInputField::PlayVolume, 0, cur.playVolume, false);
    if (cur.playSelectIndex != g_baseline.playSelectIndex)
        SendDelta(s, DeskInputField::PlaySelectIndex, 0, cur.playSelectIndex, false);
    if (cur.compMaxLevel != g_baseline.compMaxLevel)
        SendDelta(s, DeskInputField::CompMaxLevel, 0, cur.compMaxLevel, false);
    if (cur.activePlay != g_baseline.activePlay)
        SendDelta(s, DeskInputField::ActivePlay, 0, 0, cur.activePlay);
    if (cur.activeDownload != g_baseline.activeDownload)
        SendDelta(s, DeskInputField::ActiveDownload, 0, 0, cur.activeDownload);
    if (cur.activeCoords != g_baseline.activeCoords)
        SendDelta(s, DeskInputField::ActiveCoords, 0, 0, cur.activeCoords);
    if (cur.activeComp != g_baseline.activeComp)
        SendDelta(s, DeskInputField::ActiveComp, 0, 0, cur.activeComp);
    if (cur.coordIsPing != g_baseline.coordIsPing) {
        SendDelta(s, DeskInputField::CoordIsPing, 0, 0, cur.coordIsPing);
        if (s->role() == coop::net::Role::Host) {
            if (cur.coordIsPing) g_pingSetterSlot = 0;       // the host's own press
            else if (g_pingSetterSlot == 0) g_pingSetterSlot = 0xFF;  // its FSM ended
        }
    }

    // COOLDOWN: only an UPWARD jump is a press charge (decay is local; all
    // charging verbs are measured cooldown<=0-gated, so downward writes don't
    // exist). The same jump classifies the SHIFT scan by its unique full-charge
    // signature (dots charge to maxCooldown/2; ping never touches it).
    if (std::isfinite(cur.coordCooldown) && cur.coordCooldown > g_baseline.coordCooldown) {
        SendDelta(s, DeskInputField::CooldownCharge, cur.coordCooldown, 0, false);
        float maxCd = 0.f;
        if (CD::ReadMaxCooldown(maxCd) && maxCd > 0.f) {
            const bool scan = cur.coordCooldown > (maxCd * 0.5f + 0.01f);
            UE_LOGI("desk_input: cooldown charge %.2f->%.2f verdict=%s (maxCd=%.2f)",
                    g_baseline.coordCooldown, cur.coordCooldown,
                    scan ? "SCAN" : "DOT", maxCd);
            if (scan) SendScan(s, cur.coordCooldown);
        } else {
            UE_LOGW("desk_input: cooldown charge %.2f->%.2f but maxCooldown unresolved -- "
                    "scan classification skipped", g_baseline.coordCooldown, cur.coordCooldown);
        }
    }

    g_baseline = cur;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    const auto now = Clock::now();
    if (now < g_nextPoll) return;
    g_nextPoll = now + kPoll;
    if (!CD::EnsureResolved() || !CD::Instance()) return;
    if (!s->connected()) {
        // Keep the baseline current while unwired so a (re)connect never
        // bursts stale diffs.
        CD::Scalars cur;
        if (CD::ReadScalars(cur)) { g_baseline = cur; g_primed = true; }
        return;
    }
    PollOnce(s);
}

void OnDeskInput(const coop::net::DeskInputPayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (p.field >= static_cast<uint8_t>(DeskInputField::Count)) return;
    if (!std::isfinite(p.floatVal)) return;

    // v116 ROOT FIX: CoordIsPing is NEVER applied to the machine. The native
    // ping FSM is a latent tick machine gated on coord_isPing (@82980) with
    // ==1.0 stage latches inside (@79979) -- a raw write here woke a PHANTOM
    // parallel sim on every observer (divergent verdicts, double coordLog
    // authorship, the 14:47:38 phantom ARM). The delta is bookkeeping only:
    // ping attribution for the leaver clear + the desk-claim deny.
    if (static_cast<DeskInputField>(p.field) == DeskInputField::CoordIsPing) {
        if (s->role() == coop::net::Role::Host) {
            if (p.boolVal) g_pingSetterSlot = senderSlot;
            else if (g_pingSetterSlot == senderSlot) g_pingSetterSlot = 0xFF;
        }
        return;
    }

    if (!CD::EnsureResolved() || !CD::Instance()) return;  // inherited residual: dropped at unresolved desk
    CD::Scalars sc;
    if (!CD::ReadScalars(sc)) return;
    {
        // v115: wire-caused engine writes hold the audio-seam echo guard --
        // any sound a setter side effect plays must not re-forward.
        coop::desk_snd_fx::ScopedWireApply guard;
        if (!ApplyField(p, sc)) return;
    }
    // ECHO-PRIME: the applied field (and only it) advances the baseline in the
    // same GT task -- the next poll never reads this apply as a local edge.
    if (g_primed) PatchScalar(p, g_baseline);
    else { g_baseline = sc; g_primed = true; }
}

void OnDeskScan(const coop::net::DeskScanEventPayload& p, uint8_t senderSlot) {
    (void)p; (void)senderSlot;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (!CD::EnsureResolved() || !CD::Instance()) return;
    coop::desk_snd_fx::ScopedWireApply guard;  // v115: the spawnDirs replay is wire-caused
    if (!CD::PlayScanEffects()) {
        if (!g_scanWidgetWarned) {
            g_scanWidgetWarned = true;
            UE_LOGW("desk_input: scan replay skipped -- desk screen widget not live yet (log-once)");
        }
        return;
    }
    UE_LOGI("desk_input: scan replayed (spawnDirs visual; the beep rides DeskSndFx)");
}

void PrimeBaselines() {
    CD::Scalars cur;
    if (CD::EnsureResolved() && CD::Instance() && CD::ReadScalars(cur)) {
        g_baseline = cur;
        g_primed = true;
    }
}

uint8_t PingActiveSlot() { return g_pingSetterSlot; }

void SeedPingAttributionFromMachine() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (g_pingSetterSlot != 0xFF) return;  // a live wire attribution wins
    CD::Scalars sc;
    if (!CD::EnsureResolved() || !CD::Instance() || !CD::ReadScalars(sc)) return;
    if (!sc.coordIsPing) return;
    // Only the host's own FSM can be running here: no peer was connected to
    // author a delta, and receivers never write the machine field (v115b).
    g_pingSetterSlot = 0;
    UE_LOGI("desk_input: ping attribution seeded from machine ground truth "
            "(solo-host ping caught at the connect edge)");
}

void OnPeerLeft(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (g_pingSetterSlot != static_cast<uint8_t>(slot)) return;
    // v116 (RULE 2): the machine-field clear is GONE -- no receiver wire-writes
    // coord_isPing anymore, so no peer's machine can hold a leaver's dangling
    // TRUE. Only the attribution (the desk-claim deny input) needs clearing;
    // the leaver's OWN machine died with its session.
    g_pingSetterSlot = 0xFF;
    UE_LOGI("desk_input: ping attribution cleared -- setter slot %d left mid-ping", slot);
}

void OnDisconnect() {
    // v116 (RULE 2): the session-end machine clear is GONE with the raw wire
    // write that made it necessary (audit 2026-07-16 WARN-3 covered a state
    // that can no longer exist -- a LOCAL organic ping ends itself natively).
    g_baseline = {};
    g_primed = false;
    g_pingSetterSlot = 0xFF;
    g_scanWidgetWarned = false;
    g_nextPoll = {};
}

}  // namespace coop::desk_input_sync
