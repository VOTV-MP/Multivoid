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

// The feed's bottom edge, the vertical middle; the input bar sits just under this same line.
constexpr float kBottomFrac = 0.5f;

// A row's ink is drawn once its composed alpha clears this. Not a membership predicate: as one
// it conflated not-visible-yet with no-longer-exists, and a just-sent message dropped out of
// the layout for a frame. Membership is the tier (see the row build); this is only about ink.
constexpr float kAlphaFloor = 1.f / 255.f;

// The reveal ramp owns its own clock rather than integrating the frame delta, because that
// delta is whatever elapsed since the last presented overlay frame, and the overlay only
// renders when a surface is up: the first frame after a quiet lobby carries the whole quiet
// period as one delta, which would saturate a delta-integrated ramp in one frame. As an
// absolute function of the transition start it also survives the pause menu, which
// suppresses this draw while it is up. The ramp time is the store's reveal constant, and the
// store publishes the retained tier for exactly that window after a close, so the fade-out
// always has rows to draw.
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

// The scroll anchor: a sort key and a row within that entry, never an index, which shifts
// under every push and retirement. Paging moves it by whole rows, not entries: the row height
// is one constant for every wrapped row, so row paging is exactly invertible and a page up
// then down lands where it started.
bool     g_pinned = false;
uint64_t g_anchorKey = 0;
int      g_anchorSub = 0;
bool     g_frozenPublished = false;

void SetPinned(bool pinned) {
    g_pinned = pinned;
    if (pinned != g_frozenPublished) {
        // Edge-only, a handful of lines per session, and the only observable the scroll has:
        // without it a drill can press page-up and see nothing, which is also what a broken pager
        // looks like.
        UE_LOGI("chat_view: %s", pinned ? "PINNED (paged back; retention frozen)"
                                        : "FOLLOW (retention live)");
        coop::chat_feed::SetRetentionFrozen(pinned);
        g_frozenPublished = pinned;
    }
}

constexpr int kEntryRowCap = 16;   // most a single entry may wrap into

// Derived, not chosen: a hand-written ceiling below what the store can publish stopped the
// build loop early, so paging back could never reach the oldest history although the rows
// existed. The bound moves with the store by construction.
constexpr int kRowCap = coop::chat_feed::kMaxSnapshotLines * kEntryRowCap;

struct Row {
    int         line = 0;   // index into the snapshot
    int         sub = 0;    // which wrapped row of that line
    const char* b = nullptr;
    const char* e = nullptr;
};

// The wrap memo. Wrapping runs a length and a word-wrap measure over every published entry to
// draw the handful of rows that fit, and the full set is needed since page-up pages through
// it, so the fix is not recomputing an answer that did not change. The wrap depends on
// exactly four things: which rows are published (the snapshot's generation), the wrap width,
// the font pixel size and the face, since a family swap at the same size changes glyph
// advances. Holding pointers into the snapshot across frames is safe because the snapshot is
// the function-local static below, refilled only when its generation changes, the same
// generation this memo keys on. Render thread only.
Row  g_rows[kRowCap];
int  g_rowsFirst = kRowCap;   // rows live in [g_rowsFirst, kRowCap)
uint32_t g_rowsGen     = 0xFFFFFFFFu;
float    g_rowsWrapW   = -1.f;
float    g_rowsPx      = -1.f;
void*    g_rowsFont    = nullptr;  // the FACE: a swap at the same px changes advances
bool     g_rowsHistory = false;  // was the history tier in the layout for this build
bool     g_rowsValid   = false;

}  // namespace

void Draw() {
    // The snapshot is re-copied only when the store republished: with chat closed the same
    // handful of live rows, and while the reveal is up the history too.
    static coop::chat_feed::Snapshot s;
    static uint32_t gen = 0;
    coop::chat_feed::GetSnapshotIfNewer(s, gen);

    const bool open = ui::chat_input::IsOpen();
    const float reveal = Ramp(open ? 1.f : 0.f);
    if (!open) {
        SetPinned(false);   // reopening always starts at the newest line
        if (g_openLogged) {
            // The close marker bounds the window a drill asserts over: nothing-expired-while-open
            // is only checkable against a window with two ends, and the TTL resumes the instant
            // this fires.
            UE_LOGI("chat_view: reveal closed -- held %lld ms",
                    static_cast<long long>(NowMs() - g_openedAtMs));
        }
        g_openLogged = false;
    }
    // The open marker, the window's other end. Both are emitted from the surface state, above the
    // empty-store early return, and report only store facts: emitted at the end of the draw, an
    // empty store returned before reaching them, so the marker fired only when the next message
    // arrived, outside the window it was supposed to be inside.
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

    // The chat font, a per-slot coloured nick prefix, a four-way outline that reads over any scene,
    // word wrap at a fixed width.
    ImFont* font = ui::fonts::FontFor(ui::fonts::Role::Chat);
    if (!font) font = ImGui::GetFont();
    if (!font) return;
    // The pixel size the chat font was baked at: drawing at exactly that size renders the crisp
    // one-to-one rasterisation with no resample.
    const float px = ui::fonts::PxFor(ui::fonts::Role::Chat);
    const float wrapW = std::min(io.DisplaySize.x * 0.42f, S(640.f));
    const float rowH = px + S(2.f);
    const float o = std::max(1.f, S(1.f));  // outline offset

    // How tall the reveal may grow: the bottom edge is fixed at the feed's anchor and it grows
    // upward, so this decides where the top of the history sits. A third is cut off the run to
    // the top of the screen; a block reaching the top edge reads as taking the screen over rather
    // than as a panel you opened.
    constexpr float kRevealHeightFrac = 2.f / 3.f;
    const float budget = (anchorBottomY - pad) * kRevealHeightFrac;
    const float topLimit = anchorBottomY - budget;
    const int maxRows = std::max(1, static_cast<int>(budget / rowH));

    // The visual-row iterator, once per wrapped row of the text; the same split feeds the measure
    // pass and the draw pass, so they never disagree.
    auto forEachRow = [&](const char* text, auto&& fn) {
        const char* p   = text;
        const char* end = text + std::strlen(text);
        while (p < end) {
            const char* rowEnd = font->CalcWordWrapPositionA(px / font->LegacySize, p, end, wrapW);
            if (rowEnd == p) rowEnd = p + 1;  // never stall on a single overlong glyph
            fn(p, rowEnd);
            p = rowEnd;
            while (p < end && *p == ' ') ++p;  // swallow the wrap-point space
        }
    };

    // The composed alpha: the store's TTL curve, floored by the reveal. One expression for both
    // tiers: a retained row's store alpha is 0, so it is the reveal; a live row goes opaque while
    // the surface is up and falls back to its own fade on close.
    auto drawnAlpha = [&](int i) {
        return std::clamp(std::max(s.lines[i].alpha, reveal), 0.f, 1.f);
    };

    // The visible rows are built newest-first into the back of a fixed array, so running out of
    // room drops the oldest history rather than the messages just sent. Memoised on the
    // generation, the wrap width, the size, the face and whether history shows; the alpha floor
    // is not part of the key, since a row dropping below it does not change where any other row
    // wraps, and the draw re-reads alpha per row. Membership is the tier, not the alpha: a line
    // you had just sent could sit at alpha 0 for a frame (its arrival ramp and the closing reveal
    // hitting zero together), and dropping it from the layout jumped every row below it. Live
    // rows always occupy a line and leave when the store retires them; history rows do only
    // while the reveal shows them, which keeps a faded-out history from pushing the live lines
    // out of the viewport. MTA's chat has the same shape: it gates only the draw on the alpha
    // and advances the position outside that test.
    const bool showHistory = reveal > 0.f;
    const int  firstLive   = s.count - s.liveCount;
    if (!g_rowsValid || g_rowsGen != s.gen || g_rowsWrapW != wrapW ||
        g_rowsPx != px || g_rowsFont != font || g_rowsHistory != showHistory) {
        int first = kRowCap;
        for (int i = s.count - 1; i >= 0 && first > 0; --i) {
            if (i < firstLive && !showHistory) continue;  // history, not being shown
            Row tmp[kEntryRowCap];
            int nt = 0;
            forEachRow(s.lines[i].text, [&](const char* b, const char* e) {
                if (nt < kEntryRowCap) { tmp[nt] = Row{i, nt, b, e}; ++nt; }
            });
            // Continue, not break: an entry that produced no rows is an empty text, and stopping
            // there would silently drop every older row behind it. Only running out of room is a
            // reason to stop.
            if (nt == 0) continue;
            if (nt > first) break;  // whole entries only; oldest drops off
            first -= nt;
            std::memcpy(&g_rows[first], tmp, sizeof(Row) * static_cast<size_t>(nt));
        }
        g_rowsFirst = first;
        g_rowsGen     = s.gen;
        g_rowsWrapW   = wrapW;
        g_rowsPx      = px;
        g_rowsFont    = font;
        g_rowsHistory = showHistory;
        // Every build is reusable: the row set depends only on the memo key.
        g_rowsValid = true;
    }
    const int nRows = kRowCap - g_rowsFirst;
    if (nRows <= 0) return;
    const Row* row = &g_rows[g_rowsFirst];

    // Where the bottom of the view sits, in rows.
    int bottom = nRows - 1;
    if (g_pinned) {
        // The anchor is a sort key, so a live line retiring into history keeps it in the ordered
        // set; only an eviction can remove it, and then the view clamps to the oldest row still
        // present rather than jumping to the newest.
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

    // The pin is decided once, after the clamps; a key press only states an intent. Committed
    // inside the key handler, it pinned a view that never moved: with fewer rows than the
    // viewport holds, the page-up overshoot clamps straight back to the newest line, so the pin
    // was set, immediately cleared, and both edges announced.
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
    // Clamp to the oldest full viewport first; at or past the newest row there is nothing to be
    // pinned to.
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

    // One extra row above the budget, clipped, so a pageable history reads as continuing rather
    // than as a clean edge that looks like the end of it.
    const int top = std::max(0, bottom - maxRows);
    const int shown = bottom - top + 1;

    // The background draw list: over the scene, under real windows, the layer the nameplates use;
    // drawn after them, so chat wins on overlap.
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(ImVec2(0.f, topLimit), ImVec2(io.DisplaySize.x, anchorBottomY + rowH), true);
    float y = anchorBottomY - rowH * static_cast<float>(shown);

    for (int r = top; r <= bottom; ++r) {
        const auto& l = s.lines[row[r].line];
        const float a = drawnAlpha(row[r].line);
        // Skip the ink, never the slot: the row's y advances at the bottom of the loop whatever
        // happens here, so a momentarily transparent row keeps its place and nothing below it
        // jumps.
        if (a < kAlphaFloor) { y += rowH; continue; }
        const ImU32 outline = IM_COL32(0, 0, 0, static_cast<int>(a * 200.f));
        // A peer-action line draws its predicate in yellow, so a world-state action reads apart
        // from typed chat.
        const ImU32 body = l.action
            ? IM_COL32(255, 214,  80, static_cast<int>(a * 245.f))
            : IM_COL32(236, 236, 236, static_cast<int>(a * 245.f));
        ImU32 nickCol = body;
        if (l.nickLen > 0) {
            // Frozen at compose time, stored ARGB; the ImGui colour is ABGR.
            nickCol = IM_COL32((l.nickArgb >> 16) & 0xFFu, (l.nickArgb >> 8) & 0xFFu,
                               l.nickArgb & 0xFFu, static_cast<int>(a * 255.f));
        }
        const size_t textLen = std::strlen(l.text);
        const char* nickEnd = l.text + ((l.nickLen && l.nickLen < textLen) ? l.nickLen : 0);

        float x = pad;
        const char* seg = row[r].b;
        while (seg < row[r].e) {
            // The row splits at the nick boundary when it falls inside this row.
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
