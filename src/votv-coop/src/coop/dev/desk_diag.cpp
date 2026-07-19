// coop/dev/desk_diag.cpp -- see header. Read-only desk/console divergence census.
//
// Built from a 6-round /qf DESIGN pass (2026-07-15, scratchpad/qf_thread.md).
// Every field is FindPropertyOffset-resolved BY NAME (recook-robust); every read
// is FAIL-LOUD (a missing surface/offset logs UNRESOLVED, never a silent zero) so
// a zero at runtime means "genuinely empty" not "watching the wrong field". Each
// sample carries a QUIESCENT tag (q=Y/N) computed in-probe so a HOST-vs-CLIENT
// diff auto-filters to comparable settled moments instead of trusting the operator
// to pause -- with no shared clock, a moving field diffed off-phase is a false
// positive. Moving fields (needle / viewCoordinate / decoded) are logged as EXHAUST;
// the diff keys on their settled counterparts (DL_frData/poData, filter knobs, dish
// lookAt target).

#include "coop/dev/desk_diag.h"

#include "coop/config/config.h"
#include "coop/interactables/device_occupancy.h"
#include "coop/net/session.h"

#include "ue_wrap/desk/comp_pane.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/coords_panel.h"
#include "ue_wrap/desk/dish.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace coop::dev::desk_diag {
namespace {

namespace CD  = ue_wrap::console_desk;
namespace CMP = ue_wrap::comp_pane;
namespace CP  = ue_wrap::coords_panel;
namespace DSH = ue_wrap::dish;
namespace R   = ue_wrap::reflection;

using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

const wchar_t* const kDeskClaim = L"desk";

std::chrono::milliseconds Interval() {
    static const std::chrono::milliseconds s = [] {
        const std::string v = coop::config::ReadIniValue("desk_diag_ms", "1000");
        long ms = std::strtol(v.c_str(), nullptr, 10);
        if (ms < 100) ms = 100;        // never busier than 10 Hz
        if (ms > 60000) ms = 60000;
        return std::chrono::milliseconds(ms);
    }();
    return s;
}

Clock::time_point g_nextSnap{};
bool g_firstSnap = true;                 // per-surface BASELINE tag on the first periodic emit

// coordLog change tracking (the 82-vs-24 divergence): the last split tail so a
// change dumps only the delta lines (or a REWRITE tail when non-appending).
std::vector<std::wstring> g_prevLog;
bool g_logPrimed = false;

uint32_t Fnv(const std::wstring& s) {
    uint32_t h = 2166136261u;
    for (wchar_t c : s) { h ^= static_cast<uint32_t>(c); h *= 16777619u; }
    return h;
}

std::vector<std::wstring> SplitLines(const std::wstring& text) {
    std::vector<std::wstring> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find_first_of(L"\r\n", pos);
        if (eol == std::wstring::npos) eol = text.size();
        if (eol > pos) out.push_back(text.substr(pos, eol - pos));
        pos = (eol == text.size()) ? eol : text.find_first_not_of(L"\r\n", eol);
        if (pos == std::wstring::npos) break;
    }
    return out;
}

bool IsPrefix(const std::vector<std::wstring>& pre, const std::vector<std::wstring>& full) {
    if (pre.size() > full.size()) return false;
    for (size_t i = 0; i < pre.size(); ++i)
        if (pre[i] != full[i]) return false;
    return true;
}

constexpr size_t kMaxDumpLines = 24;  // bound a REWRITE burst

// SAT-console cross-surface fail-loud line count. The SAT command terminal
// (Uui_console_C.LogText@0x02E0) is a SEPARATE surface from the coords panel;
// the coord lines do NOT go here (bytecode-confirmed 2026-07-15). We count its
// lines so a zero is provably "this terminal is empty" not "we're blind to it".
// Returns line count; sets found=false when no live ui_console (so the caller
// logs the distinction). Returns -1 on UNRESOLVED class/offset (fail loud).
struct FStringView { wchar_t* data; int32_t num; int32_t max; };
int32_t SatConsoleLineCount(bool& found, bool& resolved) {
    found = false;
    resolved = false;
    static void* s_cls = nullptr;
    static int32_t s_off = -1;
    if (!s_cls) s_cls = R::FindClass(L"ui_console_C");
    if (!s_cls) return -1;                 // class not loaded yet
    if (s_off < 0) s_off = R::FindPropertyOffset(s_cls, L"LogText");
    if (s_off < 0) return -1;              // offset UNRESOLVED -> fail loud
    resolved = true;
    for (void* obj : R::FindObjectsByClass(L"ui_console_C")) {
        if (!obj || !R::IsLive(obj)) continue;
        found = true;
        auto* s = reinterpret_cast<FStringView*>(reinterpret_cast<uint8_t*>(obj) + s_off);
        if (!s->data || s->num <= 1) return 0;
        int32_t lines = 1;
        for (int32_t i = 0; i < s->num && s->data[i]; ++i)
            if (s->data[i] == L'\n') ++lines;
        return lines;
    }
    return 0;                              // resolved but no live instance
}

char RoleChar(coop::net::Session* s) {
    return (s->role() == coop::net::Role::Host) ? 'H' : 'C';
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::IsIniKeyTrue("desk_diag");
    return s;
}

void Install(coop::net::Session* session) {
    if (!IsEnabled()) return;
    // subsystems::Install is called EVERY net_pump tick (net_pump.cpp:1014 -- "one-shot
    // install ... idempotent"): the contract is that each subsystem's Install latches its
    // expensive/noisy work and no-ops on repeat. Keep g_session current each call (cheap;
    // picks up a fresh session ptr on re-host), but LATCH the banner so it logs once per
    // process instead of ~1/tick. Prior runs logged the ENABLED line ~37k times/session
    // (2026-07-15) purely because this Install did not honor the sibling idempotency
    // contract. The banner is the only side effect gated -- the census's BASELINE/q-tag
    // state lives entirely in Tick(), untouched here, so the collected samples were valid.
    g_session.store(session, std::memory_order_release);
    static bool s_banner = false;
    if (s_banner) return;
    s_banner = true;
    UE_LOGI("desk_diag: ENABLED (interval %lld ms) -- read-only desk divergence census (4/5 "
            "symptoms; stationary-PC deferred, class unresolved). grep [desk_diag], diff HOST vs "
            "CLIENT at q=Y (quiescent) samples",
            static_cast<long long>(Interval().count()));
}

void NoteJoinAdopt() {
    if (!IsEnabled()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (!CD::EnsureResolved() || !CD::Instance()) {
        UE_LOGW("[desk_diag] JOIN-PRE-ADOPT: desk UNRESOLVED at adopt -- cannot baseline");
        return;
    }
    // Read the LOCAL scalars BEFORE WriteScalars applies the host's seed -- this
    // is the client's pre-adopt state. The next periodic snapshot (post-adopt)
    // holds the host seed; the delta between them = the JOIN-SEED divergence.
    CD::Scalars sc{};
    float frData = 0, poData = 0;
    const bool haveSc = CD::ReadScalars(sc);
    const bool haveFp = CD::ReadFreqPolData(frData, poData);
    UE_LOGI("[desk_diag] %c JOIN-PRE-ADOPT local | needle=%.4f frFilt=%.4f poFilt=%.4f "
            "frData=%.4f poData=%.4f actDL=%d actComp=%d selIdx=%d compMax=%d (host seed lands next snapshot)",
            RoleChar(s),
            haveSc ? sc.dlResDetecPercent : -1.f, haveSc ? sc.dlFrFilterOffset : -1.f,
            haveSc ? sc.dlPoFilterOffset : -1.f,
            haveFp ? frData : -1.f, haveFp ? poData : -1.f,
            haveSc ? (sc.activeDownload ? 1 : 0) : -1, haveSc ? (sc.activeComp ? 1 : 0) : -1,
            haveSc ? sc.playSelectIndex : -1, haveSc ? sc.compMaxLevel : -1);
}

void Tick() {
    if (!IsEnabled()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    const auto now = Clock::now();
    if (now < g_nextSnap) return;
    g_nextSnap = now + Interval();

    if (!CD::EnsureResolved() || !CD::Instance()) return;

    const char role = RoleChar(s);
    const uint8_t holder = coop::device_occupancy::HolderOf(kDeskClaim);
    const int mine = coop::device_occupancy::LocalHolds(kDeskClaim) ? 1 : 0;

    CD::Scalars sc{};
    const bool haveScalars = CD::ReadScalars(sc);
    float frData = 0, poData = 0;
    const bool haveFp = CD::ReadFreqPolData(frData, poData);
    CMP::CompScalars comp{};
    const bool haveComp = CMP::ReadCompScalars(comp);
    float decoded = 0; int32_t polarity = -1;
    const bool haveDl = CD::ReadDownloadProgress(decoded, polarity);
    const int meshValid = CD::DownloadMeshValid() ? 1 : 0;
    CP::DishAim aim{};
    const bool haveAim = CP::ReadDishAim(aim);
    CD::CoordSignal sig{};
    const bool haveSig = CD::ReadCoordSignal(sig);
    const int32_t dishN = DSH::EnsureResolved() ? DSH::Count() : -1;
    const int32_t dishMoving = (dishN >= 0) ? DSH::MovingCount() : -1;
    DSH::DishState dishes[16];
    const int32_t dishStates = (dishN >= 0) ? DSH::ReadAllDishStates(dishes, 16) : 0;

    // coordLog tail.
    const std::wstring logTail = CD::ReadCoordLogTail(1100);
    std::vector<std::wstring> logLines = SplitLines(logTail);
    const uint32_t logHash = Fnv(logTail);
    const bool logChanged = g_logPrimed && (logLines != g_prevLog);

    // ---- QUIESCENT tag: settled -> a single-sample HOST-vs-CLIENT diff is meaningful ----
    // Gate on fields that GENUINELY settle: no dish slewing, decoder idle, no FILTER
    // RAMP, coordLog stable.
    //   - The filter-knob offsets are NOT a delta-gate input: the game drifts
    //     dlFr/PoFilterOffset continuously even at rest (MEASURED, console_state_sync.cpp:194-206),
    //     so a "knobs unchanged" input would pin q=N forever.
    //   - BUT while a filter is ACTIVE the offset is MID-RAMP (offset += speed*dt every
    //     tick -- RE 2026-07-15 desk-download-machine [2122-2128]); a mid-ramp offset is
    //     NOT single-sample comparable across peers (no shared clock -> phase-skew), and
    //     that is THE field the freq/pol redesign hangs on. So gate on the clean discrete
    //     active flags: quiescent requires BOTH filters inactive (ramp settled).
    // The jittery/ramping offsets are logged [jitter]; the settled freq/pol discriminators
    // are DL_frData/DL_poData + the active flags + polDir.
    const bool filterRamping = haveScalars && (sc.dlActiveFrFilter || sc.dlActivePoFilter);
    const bool quiescent = haveScalars &&
        (dishMoving == 0) &&
        !(haveComp && comp.decodeActive) &&
        !filterRamping &&
        !logChanged;

    const char* baseTag = g_firstSnap ? " BASELINE" : "";

    // ---- line 1: settled discriminators (freq/polarity + decode) ----
    if (haveScalars) {
        UE_LOGI("[desk_diag] %c hold=%u mine=%d q=%c%s | needle=%.4f[exhaust] frFilt=%.4f[jitter] poFilt=%.4f[jitter] "
                "frData=%.4f poData=%.4f frSpd=%.4f poSpd=%.4f actF=%d actP=%d polDir=%d | "
                "act(play=%d dl=%d coord=%d comp=%d) cool=%.4f ping=%d vol=%d selIdx=%d compMax=%d",
                role, static_cast<unsigned>(holder), mine, quiescent ? 'Y' : 'N', baseTag,
                sc.dlResDetecPercent, sc.dlFrFilterOffset, sc.dlPoFilterOffset,
                haveFp ? frData : -1.f, haveFp ? poData : -1.f,
                sc.dlFrFilterSpeed, sc.dlPoFilterSpeed,
                sc.dlActiveFrFilter ? 1 : 0, sc.dlActivePoFilter ? 1 : 0, sc.dlPolarityDir,
                sc.activePlay ? 1 : 0, sc.activeDownload ? 1 : 0, sc.activeCoords ? 1 : 0,
                sc.activeComp ? 1 : 0, sc.coordCooldown, sc.coordIsPing ? 1 : 0,
                sc.playVolume, sc.playSelectIndex, sc.compMaxLevel);
    } else {
        UE_LOGW("[desk_diag] %c UNRESOLVED ReadScalars -- desk field offsets not resolved", role);
    }

    // ---- line 2: comp/decode (needle+decoded=exhaust) + dish aggregate + surfaces ----
    bool satFound = false, satResolved = false;
    const int32_t satLines = SatConsoleLineCount(satFound, satResolved);
    UE_LOGI("[desk_diag] %c q=%c | comp(prog=%.4f dl=%.4f act=%d) dl(decoded=%.4f[exhaust] pol=%d mesh=%d) | "
            "dish(n=%d moving=%d) | surface=coordLog2Text lines=%zu h=0x%08X | "
            "surface=sat_console lines=%s",
            role, quiescent ? 'Y' : 'N',
            haveComp ? comp.progress : -1.f, haveComp ? comp.downloading : -1.f,
            (haveComp && comp.decodeActive) ? 1 : 0,
            haveDl ? decoded : -1.f, haveDl ? polarity : -99, meshValid,
            dishN, dishMoving, logLines.size(), logHash,
            !satResolved ? "UNRESOLVED" : (!satFound ? "no-live-instance" : std::to_string(satLines).c_str()));

    // ---- line 3: cursor state (viewCoordinate=exhaust) + committed coords ----
    if (haveAim) {
        UE_LOGI("[desk_diag] %c | aim(sel=%d dir=%u view=%.2f,%.2f[exhaust] "
                "c0=%.2f,%.2f c1=%.2f,%.2f c2=%.2f,%.2f)",
                role, aim.selected, static_cast<unsigned>(aim.direction),
                aim.viewX, aim.viewY, aim.c0X, aim.c0Y, aim.c1X, aim.c1Y, aim.c2X, aim.c2Y);
    }

    // ---- line 4: per-dish commanded TARGET (settled discriminator, mid-slew ok) ----
    for (int32_t i = 0; i < dishStates; ++i) {
        UE_LOGI("[desk_diag] %c | dish[%d] target=%.1f,%.1f,%.1f moving=%d",
                role, dishes[i].index, dishes[i].lookAtX, dishes[i].lookAtY,
                dishes[i].lookAtZ, dishes[i].isMoving ? 1 : 0);
    }

    // ---- caught-signal identity ----
    if (haveSig) {
        UE_LOGI("[desk_diag] %c | coordSig(name=%ls xyz=%.1f,%.1f,%.1f type=%d freq=%.3f pol=%.3f)",
                role, sig.objectName.empty() ? L"<empty>" : sig.objectName.c_str(),
                sig.x, sig.y, sig.z, sig.type, sig.frequency, sig.polarity);
    }

    g_firstSnap = false;

    // ---- coordLog line-delta: which peer generates which lines (per-surface BASELINE) ----
    if (!g_logPrimed) {
        g_prevLog = std::move(logLines);
        g_logPrimed = true;
        UE_LOGI("[desk_diag] %c surface=coordLog2Text BASELINE lines=%zu (joiner starts empty -- a t0 "
                "gap here is EXPECTED, NOT a join-seed defect)", role, g_prevLog.size());
    } else if (logLines != g_prevLog) {
        if (IsPrefix(g_prevLog, logLines)) {
            UE_LOGI("[desk_diag] %c surface=coordLog2Text +%zu appended (now %zu):",
                    role, logLines.size() - g_prevLog.size(), logLines.size());
            for (size_t i = g_prevLog.size(); i < logLines.size(); ++i)
                UE_LOGI("[desk_diag] %c   log[%zu] \"%ls\"", role, i, logLines[i].c_str());
        } else {
            UE_LOGI("[desk_diag] %c surface=coordLog2Text REWRITE (was %zu, now %zu) -- last %zu:",
                    role, g_prevLog.size(), logLines.size(),
                    logLines.size() < kMaxDumpLines ? logLines.size() : kMaxDumpLines);
            const size_t start = logLines.size() > kMaxDumpLines ? logLines.size() - kMaxDumpLines : 0;
            for (size_t i = start; i < logLines.size(); ++i)
                UE_LOGI("[desk_diag] %c   log[%zu] \"%ls\"", role, i, logLines[i].c_str());
        }
        g_prevLog = std::move(logLines);
    }
}

}  // namespace coop::dev::desk_diag
