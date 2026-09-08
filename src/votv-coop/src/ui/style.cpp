// ui/style.cpp -- see ui/style.h for WHY this is its own owner.

#include "ui/style.h"

#include <windows.h>

#include "ui/fonts.h"
#include "ui/overlay_backend.h"
#include "ui/scale.h"

#include "ue_wrap/core/log.h"

#include "imgui.h"

namespace ui::style {

void Rebuild() {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark();
    const float f = ui::scale::Ui();
    st.ScaleAllSizes(f);

    // THE CURSOR BUG. ImGui's ScaleAllSizes() applies ImTrunc to MouseCursorScale the way it does
    // to every other style field. Every other field is a PIXEL SIZE, where truncating toward zero
    // is right; MouseCursorScale is a unitless MULTIPLIER, so a UI factor below 1 -- 0.833 here --
    // truncates to 0, and RenderMouseCursor then draws a zero-area quad. With the OS cursor hidden
    // by our WM_SETCURSOR handler that is no cursor at all, and it reads as intermittent because
    // the factor tracks client size: at 1.0 and above the truncation is harmless, below 1.0 the
    // cursor vanishes. Upstream has applied the same ImTrunc to both since ImGui 1.91.5.
    //
    // So the cursor never scales BELOW 1: a cursor smaller than its authored 12x19 is not worth
    // having, and 0 is not a size. ImTrunc-then-floor-at-1, written without imgui_internal.h
    // (ImTrunc(x) == (float)(int)x for the positive factors this ever sees).
    // VOTVCOOP_CURSOR_SCALE_DRILL=1 skips the correction so the guard below can be SHOWN RED -- a
    // guard never observed failing passes by construction.
    static int sDrill = -1;
    if (sDrill == -1) {
        char v[8]{};
        sDrill = (::GetEnvironmentVariableA("VOTVCOOP_CURSOR_SCALE_DRILL", v, sizeof(v)) > 0 &&
                  v[0] == '1') ? 1 : 0;
    }
    if (!sDrill) st.MouseCursorScale = (f < 1.0f) ? 1.0f : static_cast<float>(static_cast<int>(f));

    // The guard. A zero here means no cursor is drawn at all and nothing else complains --
    // the exact silence that let this ship. Loud, every rebuild, not once.
    if (!(st.MouseCursorScale >= 1.0f))
        UE_LOGE("ui::style: MouseCursorScale=%.3f at uiScale=%.3f -- the software cursor "
                "will draw a ZERO-AREA quad and be invisible", st.MouseCursorScale, f);
    else
        UE_LOGI("ui::style: style rebuilt (uiScale=%.3f, MouseCursorScale=%.3f)",
                f, st.MouseCursorScale);
}

void MaybeRescale(HWND hwnd) {
    RECT rc{};
    if (hwnd && ::GetClientRect(hwnd, &rc))
        ui::scale::NoteViewport(static_cast<float>(rc.right - rc.left),
                                static_cast<float>(rc.bottom - rc.top));
    if (!ui::scale::ConsumeRebuild()) return;
    ui::fonts::Load();  // clears + re-bakes the atlas at the new px/family
    overlay_backend::InvalidateDeviceObjects();
    Rebuild();
    UE_LOGI("ui::style: UI re-scaled (factor %.2f, client %ldx%ld)", ui::scale::Ui(),
            rc.right - rc.left, rc.bottom - rc.top);
}

}  // namespace ui::style
