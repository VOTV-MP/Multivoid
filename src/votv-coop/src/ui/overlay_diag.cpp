// ui/overlay_diag.cpp -- see ui/overlay_diag.h.
//
// The overlay's two diagnostic probes, kept out of imgui_overlay.cpp so neither file carries
// the other's weight. Each takes its gate state as parameters rather than reading the
// overlay's file-local predicates.

#include "ui/overlay_diag.h"

#include "coop/dev/input_focus_probe.h"
#include "ui/scale.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <atomic>

#include "imgui.h"
#include "imgui_internal.h"  // ImFontAtlasGetMouseCursorTexData (diagnostic only)

namespace ui::overlay_diag {
namespace {

std::atomic<unsigned> g_cntSetCursorPos{0};
std::atomic<int> g_lastSetX{-1}, g_lastSetY{-1};
std::atomic<unsigned> g_cntMouseMove{0}, g_cntInput{0}, g_cntSetCursor{0}, g_cntNcMouseMove{0};

// Read the environment ONCE. Both probes are off in every normal run, and an
// instrument that costs a syscall per frame to stay silent is its own regression.
bool ArmedOnce(const char* var) {
    char v[8]{};
    return ::GetEnvironmentVariableA(var, v, sizeof(v)) > 0 && v[0] == '1';
}

}  // namespace

void NoteWndProcMsg(UINT msg) {
    switch (msg) {
        case WM_MOUSEMOVE:   g_cntMouseMove.fetch_add(1, std::memory_order_relaxed); break;
        case WM_INPUT:       g_cntInput.fetch_add(1, std::memory_order_relaxed); break;
        case WM_SETCURSOR:   g_cntSetCursor.fetch_add(1, std::memory_order_relaxed); break;
        case WM_NCMOUSEMOVE: g_cntNcMouseMove.fetch_add(1, std::memory_order_relaxed); break;
        default: break;
    }
}

void NoteWndProcThread() {
    static std::atomic<bool> sLogged{false};
    if (sLogged.load(std::memory_order_relaxed)) return;
    // `IsGameThread()` is false BOTH on a genuine worker AND before the ProcessEvent
    // detour has ever run (the id is 0 then), and the first window messages arrive during
    // bring-up. Latching on that would record "not the game thread" for a reason that has
    // nothing to do with the question. So wait for a DEFINITIVE answer -- one of the two
    // predicates true -- and only then latch. Until then this costs one relaxed load.
    const bool yes = ue_wrap::game_thread::IsGameThread();
    const bool no  = ue_wrap::game_thread::IsDefinitelyOffGameThread();
    if (!yes && !no) return;  // id not known yet; ask again on the next message
    sLogged.store(true, std::memory_order_relaxed);
    UE_LOGI("gate3: WndProcDetour thread=%lu isGameThread=%d definitelyOff=%d",
            ::GetCurrentThreadId(), (int)yes, (int)no);
}

void NoteSetCursorPos(int x, int y) {
    g_cntSetCursorPos.fetch_add(1, std::memory_order_relaxed);
    g_lastSetX.store(x, std::memory_order_relaxed);
    g_lastSetY.store(y, std::memory_order_relaxed);
}

// M4: TranslateMessage queues WM_CHAR from a WM_KEYDOWN BEFORE DispatchMessage ever
// reaches the overlay's detour, so swallowing the keydown cannot stop the char existing.
// This records, for every key message, what arrived and what we did with it -- so "which
// message does the reporter actually lose" stops being inferred from the keydown gate.
void NoteKeyMsg(UINT msg, WPARAM wParam, const char* verdict, KeyGates gates) {
    if (!coop::dev::input_focus_probe::IsArmed()) return;
    // The overlay's swallow switch shares ONE body across the mouse and key cases, so
    // this is reached for WM_MOUSEMOVE as well; run 1 logged a single "?" line from
    // exactly that. Only key messages are the subject here.
    if (msg != WM_KEYDOWN && msg != WM_KEYUP && msg != WM_CHAR &&
        msg != WM_SYSKEYDOWN && msg != WM_SYSKEYUP) return;
    const char* kind = msg == WM_KEYDOWN ? "KEYDOWN" : msg == WM_KEYUP ? "KEYUP"
                     : msg == WM_CHAR ? "CHAR" : msg == WM_SYSKEYDOWN ? "SYSKEYDOWN"
                     : "SYSKEYUP";
    UE_LOGI("key_probe: %s wParam=0x%02X ('%c') -> %s  [capture=%d chat=%d pause=%d]",
            kind, (unsigned)wParam,
            (wParam >= 32 && wParam < 127) ? (char)wParam : '.', verdict,
            (int)gates.capture, (int)gates.chat, (int)gates.pause);
}

// DIAGNOSTIC (VOTVCOOP_CURSOR_PROBE=1): why no cursor draws over a capturing surface.
// Called at the END of the frame, immediately before ImGui::Render(), so it reads exactly
// the state Render() will. One line per couple of seconds while a capturing surface is up,
// naming every term of the draw guard and of RenderMouseCursor's own early-outs, so one run
// separates the candidates: MousePos never valid, MouseDrawCursor clobbered, the cursor
// drawn outside the viewport rect, or the atlas returning no cursor texture data. Every
// ImGui term reads healthy; io.MousePos is the one that fails.
void CursorFrame(HWND hwnd, bool captureActive, SetCursorPosFn origSetCursorPos) {
    static int sArmed = -1;
    if (sArmed == -1) sArmed = ArmedOnce("VOTVCOOP_CURSOR_PROBE") ? 1 : 0;
    if (!sArmed || !captureActive) return;
    static double sNext = 0.0;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    if (g.Time < sNext) return;
    sNext = g.Time + 2.0;

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 off{}, size{}, uv[4]{};
    const bool texOk = ImFontAtlasGetMouseCursorTexData(g.DrawListSharedData.FontAtlas,
                                                        g.MouseCursor, &off, &size, &uv[0], &uv[2]);
    const ImRect vp = g.Viewports[0]->GetMainRect();
    const float sc = g.Style.MouseCursorScale;
    const ImVec2 pos(io.MousePos.x - off.x, io.MousePos.y - off.y);
    const ImRect box(pos, ImVec2(pos.x + (size.x + 2) * sc, pos.y + (size.y + 2) * sc));
    const bool overlaps = vp.Overlaps(box);
    POINT osp{}; const BOOL gcpOk = ::GetCursorPos(&osp);
    POINT phys{}; ::GetPhysicalCursorPos(&phys);
    POINT cli = osp; ::ScreenToClient(hwnd, &cli);
    RECT clip{}; ::GetClipCursor(&clip);
    // RIDEV_NOLEGACY on the mouse page suppresses WM_MOUSEMOVE app-wide -- the one
    // mechanism that explains "cursor over our foreground window, zero mouse messages".
    UINT rawCount = 0;
    char ridDump[192]{}; int ridLen = 0;
    if (::GetRegisteredRawInputDevices(nullptr, &rawCount, sizeof(RAWINPUTDEVICE)) == (UINT)-1 &&
        rawCount > 0 && rawCount < 32) {
        RAWINPUTDEVICE rid[32]{};
        if (::GetRegisteredRawInputDevices(rid, &rawCount, sizeof(RAWINPUTDEVICE)) != (UINT)-1)
            for (UINT i = 0; i < rawCount && ridLen < 150; ++i) {
                ridLen += _snprintf_s(ridDump + ridLen, sizeof(ridDump) - ridLen, _TRUNCATE,
                                      "[%u/%u f=0x%X hw=%p]", rid[i].usUsagePage, rid[i].usUsage,
                                      (unsigned)rid[i].dwFlags, (void*)rid[i].hwndTarget);
            }
    }
    // Is the clip rect the game keeps re-applying actually OUR client rect? A cursor
    // pinned at the clip corner is the signature of a clamp against a mismatched rect.
    RECT wr{}, cr{}; ::GetWindowRect(hwnd, &wr); ::GetClientRect(hwnd, &cr);
    POINT cOrg{cr.left, cr.top}; ::ClientToScreen(hwnd, &cOrg);
    // NEGATIVE CONTROL (M2): the earlier revision wrote the client centre through the
    // ORIGINAL SetCursorPos here and then read it back. That write is exactly the input
    // whose ABSENCE is the hypothesis -- with it in place the probe could not tell a
    // frozen pointer from a moving one, so it is gone. VOTVCOOP_CURSOR_PROBE_WRITE=1
    // puts it back for the positive control (a run that shows the pointer IS movable).
    static int sWriteTest = -1;
    if (sWriteTest == -1) sWriteTest = ArmedOnce("VOTVCOOP_CURSOR_PROBE_WRITE") ? 1 : 0;
    POINT want{cOrg.x + (cr.right - cr.left) / 2, cOrg.y + (cr.bottom - cr.top) / 2};
    BOOL wrote = FALSE;
    DWORD writeErr = 0;
    if (sWriteTest && origSetCursorPos) {
        ::SetLastError(0);
        wrote = origSetCursorPos(want.x, want.y);
        writeErr = ::GetLastError();
    }
    POINT after{}; ::GetCursorPos(&after);
    // M3: the cross-process disagreement (PowerShell GetCursorInfo=(426,344) vs this
    // process GetCursorPos=(0,24)) had DPI virtualisation as its only cheap candidate,
    // and that is what it was -- the game process is DPI_AWARENESS_UNAWARE and PowerShell
    // is not, so the two readings were never comparable. Reported so the comparison stays
    // decidable rather than being re-filed as an anomaly.
    const DPI_AWARENESS_CONTEXT dpiCtx = ::GetThreadDpiAwarenessContext();
    const DPI_AWARENESS dpiAware = ::GetAwarenessFromDpiAwarenessContext(dpiCtx);
    // Print every INPUT to the overlap predicate, not its verdict. A bare `overlaps=0` cannot
    // be rechecked: recomputed against the pinned cursor texture data, whose Arrow offset is
    // (0,0), the same numbers say it should have been 1, and with only the verdict recorded
    // there is no way to tell which input was wrong. FramebufferScale and MouseCursorScale
    // matter most here, since the UI runs at a fractional scale and neither is obvious from
    // the box coordinates alone.
    // GetCursorInfo, not a screenshot: `flags` separates HIDDEN from RE-CENTRED, which a photo
    // cannot, and no capture path here composites an OS cursor at all -- an instrument blind to
    // the phenomenon always reports it absent.
    CURSORINFO ci{}; ci.cbSize = sizeof(ci);
    const BOOL ciOk = ::GetCursorInfo(&ci);
    UE_LOGI("cursor_terms: box=(%.1f,%.1f)-(%.1f,%.1f) vp=(%.1f,%.1f)-(%.1f,%.1f) "
            "curScale=%.3f fbScale=(%.3f,%.3f) dispSize=(%.1f,%.1f) uiScale=%.3f "
            "cursorInfo ok=%d flags=0x%X showing=%d at=(%ld,%ld) hCursor=%p "
            "overlapsTerms=[%d %d %d %d]",
            box.Min.x, box.Min.y, box.Max.x, box.Max.y,
            vp.Min.x, vp.Min.y, vp.Max.x, vp.Max.y,
            sc, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y,
            io.DisplaySize.x, io.DisplaySize.y, ui::scale::Ui(),
            (int)ciOk, (unsigned)ci.flags, (int)((ci.flags & CURSOR_SHOWING) != 0),
            ci.ptScreenPos.x, ci.ptScreenPos.y, (void*)ci.hCursor,
            (int)(vp.Min.x < box.Max.x), (int)(vp.Min.y < box.Max.y),
            (int)(vp.Max.x > box.Min.x), (int)(vp.Max.y > box.Min.y));
    UE_LOGI("cursor_probe: draw=%d cursor=%d posValid=%d imguiPos=(%.1f,%.1f) osScreen=(%ld,%ld) "
            "osClient=(%ld,%ld) clip=(%ld,%ld)-(%ld,%ld) capture=%p vp=(%.0f,%.0f)-(%.0f,%.0f) "
            "overlaps=%d texOk=%d curSize=(%.0f,%.0f) curOff=(%.0f,%.0f) atlasFlags=0x%X "
            "gcpOk=%d phys=(%ld,%ld) win=(%ld,%ld)-(%ld,%ld) cliOrg=(%ld,%ld) cliSz=%ldx%ld "
            "writeTest armed=%d wrote=%d err=%lu want=(%ld,%ld) after=(%ld,%ld) dpiAware=%d rid=%u%s "
            "scpCalls=%u scpLast=(%d,%d) msgs mm=%u ncmm=%u raw=%u setcur=%u fg=%p hwnd=%p",
            (int)io.MouseDrawCursor, (int)g.MouseCursor, (int)ImGui::IsMousePosValid(),
            io.MousePos.x, io.MousePos.y, osp.x, osp.y, cli.x, cli.y,
            clip.left, clip.top, clip.right, clip.bottom, (void*)::GetCapture(),
            vp.Min.x, vp.Min.y, vp.Max.x, vp.Max.y,
            (int)overlaps, (int)texOk, size.x, size.y, off.x, off.y,
            (unsigned)io.Fonts->Flags,
            (int)gcpOk, phys.x, phys.y, wr.left, wr.top, wr.right, wr.bottom,
            cOrg.x, cOrg.y, cr.right - cr.left, cr.bottom - cr.top,
            sWriteTest, (int)wrote, writeErr, want.x, want.y, after.x, after.y,
            (int)dpiAware, rawCount, ridDump,
            g_cntSetCursorPos.load(std::memory_order_relaxed),
            g_lastSetX.load(std::memory_order_relaxed), g_lastSetY.load(std::memory_order_relaxed),
            g_cntMouseMove.load(std::memory_order_relaxed),
            g_cntNcMouseMove.load(std::memory_order_relaxed),
            g_cntInput.load(std::memory_order_relaxed),
            g_cntSetCursor.load(std::memory_order_relaxed),
            (void*)::GetForegroundWindow(), (void*)hwnd);
}

}  // namespace ui::overlay_diag
