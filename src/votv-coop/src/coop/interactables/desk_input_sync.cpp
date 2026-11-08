// coop/interactables/desk_input_sync.cpp -- see coop/interactables/desk_input_sync.h.

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
// 0xFF = none). The wire delta is bookkeeping only -- rising arms this attribution
// and the desk-claim deny (device_occupancy reads PingActiveSlot()), falling
// clears it -- because the flag it carries is the run-flag of a latent tick
// machine, and writing it into a receiver would wake a phantom parallel sim there.
// The machine field is written only by the local native FSM.
uint8_t g_pingSetterSlot = 0xFF;
// HOST: a client's verdict the host's machine runs for it (desk_ping_sync), kept apart from g_pingSetterSlot, which
// that client's own deltas write: its own next ping may start before the host's run ends, so one variable would let
// the run's falling edge clear the ping's attribution. The slot is pushed at the priming and taken by the machine's
// next rising edge, and the run is that client's until the falling edge.
uint8_t g_nextPrimedSlot = 0xFF;
uint8_t g_primedRunSlot = 0xFF;

bool g_scanWidgetWarned = false;  // log-once for the spawnDirs null-guard

// A signal-view mode a wire apply could not draw because the desk's screen widget was not live; Tick
// applies it once the widget is, and only then moves the baseline (the desk's flag still holds the old
// mode until then, so the poll sees no local edge).
bool g_visPending = false;
bool g_visPendingValue = false;
bool g_visDeferWarned = false;
bool g_remapPending = false;  // the join adopt's updRemap repaint, owed until the widget is live
bool g_capLabelPending = false;  // the cap label's repaint, owed until the widget is live

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
    // CoordIsPing is deliberately absent: it is the ping FSM's run-flag, and patching
    // it into a scalar set that reaches WriteScalars would wake the phantom sim.
    // OnDeskInput intercepts it as bookkeeping before ApplyField.
    case DeskInputField::CooldownCharge:  sc.coordCooldown = p.floatVal; break;
    case DeskInputField::RemapMin:        sc.remapMin = p.floatVal; break;
    case DeskInputField::RemapMax:        sc.remapMax = p.floatVal; break;
    case DeskInputField::VisualizerMode:  sc.visualizerMode = p.boolVal != 0; break;
    default: return false;
    }
    return true;
}

// The painters ONE changed scalar needs, read off the verbs' bytecode (the map lives on the verb
// array in ue_wrap/desk/console_desk.cpp). Naming them per field rather than running the whole
// chain is not an optimisation: `updToggles` nulls the local player's lookAtComponent, so a verb
// dispatched for a field it does not paint rebuilds every action button under that peer's
// crosshair for nothing. `kPaintNone` is a real answer -- no verb reads coord_cooldown, and the
// input poll ships a charge delta as often as every 250 ms while a peer holds the scan.
uint32_t PaintersFor(DeskInputField f) {
    switch (f) {
    case DeskInputField::FrFilterActive:
    case DeskInputField::PoFilterActive:  return CD::kPaintToggles;
    case DeskInputField::PolarityDir:     return CD::kPaintPolarity | CD::kPaintPolarityLights;
    case DeskInputField::PlayVolume:      return CD::kPaintVolume;
    case DeskInputField::CompMaxLevel:    return CD::kPaintMaxLevelLights;
    case DeskInputField::RemapMin:
    case DeskInputField::RemapMax:        return CD::kPaintRemap;
    // Read by no painter: the filter speeds, the play index, and the cooldown charge.
    case DeskInputField::FrFilterSpeed:
    case DeskInputField::PoFilterSpeed:
    case DeskInputField::PlaySelectIndex:
    case DeskInputField::VisualizerMode:   // drawn by ApplyVisualizerMode
    case DeskInputField::CooldownCharge:  return CD::kPaintNone;
    default:                              return CD::kPaintNone;
    }
}

// Apply ONE field onto the local desk: patch the scalar set + run that field's painters, then the
// live volume the painters do not set.
bool ApplyField(const coop::net::DeskInputPayload& p, CD::Scalars& sc) {
    if (!PatchScalar(p, sc)) return false;
    if (!CD::WriteScalars(sc, PaintersFor(static_cast<DeskInputField>(p.field)))) return false;
    if (static_cast<DeskInputField>(p.field) == DeskInputField::PlayVolume)
        CD::ApplyPlayVolumeEffects(p.intVal);
    // The cap's label is not a painter's: only the press and upgraded_pcUpgrades write it.
    if (static_cast<DeskInputField>(p.field) == DeskInputField::CompMaxLevel) RepaintCapLevelOrDefer();
    // A deferred mode is not applied yet: false keeps OnDeskInput from priming the baseline with it.
    if (static_cast<DeskInputField>(p.field) == DeskInputField::VisualizerMode &&
        !ApplyVisualizerModeOrDefer(p.boolVal != 0))
        return false;
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
    if (cur.remapMin != g_baseline.remapMin && std::isfinite(cur.remapMin))
        SendDelta(s, DeskInputField::RemapMin, cur.remapMin, 0, false);
    if (cur.remapMax != g_baseline.remapMax && std::isfinite(cur.remapMax))
        SendDelta(s, DeskInputField::RemapMax, cur.remapMax, 0, false);
    if (cur.visualizerMode != g_baseline.visualizerMode)
        SendDelta(s, DeskInputField::VisualizerMode, 0, 0, cur.visualizerMode);
    if (cur.coordIsPing != g_baseline.coordIsPing) {
        SendDelta(s, DeskInputField::CoordIsPing, 0, 0, cur.coordIsPing);
        if (s->role() == coop::net::Role::Host) {
            // The host's own press, or a client's verdict the host's machine runs (desk_ping_sync).
            if (cur.coordIsPing) {
                if (g_nextPrimedSlot != 0xFF) {
                    g_primedRunSlot = g_nextPrimedSlot;
                    g_nextPrimedSlot = 0xFF;
                } else {
                    g_pingSetterSlot = 0;
                }
            } else if (g_primedRunSlot != 0xFF) {
                g_primedRunSlot = 0xFF;  // the verdict's run ended
            } else if (g_pingSetterSlot == 0) {
                g_pingSetterSlot = 0xFF;  // its FSM ended
            }
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
    if (g_visPending) {
        coop::desk_snd_fx::ScopedWireApply guard;  // the mode draw is wire-caused
        if (CD::ApplyVisualizerMode(g_visPendingValue)) {
            g_visPending = false;
            if (g_primed) g_baseline.visualizerMode = g_visPendingValue;
            UE_LOGI("desk_input: deferred signal-view mode %d applied (screen widget live)",
                    g_visPendingValue ? 1 : 0);
        }
    }
    if (g_remapPending && CD::RepaintRemap()) {
        g_remapPending = false;
        UE_LOGI("desk_input: deferred remap repaint done (screen widget live)");
    }
    if (g_capLabelPending && CD::RepaintCapLevel()) {
        g_capLabelPending = false;
        UE_LOGI("desk_input: deferred cap label repaint done (screen widget live)");
    }
    if (!s->connected()) {
        // Keep the baseline current while unwired so a (re)connect never
        // bursts stale diffs.
        CD::Scalars cur;
        if (CD::ReadScalars(cur)) { g_baseline = cur; g_primed = true; }
        return;
    }
    PollOnce(s);
}

void OnDeskInput(const coop::net::DeskInputPayload& wire, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (wire.field >= static_cast<uint8_t>(DeskInputField::Count)) return;
    if (!std::isfinite(wire.floatVal)) return;
    // The remap bounds are clamped to the range updRemap clamps to (the way a wire codec clamps a field
    // to its range): the echo-prime below then stores what the desk will hold, and a peer's out-of-range
    // value cannot read as a local edge on the next poll and bounce back through the host.
    coop::net::DeskInputPayload p = wire;
    if (p.field == static_cast<uint8_t>(DeskInputField::RemapMin) ||
        p.field == static_cast<uint8_t>(DeskInputField::RemapMax))
        p.floatVal = CD::ClampRemap(p.floatVal);

    // CoordIsPing is NEVER applied to the machine. The native ping FSM is a latent
    // tick machine gated on that flag, so a raw write here woke a phantom parallel sim
    // on every observer -- divergent verdicts, double coordLog authorship. The delta
    // is bookkeeping only: the ping attribution for the leaver clear, and the
    // desk-claim deny.
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
        // Wire-caused engine writes hold the audio-seam echo guard -- any sound a setter
        // side effect plays must not re-forward.
        coop::desk_snd_fx::ScopedWireApply guard;
        if (!ApplyField(p, sc)) return;
    }
    // ECHO-PRIME: the applied field (and only it) advances the baseline in the
    // same GT task -- the next poll never reads this apply as a local edge.
    if (g_primed) PatchScalar(p, g_baseline);
    else { g_baseline = sc; g_primed = true; }
}

bool ApplyVisualizerModeOrDefer(bool spectrogram) {
    if (CD::ApplyVisualizerMode(spectrogram)) {
        g_visPending = false;  // a newer apply supersedes a deferred one
        return true;
    }
    g_visPending = true;
    g_visPendingValue = spectrogram;
    if (!g_visDeferWarned) {
        g_visDeferWarned = true;
        UE_LOGW("desk_input: signal-view mode %d deferred -- desk screen widget not live yet "
                "(applied by the poll tick when it is; log-once)", spectrogram ? 1 : 0);
    }
    return false;
}

bool RepaintRemapOrDefer() {
    if (CD::RepaintRemap()) {
        g_remapPending = false;
        return true;
    }
    g_remapPending = true;
    return false;
}

bool RepaintCapLevelOrDefer() {
    if (CD::RepaintCapLevel()) {
        g_capLabelPending = false;
        return true;
    }
    g_capLabelPending = true;
    return false;
}

void OnDeskScan(const coop::net::DeskScanEventPayload& p, uint8_t senderSlot) {
    (void)p; (void)senderSlot;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (!CD::EnsureResolved() || !CD::Instance()) return;
    coop::desk_snd_fx::ScopedWireApply guard;  // the spawnDirs replay is wire-caused
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

uint8_t PingActiveSlot() { return g_pingSetterSlot != 0xFF ? g_pingSetterSlot : g_primedRunSlot; }

void AttributePrimedRun(uint8_t slot) { g_nextPrimedSlot = slot; }

void SeedPingAttributionFromMachine() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (g_pingSetterSlot != 0xFF || g_primedRunSlot != 0xFF || g_nextPrimedSlot != 0xFF)
        return;  // a live attribution wins
    CD::Scalars sc;
    if (!CD::EnsureResolved() || !CD::Instance() || !CD::ReadScalars(sc)) return;
    if (!sc.coordIsPing) return;
    // Only the host's own FSM can be running here: no peer was connected to author a
    // delta, and receivers never write the machine field.
    g_pingSetterSlot = 0;
    UE_LOGI("desk_input: ping attribution seeded from machine ground truth "
            "(solo-host ping caught at the connect edge)");
}

void OnPeerLeft(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    const uint8_t leaver = static_cast<uint8_t>(slot);
    // A verdict the host's machine runs for the leaver finishes as the host machine's own run: the desk stays held,
    // by the host, until the machine's falling edge.
    if (g_nextPrimedSlot == leaver) g_nextPrimedSlot = 0;
    if (g_primedRunSlot == leaver) g_primedRunSlot = 0;
    if (g_pingSetterSlot != leaver) return;
    // No machine-field clear: no receiver wire-writes the ping flag, so no peer's
    // machine can hold a leaver's dangling TRUE. Only the attribution -- the
    // desk-claim deny's input -- needs clearing; the leaver's own machine died with
    // its session.
    g_pingSetterSlot = 0xFF;
    UE_LOGI("desk_input: ping attribution cleared -- setter slot %d left mid-ping", slot);
}

void OnDisconnect() {
    // No session-end machine clear either: nothing wire-writes the flag, and a local
    // organic ping ends itself natively.
    g_baseline = {};
    g_primed = false;
    g_pingSetterSlot = 0xFF;
    g_nextPrimedSlot = 0xFF;
    g_primedRunSlot = 0xFF;
    g_scanWidgetWarned = false;
    g_visPending = false;
    g_visPendingValue = false;
    g_visDeferWarned = false;
    g_remapPending = false;
    g_capLabelPending = false;
    g_nextPoll = {};
}

}  // namespace coop::desk_input_sync
