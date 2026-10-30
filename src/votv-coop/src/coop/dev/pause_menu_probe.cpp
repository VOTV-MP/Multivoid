// coop/dev/pause_menu_probe.cpp -- see coop/dev/pause_menu_probe.h.

#include "coop/dev/pause_menu_probe.h"

#include "pause_menu_probe_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/drill_chat.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
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
constexpr double kM2WindowS = 10.0;
constexpr auto kStepBound = std::chrono::seconds(30);
constexpr auto kHostBound = std::chrono::seconds(180);
constexpr auto kReadyBound = std::chrono::seconds(180);

int ArmNow() {
    static const int arm =
        static_cast<int>(coop::config::ResolveInt(::coop::config_registry::rows::pause_menu_probe));
    return arm;
}

enum class Ph : uint8_t { Wait, HostWatch, Closed, WaitOpen, M2Window, WaitClosed, OpenFrames, Rows, RowFrames,
                          Bind, ClosedEnd, Done };
enum class Cond : uint8_t { None, Closed, Open, Rows };

Ph    g_ph = Ph::Wait;
Cond  g_cond = Cond::None;
bool  g_peerRun = false;     // a client runs the arms with M2: its host is its peer
bool  g_hostWatch = false;   // arm 4, the host only: it watches the client's M2 window
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
    bind::Release();  // the delegates it bound are written back before the widgets they sit on can be freed
    rows::Teardown();
    // The game's own state, not our flag: a Fail between enterPause and the open edge leaves the menu opening.
    if (coop::multiplayer_menu::IsPauseMenuOpen() && g_menu && g_resumeFn && R::IsLiveByIndex(g_menu, g_menuIdx))
        Call0(g_resumeFn);
    g_menuOpen = false;
    Release();
    g_ph = Ph::Done;
}

void Fail(const char* why) {
    UE_LOGW("pmprobe FAIL: %s", why);
    Finish();
}

void Done() {
    Finish();
    UE_LOGI("pmprobe DONE %d", ArmNow());
}

bool Overdue(std::chrono::seconds bound) { return Clock::now() - g_stageAt > bound; }
void Stage(Ph next) { g_ph = next; g_stageAt = Clock::now(); }

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
    // The game's own reference to the menu: a field read on each pump tick until the gamemode has made it.
    g_menu = coop::multiplayer_menu::PauseMenu();
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
    // The role is the row's, never inferred from who is connected: a host is ready before its client joins.
    g_hostWatch = ArmNow() == 4;
    if (g_hostWatch && s.role() != coop::net::Role::Host) { Fail("arm 4 is the host's watch"); return false; }
    g_peerRun = s.role() != coop::net::Role::Host;
    const char* why = "";
    if (ArmNow() >= 3 && !g_hostWatch && !bind::Start(&why)) { Fail(why); return false; }
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
        wire::Begin();
        g_stageAt = Clock::now();
        UE_LOGI("pmprobe m2 host: the client's pause is open");
        return;
    }
    wire::Pump("host", 60);
    if (!DC::SeenHas(kLineClosed)) return;
    wire::End("host");
    Done();
}

// The player's Escape event, which opens the menu when it is closed and closes it when it is open: the route
// the game itself takes, so the middle cycle shows how enterPause and resume are reached from it.
bool SendEscape() {
    void* player = coop::players::Registry::Get().Local();
    void* fn = player ? R::FindFunction(R::ClassOf(player), P::name::MainPlayerEscapeFn) : nullptr;
    if (!fn) return false;
    const int32_t size = R::FunctionFrameSize(fn);
    std::vector<uint8_t> frame(size > 0 ? static_cast<size_t>(size) : 0, 0u);
    R::CallFunction(player, fn, frame.empty() ? nullptr : frame.data());
    return true;
}

// Cycle 2 of arm 3 opens through the Escape event; every other open, and every close, calls the menu's own
// function. The player's Escape never closes the menu: with no interface active it always reaches enterPause
// (mainPlayer.cpp:2263-2315); while the menu is open the key reaches the menu widget instead (ui_menu OnKeyDown).
bool ViaEscape() { return g_cycle == 2 && ArmNow() >= 3; }

void OpenMenu() {
    ++g_cycle;
    if (!ViaEscape()) Call0(g_enterFn);
    else if (!SendEscape()) return Fail("the Escape event was not found");
    UE_LOGI("pmprobe enterPause #%d via %s (game_paused=%d)", g_cycle, ViaEscape() ? "Escape" : "direct call",
            ue_wrap::engine::IsGamePaused() ? 1 : 0);
    Stage(Ph::WaitOpen);
}

void CloseMenu() {
    Call0(g_resumeFn);
    g_menuOpen = false;
    Stage(Ph::WaitClosed);
}

void AfterClosed() {
    bind::ReportDispatch(g_cycle, ViaEscape() ? "Escape open, direct resume" : "direct call");
    if (g_cycle < 3) return OpenMenu();
    // The first closed window ran while a joining client still loaded the host; this one is the settled baseline.
    ArmSampler(Cond::Closed, "daynightCycle_C::ReceiveTick(post)");
    Stage(Ph::ClosedEnd);
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
        wire::Begin();
        return Stage(Ph::M2Window);
    }
    UE_LOGI("pmprobe m2 SKIPPED: the host runs no m2 (a client on arm 3 with its host on arm 4 does)");
    CloseMenu();
}

void M2Window() {
    wire::Pump("client", 10);
    if (wire::Elapsed() < kM2WindowS) return;
    wire::End("client");
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
    if (ArmNow() >= 3) return Stage(Ph::Bind);
    rows::Teardown();
    CloseMenu();
}

// The bound delegates, on the rows still in the open menu; the candidates' counting window may have time left.
void Bind() {
    const char* why = "";
    const bind::Step st = bind::Advance(&why);
    if (st == bind::Step::Failed) return Fail(why);
    if (st == bind::Step::Waiting) return;
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
    case Ph::Bind:        return Bind();
    case Ph::ClosedEnd:
        if (Overdue(kStepBound)) return Fail("the day cycle actor's tick did not run 300 times at the end");
        if (g_s.n < kFrames) return;
        LogFrames("closed_end");
        g_cond = Cond::None;
        return Done();
    default:              return;
    }
}

}  // namespace

void Tick(coop::net::Session* s) {
    if (ArmNow() <= 0 || g_ph == Ph::Done || !s || !s->running()) return;
    if (g_ph == Ph::Wait) {
        if (g_stageAt.time_since_epoch().count() == 0) g_stageAt = Clock::now();
        if (!Ready(*s) || !Prepare(*s)) {
            if (g_ph == Ph::Wait && Overdue(kReadyBound)) Fail("the pause menu or the session was not ready");
            return;
        }
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
