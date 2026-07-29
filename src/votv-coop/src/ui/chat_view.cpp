// ui/chat_view.cpp -- see ui/chat_view.h.

#include "ui/chat_view.h"

#include "coop/comms/chat_feed.h"
#include "ui/chat_input.h"
#include "ui/fonts.h"
#include "ui/scale.h"

#include "ue_wrap/core/log.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <cstring>

namespace ui::chat_view {
namespace {

using ui::scale::S;

// The feed's bottom edge: 0.5 = vertical middle (user 2026-06-08). ui/chat_input.cpp
// places the input bar just under this same line.
constexpr float kBottomFrac = 0.5f;

// A row is drawn once its composed alpha clears this. It is a MEMBERSHIP predicate,
// not a consequence of the alpha: an invisible row must not occupy height, or a
// history block that has faded to nothing would still push the live lines up the
// screen.
constexpr float kAlphaFloor = 1.f / 255.f;

// ---- the reveal ramp.
//
// It owns its own clock rather than integrating io.DeltaTime, because DeltaTime here
// is whatever elapsed since the last PRESENTED overlay frame -- and the overlay only
// renders when a surface is up. The first frame after a quiet lobby therefore carries
// the entire quiet period as one delta, and a dt-integrated ramp would saturate in a
// single frame. As an absolute function of (now, transitionStart) it also survives
// the pause menu, which suppresses this whole draw for as long as it is up.
//
// 220 ms = chat_feed::kRevealMs = the store's existing arrival-fade constant, already
// tuned for this surface. The store publishes the retained tier for exactly the same
// window after a close, so the fade-out always has rows to draw.
using Clock = std::chrono::steady_clock;

float   g_revealValue = 0.f;   // last computed value (the ramp's start on a reversal)
float   g_revealFrom  = 0.f;
float   g_revealTo    = 0.f;
int64_t g_revealStart = 0;     // ms
bool    g_openLogged  = false;
int64_t g_openedAtMs  = 0;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch()).count();
}

float Ramp(float target) {
    const int64_t now = NowMs();
    if (target != g_revealTo) {
        g_revealFrom  = g_revealValue;
        g_revealTo    = target;
        g_revealStart = now;
    }
    const int64_t elapsed = now - g_revealStart;
    if (elapsed >= static_cast<int64_t>(coop::chat_feed::kRevealMs)) {
        g_revealValue = g_revealTo;  // EXACT endpoints -- no asymptote, no 0.998 forever
    } else {
        const float t = static_cast<float>(elapsed) /
                        static_cast<float>(coop::chat_feed::kRevealMs);
        g_revealValue = g_revealFrom + (g_revealTo - g_revealFrom) * t;
    }
    return std::clamp(g_revealValue, 0.f, 1.f);
}

// ---- the scroll anchor.
//
// (sort key, row within that entry) -- never an index, which shifts under every push
// and every retirement. Paging moves it by whole ROWS, not entries: hud's row height
// is one constant for every wrapped row, so row-paging is exactly invertible and
// PgUp-then-PgDn lands where it started. Paging by a measured pixel viewport over
// variable-height entries is not.
bool     g_pinned = false;
uint64_t g_anchorKey = 0;
int      g_anchorSub = 0;
bool     g_frozenPublished = false;

void SetPinned(bool pinned) {
    g_pinned = pinned;
    if (pinned != g_frozenPublished) {
        // Edge-only, so this is a handful of lines per session, not per frame -- and
        // it is the only observable the scroll has. Without it a drill can press
        // PgUp and see nothing, which is also what a BROKEN pager looks like.
        UE_LOGI("chat_view: %s", pinned ? "PINNED (paged back; retention frozen)"
                                        : "FOLLOW (retention live)");
        coop::chat_feed::SetRetentionFrozen(pinned);
        g_frozenPublished = pinned;
    }
}

constexpr int kRowCap      = 512;  // ~106 entries * up to 4 wrapped rows, with headroom
constexpr int kEntryRowCap = 16;

struct Row {
    int         line = 0;   // index into the snapshot
    int         sub = 0;    // which wrapped row of that line
    const char* b = nullptr;
    const char* e = nullptr;
};

}  // namespace

void Draw() {
    // The snapshot is re-copied only when the store republished. With chat closed it
    // is the same <= 6 rows it has always been; while the reveal is up it also carries
    // the history, which the store rewrites only when it actually changes.
    static coop::chat_feed::Snapshot s;
    static uint32_t gen = 0;
    coop::chat_feed::GetSnapshotIfNewer(s, gen);

    const bool open = ui::chat_input::IsOpen();
    const float reveal = Ramp(open ? 1.f : 0.f);
    if (!open) {
        SetPinned(false);   // reopening always starts at the newest line
        if (g_openLogged) {
            // The CLOSE marker. It bounds the window a drill asserts over -- "while
            // the reveal was up, nothing expired" is only checkable against a window
            // with two ends, and the TTL resumes the instant this fires.
            UE_LOGI("chat_view: reveal closed -- held %lld ms",
                    static_cast<long long>(NowMs() - g_openedAtMs));
        }
        g_openLogged = false;
    }
    // The OPEN marker, and the window's other end. Both are emitted from the SURFACE
    // state, ABOVE the empty-store early return below, and report only store facts.
    // They were once emitted at the end of the draw, and an injected run caught it:
    // with nothing to show, Draw returned before reaching them, so the marker did not
    // fire until the next message arrived -- and that message then fell OUTSIDE the
    // window it was supposed to be inside. An instrument whose window moves with the
    // thing it measures is not an instrument.
    if (open && reveal >= 1.f && !g_openLogged) {
        g_openLogged = true;
        g_openedAtMs = NowMs();
        UE_LOGI("chat_view: reveal open -- history=%d live=%d",
                s.count - s.liveCount, s.liveCount);
    }
    if (s.count <= 0) return;

    const ImGuiIO& io = ImGui::GetIO();
    const float pad = S(14.f);
    const float anchorBottomY = io.DisplaySize.y * kBottomFrac;

    // 2026-07-04 chat-imgui-samp: bold chat font (Cyrillic-capable), per-slot colored
    // nick prefix, 4-way outline (reads over any scene), word-wrap at a fixed width.
    ImFont* font = ui::fonts::FontFor(ui::fonts::Role::Chat);
    if (!font) font = ImGui::GetFont();
    if (!font) return;
    // PxFor(Chat) = the px the chat font was BAKED at (scaled): drawing at exactly that
    // size renders the crisp 1:1 rasterization, no bitmap resample.
    const float px = ui::fonts::PxFor(ui::fonts::Role::Chat);
    const float wrapW = std::min(io.DisplaySize.x * 0.42f, S(640.f));
    const float rowH = px + S(2.f);
    const float o = std::max(1.f, S(1.f));  // outline offset

    // How tall the reveal may grow. The bottom edge is fixed at the feed's anchor and
    // it grows UPWARD, so this is what decides where the top of the history sits.
    // A third is cut off the available run to the top of the screen (user 2026-07-29:
    // "the history box is too high") -- a block reaching the top edge reads as taking
    // the screen over rather than as a panel you opened.
    constexpr float kRevealHeightFrac = 2.f / 3.f;
    const float budget = (anchorBottomY - pad) * kRevealHeightFrac;
    const float topLimit = anchorBottomY - budget;
    const int maxRows = std::max(1, static_cast<int>(budget / rowH));

    // Visual-row iterator: invokes fn(rowStart, rowEnd) once per wrapped row of `text`.
    // The SAME split feeds the measure pass and the draw pass so they never disagree.
    auto forEachRow = [&](const char* text, auto&& fn) {
        const char* p   = text;
        const char* end = text + std::strlen(text);
        while (p < end) {
            const char* rowEnd = font->CalcWordWrapPositionA(px / font->FontSize, p, end, wrapW);
            if (rowEnd == p) rowEnd = p + 1;  // never stall on a single overlong glyph
            fn(p, rowEnd);
            p = rowEnd;
            while (p < end && *p == ' ') ++p;  // swallow the wrap-point space
        }
    };

    // The composed alpha: the store's TTL curve, floored by the reveal. One expression
    // for both tiers -- a retained row's store alpha is 0, so it IS the reveal; a live
    // row goes opaque while the surface is up and falls back to its own fade on close.
    auto drawnAlpha = [&](int i) {
        return std::clamp(std::max(s.lines[i].alpha, reveal), 0.f, 1.f);
    };

    // Build the visible rows NEWEST-first into the back of a fixed array, so running
    // out of room drops the OLDEST history rather than the messages just sent.
    Row rows[kRowCap];
    int first = kRowCap;
    for (int i = s.count - 1; i >= 0 && first > 0; --i) {
        if (drawnAlpha(i) < kAlphaFloor) continue;
        Row tmp[kEntryRowCap];
        int nt = 0;
        forEachRow(s.lines[i].text, [&](const char* b, const char* e) {
            if (nt < kEntryRowCap) { tmp[nt] = Row{i, nt, b, e}; ++nt; }
        });
        if (nt == 0 || nt > first) break;  // whole entries only; oldest drops off
        first -= nt;
        std::memcpy(&rows[first], tmp, sizeof(Row) * static_cast<size_t>(nt));
    }
    const int nRows = kRowCap - first;
    if (nRows <= 0) return;
    const Row* row = &rows[first];

    // ---- where the bottom of the view sits, in ROWS.
    int bottom = nRows - 1;
    if (g_pinned) {
        // The anchor is a sort key, so a live line retiring into history keeps it in
        // the ordered set: only an eviction can remove it. If that happens, clamp to
        // the oldest row still present rather than silently jumping to the newest.
        int found = -1;
        for (int i = 0; i < nRows; ++i) {
            if (s.lines[row[i].line].key == g_anchorKey && row[i].sub == g_anchorSub) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            for (int i = 0; i < nRows; ++i) {
                if (s.lines[row[i].line].key >= g_anchorKey) { found = i; break; }
            }
        }
        bottom = (found >= 0) ? found : nRows - 1;
    }

    // The pin is DECIDED ONCE, after the clamps -- a key press only states an intent.
    // Committing it inside the key handler pinned a view that never moved: with fewer
    // rows than the viewport holds, PgUp's overshoot clamps straight back to the
    // newest line, so the pin was set and then immediately cleared, and both edges
    // were announced. Caught by the injected drill run, where an emptied history left
    // exactly one row on screen and the pager still claimed to have paged.
    bool wantPin = g_pinned;
    if (open) {
        const int page = std::max(1, maxRows - 1);  // one row of carried context
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true)) {
            bottom -= page;
            wantPin = true;
        }
        if (g_pinned && ImGui::IsKeyPressed(ImGuiKey_PageDown, true)) {
            bottom += page;
        }
    }
    // Clamp to the OLDEST full viewport first; then, at (or past) the newest row,
    // there is nothing to be pinned TO.
    const int floorBottom = std::min(nRows - 1, maxRows - 1);
    if (bottom < floorBottom) bottom = floorBottom;
    if (bottom >= nRows - 1) {
        bottom = nRows - 1;
        wantPin = false;
    }
    SetPinned(wantPin);
    if (g_pinned) {
        g_anchorKey = s.lines[row[bottom].line].key;
        g_anchorSub = row[bottom].sub;
    }

    // One extra row above the budget, clipped, so a pageable history reads as
    // continuing rather than as a clean edge that looks like the end of it.
    const int top = std::max(0, bottom - maxRows);
    const int shown = bottom - top + 1;

    // BACKGROUND draw list: over the scene, under real windows (menus/scoreboard) --
    // the same layer the nameplates use. Drawn after them so chat wins on overlap.
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(ImVec2(0.f, topLimit), ImVec2(io.DisplaySize.x, anchorBottomY + rowH), true);
    float y = anchorBottomY - rowH * static_cast<float>(shown);

    for (int r = top; r <= bottom; ++r) {
        const auto& l = s.lines[row[r].line];
        const float a = drawnAlpha(row[r].line);
        const ImU32 outline = IM_COL32(0, 0, 0, static_cast<int>(a * 200.f));
        // Peer-action lines ("<nick> deleted an email: X") draw their predicate in
        // yellow (user 2026-07-11) so world-state actions read apart from typed chat.
        const ImU32 body = l.action
            ? IM_COL32(255, 214,  80, static_cast<int>(a * 245.f))
            : IM_COL32(236, 236, 236, static_cast<int>(a * 245.f));
        ImU32 nickCol = body;
        if (l.nickLen > 0) {
            // Frozen at compose time (coop::chat_nick_color), stored ARGB; ImU32 is ABGR.
            nickCol = IM_COL32((l.nickArgb >> 16) & 0xFFu, (l.nickArgb >> 8) & 0xFFu,
                               l.nickArgb & 0xFFu, static_cast<int>(a * 255.f));
        }
        const size_t textLen = std::strlen(l.text);
        const char* nickEnd = l.text + ((l.nickLen && l.nickLen < textLen) ? l.nickLen : 0);

        float x = pad;
        const char* seg = row[r].b;
        while (seg < row[r].e) {
            // Split the row at the nick boundary when it falls inside this row.
            const char* segEnd = (seg < nickEnd && nickEnd < row[r].e) ? nickEnd : row[r].e;
            const ImU32 col = (seg < nickEnd) ? nickCol : body;
            dl->AddText(font, px, ImVec2(x - o, y), outline, seg, segEnd);
            dl->AddText(font, px, ImVec2(x + o, y), outline, seg, segEnd);
            dl->AddText(font, px, ImVec2(x, y - o), outline, seg, segEnd);
            dl->AddText(font, px, ImVec2(x, y + o), outline, seg, segEnd);
            dl->AddText(font, px, ImVec2(x, y), col, seg, segEnd);
            x += font->CalcTextSizeA(px, FLT_MAX, 0.f, seg, segEnd).x;
            seg = segEnd;
        }
        y += rowH;
    }
    dl->PopClipRect();
}

}  // namespace ui::chat_view
