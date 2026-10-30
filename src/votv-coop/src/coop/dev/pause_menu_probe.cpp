// coop/dev/pause_menu_probe.cpp -- see coop/dev/pause_menu_probe.h.

#include "coop/dev/pause_menu_probe.h"

#include "pause_menu_probe_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/drill_chat.h"
#include "coop/input/input_owner.h"
#include "coop/net/net_stats.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "ui/input_focus.h"
#include "ui/multiplayer_menu.h"    // IsPauseMenuOpen
#include "ui/native_screen.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::pause_menu_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace P  = ue_wrap::profile;
namespace GT = ue_wrap::game_thread;
namespace U  = ue_wrap::umg;
namespace NS = ui::native_screen;
namespace DC = coop::dev::drill_chat;
using namespace detail;
using Clock = std::chrono::steady_clock;

// The per-frame sources. A closed menu does not tick, and the world's own tick stops while the game is
// paused, so each condition is read from the one that runs in it: the day cycle actor's tick while the menu is
// closed (the world runs), the pause menu widget's own tick while it is open (widgets tick paused).
UE_FACT_FN(kRowMenuTick, "pause_menu_probe", L"ui_menu_C", L"Tick", Declared);
UE_FACT_FN(kRowCycleTick, "pause_menu_probe", L"daynightCycle_C", L"ReceiveTick", Declared);

constexpr const char* kLineOpen   = "[PM] pause open";
constexpr const char* kLineClosed = "[PM] pause closed";
// The measurement window of the client's pause, and the fail-safe bounds on the waits that are otherwise
// on a readiness line (they end a run that cannot finish, they are not a verdict).
constexpr auto kM2Window = std::chrono::seconds(10);
constexpr auto kStepBound = std::chrono::seconds(30);
constexpr auto kHostBound = std::chrono::seconds(180);

int ArmNow() {
    static const int arm =
        static_cast<int>(coop::config::ResolveInt(::coop::config_registry::rows::pause_menu_probe));
    return arm;
}

enum class Ph : uint8_t { Wait, HostWatch, Closed, Open, WaitOpen, AtOpen, M2Window, Close, WaitClosed,
                          OpenFrames, Rows, RowFrames, Done };
enum class Cond : uint8_t { None, Closed, Open, Rows };

Ph    g_ph = Ph::Wait;
Cond  g_cond = Cond::None;
bool  g_peerRun = false;     // the client of a settled pair: the arms include M2
bool  g_hostWatch = false;   // the host of a settled pair: it only watches
int   g_cycle = 0;           // enterPause calls so far
void* g_menu = nullptr;
int32_t g_menuIdx = -1;
void* g_screen = nullptr;
void* g_enterFn = nullptr;
void* g_resumeFn = nullptr;
bool  g_menuOpen = false;
Clock::time_point g_stageAt{};
ue_wrap::facts::Resolved g_resMenuTick, g_resCycleTick;

struct Named { Ident menu, screen, switcher, vbox; };
Named g_first;
std::vector<Ident> g_firstIds;

// ---- frame samples -------------------------------------------------------------------------------------

struct Sampler {
    int64_t last = 0;
    int     n = 0;
    double  ms[kFrames]{};
    const char* src = "";
    float   end = 0.f;       // the scroll box's travel, for the rows condition
};
Sampler g_s;

double QpcMs(int64_t ticks) {
    static const double perMs = [] { LARGE_INTEGER f{}; ::QueryPerformanceFrequency(&f); return f.QuadPart / 1000.0; }();
    return static_cast<double>(ticks) / perMs;
}

void SampleFrame() {
    LARGE_INTEGER now{};
    ::QueryPerformanceCounter(&now);
    if (g_s.last != 0 && g_s.n < kFrames) g_s.ms[g_s.n++] = QpcMs(now.QuadPart - g_s.last);
    g_s.last = now.QuadPart;
}

void ArmSampler(Cond c, const char* src) {
    g_s.last = 0;
    g_s.n = 0;
    g_s.src = src;
    g_s.end = 0.f;
    g_cond = c;
}

void OnCycleTick(void* /*self*/, void* /*function*/, void* /*params*/) {
    if (g_cond == Cond::Closed && g_s.n < kFrames) SampleFrame();
}

// The pause menu's tick, per frame while it is open, paused or not. The rows condition scrolls the box over
// its whole travel across the samples, one dispatch a frame.
void OnMenuTick(void* self, void* /*function*/, void* /*params*/) {
    if (self != g_menu || g_s.n >= kFrames) return;
    if (g_cond == Cond::Rows && g_s.end > 0.f)
        U::SetScrollOffset(rows::Scroll(), g_s.end * static_cast<float>(g_s.n) / static_cast<float>(kFrames - 1));
    if (g_cond == Cond::Open || g_cond == Cond::Rows) SampleFrame();
}

void LogFrames(const char* cond) {
    std::vector<double> v(g_s.ms, g_s.ms + g_s.n);
    std::sort(v.begin(), v.end());
    const int n = static_cast<int>(v.size());
    const double p50 = v[static_cast<size_t>(n / 2)];
    const double p99 = v[static_cast<size_t>((std::min)(n - 1, static_cast<int>(std::ceil(0.99 * n)) - 1))];
    UE_LOGI("pmprobe m4 %s src=%s frames=%d p50=%.3f p99=%.3f max=%.3f ms", cond, g_s.src, n, p50, p99, v.back());
}

Named Collect(void* menu) {
    Named n;
    n.menu = IdentOf(menu);
    n.screen = IdentOf(NS::DonorChild(menu, L"screen"));
    n.switcher = IdentOf(NS::DonorField(menu, L"switcher_widgets"));
    n.vbox = IdentOf(NS::DonorChild(menu, L"VerticalBox_114"));
    return n;
}

void LogNamed(const char* tag, const char* what, const Ident& a, const Ident& b) {
    UE_LOGI("pmprobe m1 %s %s first=%p/%d %s=%p/%d same_ptr=%d same_serial=%d", tag, what, a.p, a.serial, tag,
            b.p, b.serial, a.p == b.p ? 1 : 0, a.serial == b.serial ? 1 : 0);
}

void DumpMenu(int cycle) {
    char tag[24];
    std::snprintf(tag, sizeof(tag), "m1 tree c%d", cycle);
    void* tree = NS::DonorField(g_menu, L"WidgetTree");
    void* root = NS::ReadPtr(tree, static_cast<int32_t>(P::off::UWidgetTree_RootWidget));
    std::vector<Ident> ids;
    DumpTree(tag, root, 4, &ids);
    const Named now = Collect(g_menu);
    UE_LOGI("pmprobe m1 c%d identity menu=%p/%d screen=%p/%d switcher_widgets=%p/%d VerticalBox_114=%p/%d", cycle,
            now.menu.p, now.menu.serial, now.screen.p, now.screen.serial, now.switcher.p, now.switcher.serial,
            now.vbox.p, now.vbox.serial);
    if (cycle == 1) {
        g_first = now;
        g_firstIds = std::move(ids);
        return;
    }
    LogNamed("menu", "identity", g_first.menu, now.menu);
    LogNamed("screen", "identity", g_first.screen, now.screen);
    LogNamed("switcher_widgets", "identity", g_first.switcher, now.switcher);
    LogNamed("VerticalBox_114", "identity", g_first.vbox, now.vbox);
    size_t same = 0;
    for (const Ident& b : ids)
        for (const Ident& a : g_firstIds)
            if (a.p == b.p && a.serial == b.serial) { ++same; break; }
    UE_LOGI("pmprobe m1 tree first=%zu third=%zu same_objects=%zu -> the widgets are %s", g_firstIds.size(),
            ids.size(), same, same == ids.size() && same == g_firstIds.size() ? "KEPT across opens" : "REBUILT or changed");
}

// ---- the menu --------------------------------------------------------------------------------------------

bool IsPauseInstance(void* menu) {
    static int32_t off = -1;
    static uint8_t mask = 0;
    if (off < 0 && !R::FindBoolProperty(R::ClassOf(menu), P::name::UiMenuIsPauseProp, off, mask)) return false;
    return (*(static_cast<const uint8_t*>(menu) + off) & mask) != 0;
}

// The live ui_menu_C whose isPause reads true; the GameInstance owns it, so there is no world to filter by.
void* FindPauseMenu(bool& ambiguous) {
    void* found = nullptr;
    for (void* obj : R::FindObjectsByClass(P::name::UiMenuClass)) {
        if (!R::IsLive(obj) || !IsPauseInstance(obj)) continue;
        if (found) { ambiguous = true; return nullptr; }
        found = obj;
    }
    return found;
}

void Release() {
    if (g_resMenuTick) GT::UnregisterObservers(g_resMenuTick.Fn(), &OnMenuTick);
    if (g_resCycleTick) GT::UnregisterObservers(g_resCycleTick.Fn(), &OnCycleTick);
    g_resMenuTick = {};
    g_resCycleTick = {};
    g_cond = Cond::None;
}

void Call0(void* fn) { R::CallFunction(g_menu, fn, nullptr); }

// The probe is over and the menu is as the player left it.
void Finish() {
    rows::Teardown();
    if (g_menuOpen && g_menu && R::IsLiveByIndex(g_menu, g_menuIdx)) Call0(g_resumeFn);
    g_menuOpen = false;
    Release();
    g_ph = Ph::Done;
}

void Fail(const char* why) {
    UE_LOGW("pmprobe FAIL: %s", why);
    Finish();
}

void Done() {
    UE_LOGI("pmprobe DONE %d", ArmNow());
    Finish();
}

bool Overdue(std::chrono::seconds bound) { return Clock::now() - g_stageAt > bound; }
void Stage(Ph next) { g_ph = next; g_stageAt = Clock::now(); }

// ---- the wire counters and the hotkeys (M2) ---------------------------------------------------------------

struct M2 {
    Clock::time_point t0{};
    Clock::time_point lastSec{};
    coop::net::net_stats::Snapshot s0{};
    coop::net::net_stats::Snapshot last{};
    int sec = 0;
};
M2 g_m2;

void M2Begin() {
    coop::net::net_stats::Get(g_m2.s0);
    g_m2.last = g_m2.s0;
    g_m2.t0 = g_m2.lastSec = Clock::now();
    g_m2.sec = 0;
}

// What the hotkeys' handlers consult, read as they read it. Chat's T (ui/imgui_overlay.cpp:392) needs no overlay
// capture, the pause menu NOT open, the input owner's yes for the key and a chat session; F1 (:360) needs the
// overlay menu open or the input owner's yes.
void LogHotkeys(const char* who) {
    const bool menu = coop::multiplayer_menu::IsPauseMenuOpen();
    const bool f1 = coop::input::input_owner::MayTakeKey(VK_F1);
    const bool t = coop::input::input_owner::MayTakeKey('T');
    UE_LOGI("pmprobe m2 %s hotkeys: pause_menu_open=%d may_take_F1=%d may_take_T=%d overlay_capturing_text=%d "
            "our_window_foreground=%d -> F1 %s, T chat %s", who, menu ? 1 : 0, f1 ? 1 : 0, t ? 1 : 0,
            ui::input_focus::IsOverlayCapturingText() ? 1 : 0, ui::input_focus::IsOurWindowForeground() ? 1 : 0,
            f1 ? "toggles the overlay" : "is left to the game", !menu && t ? "opens" : "does NOT open");
}

// One line a second: wire packets since the last line (every message kind; the pose stream is the steady
// bulk, and no pose-only counter exists), and the world's paused state read by UGameplayStatics::IsGamePaused.
void M2Second(const char* who) {
    coop::net::net_stats::Snapshot now{};
    coop::net::net_stats::Get(now);
    ++g_m2.sec;
    UE_LOGI("pmprobe m2 %s t=%ds wire_sent=%llu wire_recv=%llu paused=%d", who, g_m2.sec,
            static_cast<unsigned long long>(now.packetsSent - g_m2.last.packetsSent),
            static_cast<unsigned long long>(now.packetsRecv - g_m2.last.packetsRecv),
            ue_wrap::engine::IsGamePaused() ? 1 : 0);
    if (g_m2.sec == 1) LogHotkeys(who);
    g_m2.last = now;
    g_m2.lastSec += std::chrono::seconds(1);
}

void M2End(const char* who) {
    coop::net::net_stats::Snapshot now{};
    coop::net::net_stats::Get(now);
    const double s = std::chrono::duration<double>(Clock::now() - g_m2.t0).count();
    UE_LOGI("pmprobe m2 %s window=%.1fs wire_sent=%.1f/s wire_recv=%.1f/s paused_at_end=%d", who, s,
            static_cast<double>(now.packetsSent - g_m2.s0.packetsSent) / s,
            static_cast<double>(now.packetsRecv - g_m2.s0.packetsRecv) / s, ue_wrap::engine::IsGamePaused() ? 1 : 0);
}

// ---- readiness ------------------------------------------------------------------------------------------

bool PeerPresent(coop::net::Session& s) {
    if (s.role() != coop::net::Role::Host) return s.connected();
    for (int slot = 1; slot < coop::players::kMaxPeers; ++slot)
        if (s.IsSlotWorldReady(slot)) return true;
    return false;
}

bool Ready(coop::net::Session& s) {
    if (!coop::players::Registry::Get().Local()) return false;
    if (s.role() == coop::net::Role::Host) return true;
    return s.connected() && coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

// Resolves everything the arms need; false, with the reason printed, when something is not there.
bool Prepare(coop::net::Session& s) {
    static Clock::time_point lastFind{};
    if (lastFind.time_since_epoch().count() != 0 && Clock::now() - lastFind < std::chrono::seconds(1)) return false;
    lastFind = Clock::now();
    bool ambiguous = false;
    g_menu = FindPauseMenu(ambiguous);
    if (ambiguous) { Fail("two pause menus"); return false; }
    if (!g_menu) return false;
    g_menuIdx = R::InternalIndexOf(g_menu);
    g_enterFn = R::FindFunction(R::ClassOf(g_menu), L"enterPause");
    g_resumeFn = R::FindFunction(R::ClassOf(g_menu), L"resume");
    g_screen = NS::DonorChild(g_menu, L"screen");
    if (!g_enterFn || !g_resumeFn || !g_screen) { Fail("enterPause, resume or the screen canvas was not found"); return false; }
    g_resMenuTick = ue_wrap::facts::Resolve(kRowMenuTick);
    g_resCycleTick = ue_wrap::facts::Resolve(kRowCycleTick);
    if (!g_resMenuTick || !g_resCycleTick || !GT::RegisterPostObserver(g_resMenuTick, &OnMenuTick) ||
        !GT::RegisterPostObserver(g_resCycleTick, &OnCycleTick)) {
        Fail("a frame-time observer was not taken (ui_menu_C::Tick or daynightCycle_C::ReceiveTick)");
        return false;
    }
    const bool peer = PeerPresent(s);
    g_hostWatch = peer && s.role() == coop::net::Role::Host;
    g_peerRun = peer && s.role() != coop::net::Role::Host;
    UE_LOGI("pmprobe start arm=%d role=%s peer=%d mode=%s menu=%p tick_sources=ui_menu_C::Tick(post),"
            "daynightCycle_C::ReceiveTick(post)", ArmNow(), s.role() == coop::net::Role::Host ? "host" : "client",
            peer ? 1 : 0, g_hostWatch ? "watch" : g_peerRun ? "client-with-peer" : "solo", g_menu);
    return true;
}

// ---- the phases ------------------------------------------------------------------------------------------

void HostWatch() {
    static bool opened = false;
    if (Overdue(opened ? kStepBound : kHostBound)) return Fail("the client's pause lines did not arrive");
    if (!DC::PollDue()) return;
    if (!opened) {
        if (!DC::SeenHas(kLineOpen)) return;
        opened = true;
        M2Begin();
        g_stageAt = Clock::now();
        UE_LOGI("pmprobe m2 host: the client's pause is open");
        return;
    }
    while (Clock::now() - g_m2.lastSec >= std::chrono::seconds(1) && g_m2.sec < 60) M2Second("host");
    if (!DC::SeenHas(kLineClosed)) return;
    M2End("host");
    Done();
}

void OpenMenu() {
    ++g_cycle;
    Call0(g_enterFn);
    UE_LOGI("pmprobe enterPause #%d called (game_paused=%d)", g_cycle, ue_wrap::engine::IsGamePaused() ? 1 : 0);
    Stage(Ph::WaitOpen);
}

void CloseMenu() {
    Call0(g_resumeFn);
    g_menuOpen = false;
    Stage(Ph::WaitClosed);
}

void AfterClosed() {
    if (g_cycle < 3) return OpenMenu();
    Done();
}

void AtOpen() {
    if (g_cycle == 2) return CloseMenu();
    DumpMenu(g_cycle);
    if (g_cycle == 3) {
        ArmSampler(Cond::Open, "ui_menu_C::Tick(post)");
        return Stage(Ph::OpenFrames);
    }
    if (g_peerRun) {
        DC::Say(kLineOpen);
        M2Begin();
        return Stage(Ph::M2Window);
    }
    UE_LOGI("pmprobe m2 SKIPPED: no peer was connected when the probe began");
    CloseMenu();
}

void M2Window() {
    while (Clock::now() - g_m2.lastSec >= std::chrono::seconds(1) && g_m2.sec < 10) M2Second("client");
    if (Clock::now() - g_m2.t0 < kM2Window) return;
    M2End("client");
    DC::Say(kLineClosed);
    CloseMenu();
}

void OpenFrames() {
    if (Overdue(kStepBound)) return Fail("the pause menu's tick did not run 300 times");
    if (g_s.n < kFrames) return;
    LogFrames("open");
    g_cond = Cond::None;
    if (ArmNow() < 2) return CloseMenu();
    const char* why = "";
    if (!rows::Begin(g_menu, g_screen, &why)) return Fail(why);
    Stage(Ph::Rows);
}

void Rows() {
    const char* why = "";
    const rows::Step st = rows::Advance(&why);
    if (st == rows::Step::Failed) return Fail(why);
    if (st == rows::Step::Running) return;
    ArmSampler(Cond::Rows, "ui_menu_C::Tick(post), SetScrollOffset each frame");
    float end = 0.f;
    if (!U::ScrollOffsetOfEnd(rows::Scroll(), end) || end <= 0.f) return Fail("the 100 rows have no travel to scroll");
    g_s.end = end;
    Stage(Ph::RowFrames);
}

void RowFrames() {
    if (Overdue(kStepBound)) return Fail("the pause menu's tick did not run 300 times with the rows");
    if (g_s.n < kFrames) return;
    LogFrames("rows100_scrolled");
    g_cond = Cond::None;
    rows::Teardown();
    CloseMenu();
}

void Run() {
    switch (g_ph) {
    case Ph::Closed:
        if (Overdue(kStepBound)) return Fail("the day cycle actor's tick did not run 300 times");
        if (g_s.n < kFrames) return;
        LogFrames("closed");
        g_cond = Cond::None;
        return OpenMenu();
    case Ph::WaitOpen:
        if (Overdue(kStepBound)) return Fail("the pause menu did not open");
        if (!coop::multiplayer_menu::IsPauseMenuOpen()) return;
        g_menuOpen = true;
        return AtOpen();
    case Ph::M2Window:    return M2Window();
    case Ph::WaitClosed:
        if (Overdue(kStepBound)) return Fail("the pause menu did not close");
        if (coop::multiplayer_menu::IsPauseMenuOpen()) return;
        return AfterClosed();
    case Ph::OpenFrames:  return OpenFrames();
    case Ph::Rows:        return Rows();
    case Ph::RowFrames:   return RowFrames();
    default:              return;
    }
}

}  // namespace

void Tick(coop::net::Session* s) {
    if (ArmNow() <= 0 || g_ph == Ph::Done || !s || !s->running()) return;
    if (g_ph == Ph::Wait) {
        if (!Ready(*s) || !Prepare(*s)) return;
        if (g_hostWatch) { Stage(Ph::HostWatch); return; }
        ArmSampler(Cond::Closed, "daynightCycle_C::ReceiveTick(post)");
        return Stage(Ph::Closed);
    }
    if (g_ph == Ph::HostWatch) return HostWatch();
    if (g_menu && !R::IsLiveByIndex(g_menu, g_menuIdx)) return Fail("the pause menu was destroyed");
    Run();
}

void OnDisconnect() {
    if (g_ph != Ph::Wait && g_ph != Ph::Done) Fail("the session ended");
}

}  // namespace coop::dev::pause_menu_probe
