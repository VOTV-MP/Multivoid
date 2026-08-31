// ui/server_browser_panels.cpp -- see ui/server_browser_panels.h.

#include "ui/server_browser_panels.h"

#include "coop/net/lobby_client.h"
#include "coop/net/protocol.h"            // kProtocolVersion -- which side must update
#include "coop/session/session_manager.h"
#include "coop/text/utf8_codec.h"         // the ONE owner of text encoding: a server NAME
                                          // and a status sentence both arrive as UTF-8
#include "ui/native_screen.h"
#include "ui/server_browser_actions.h"    // LastOutcome -- the sentence a click reached
#include "ui/server_browser_rows.h"       // the selection, the count, the fetch clock
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <windows.h>

#include <string>

namespace ui::server_browser_panels {
namespace {

namespace E  = ue_wrap::engine;
namespace U  = ue_wrap::umg;
namespace P  = ue_wrap::profile;
namespace NS = ui::native_screen;
namespace sm = coop::session_manager;
namespace rows = ui::server_browser_rows;

using ue_wrap::FLinearColor;

const FLinearColor kText   = NS::Text();
const FLinearColor kAccent = NS::Accent();
const FLinearColor kDim    = NS::Dim();
const FLinearColor kBad    = NS::Bad();
const FLinearColor kAmber  = NS::Amber();
const FLinearColor kPanel  = NS::Panel();
const FLinearColor kBlack  = NS::Black();

constexpr float kBorderPx = 2.f;

// ONE LINE OF EITHER PANE: the widget, and the string it last rendered.
//
// The cache is the whole performance story of this module (header). It is a value member
// rather than a pointer into a table so that "did this change" cannot accidentally be asked
// of a different line than the one being written.
struct Line {
    void*       w = nullptr;
    std::string last;
    // Written ONLY on a change; the return says whether the engine was touched, which the
    // one-shot build log uses and nothing else does.
    bool Set(const std::string& utf8) {
        if (!w || utf8 == last) return false;
        const bool wasEmpty = last.empty();
        last = utf8;
        const std::wstring wide = coop::text::FromUtf8Lossy(utf8.data(), utf8.size());
        E::SetWidgetText(w, wide.c_str());
        // AN EMPTY LINE IS COLLAPSED, NOT BLANK. A UTextBlock with no text still reports its
        // font's LINE HEIGHT as its desired size, so the four status lines that are silent
        // most of the time (the alarm, the notice, the update line) each held ~20 px of
        // nothing and the pane read as a box with holes punched in it. Written on the
        // EMPTINESS EDGE only, so a line whose text merely changed costs no visibility
        // dispatch. ESlateVisibility: Visible=0, Collapsed=1.
        if (wasEmpty != utf8.empty()) E::SetWidgetVisibility(w, utf8.empty() ? 1 : 0);
        return true;
    }
    // The COLOUR changes on some lines (the version line goes red on a mismatch), and it is
    // cached for the same reason the text is: SetTextBlockColorDispatch is a dispatch too.
    void SetColor(const FLinearColor& c) {
        if (!w) return;
        if (haveColor && c.R == color.R && c.G == color.G && c.B == color.B && c.A == color.A)
            return;
        color = c;
        haveColor = true;
        E::SetTextBlockColorDispatch(w, c);
    }
    FLinearColor color{};
    bool         haveColor = false;
};

// ---- the details panel ---------------------------------------------------------------
// One label line per fact, in the order the save browser reads: what it IS, then whether
// you can join it, then how busy and how fresh.
Line g_dName, g_dWorld, g_dVersion, g_dPlayers, g_dConn, g_dSeen;

// ---- the status pane -------------------------------------------------------------------
Line g_sCount, g_sFresh, g_sAlarm, g_sNotice, g_sUpdate, g_sNick;

uint64_t g_noticeUntilMs = 0;
std::string g_notice;
uint64_t g_lastPaintMs = 0;
// A second, because the fastest thing on either pane is a whole-seconds counter. Anything
// faster would repaint identical text; anything slower would visibly lag the clock.
constexpr uint64_t kPaintEveryMs = 1000;
constexpr uint64_t kNoticeMs     = 6000;

// A titled section inside a framed box: the orange header, then the caller's lines.
//
// `fixedH > 0` wraps the box in a SizeBox. The details panel wants that and the status pane
// does not: the details panel's line COUNT changes with the selection (two lines when
// nothing is chosen, seven when something is), and letting the box breathe would slide the
// black pane under it up and down every time the player clicked a different row. A pane
// that moves when you use it is harder to read than one with space in it.
void* SectionBody(void* parent, const FLinearColor& fill, const wchar_t* title,
                  float parentWeight, float fixedH) {
    void* holder = fixedH > 0.f ? NS::Spawn(L"SizeBox", parent) : parent;
    if (!holder) return nullptr;
    void* box = NS::AddFramedBox(holder, fill, kBorderPx);
    void* col = box ? NS::Spawn(L"VerticalBox", box) : nullptr;
    if (!box || !col) return nullptr;
    if (void* s = U::AddChild(box, col)) {
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign,
                        NS::kFill, NS::kTop);
        NS::SetSlotPadding(s, P::off::UOverlaySlot_Padding, 10.f, 8.f, 10.f, 8.f);
    }
    if (title) NS::AddText(col, title, 18, kAccent, NS::kJustLeft, 0.f);
    if (fixedH > 0.f) {
        U::SetSizeBoxHeight(holder, fixedH);
        U::SetContent(holder, box);
        if (void* s = NS::AddVFill(parent, holder, parentWeight, NS::kFill, NS::kTop))
            NS::SetSlotPadding(s, P::off::UVerticalBoxSlot_Padding, 0.f, 0.f, 0.f, 6.f);
    } else {
        NS::AddVFill(parent, box, parentWeight, NS::kFill, NS::kFill);
    }
    return col;
}

// One detail line. Auto-sized in a vertical box, so no weight.
//
// BORN COLLAPSED, because it is born EMPTY and `Line::Set` only toggles visibility on the
// emptiness EDGE -- a line that starts blank and stays blank would never reach that edge and
// would hold a line height of nothing forever.
void* DetailLine(void* col, int32_t size, const FLinearColor& c) {
    void* t = NS::AddText(col, L"", size, c, NS::kJustLeft, 0.f);
    if (t) {
        U::SetClipping(t, 1);   // a 40-char world name must not paint over the list
        E::SetWidgetVisibility(t, 1);   // ESlateVisibility::Collapsed
    }
    return t;
}

std::string Sec(int s) { return std::to_string(s) + "s ago"; }

}  // namespace

bool BuildDetails(void* parent) {
    // Sized for the full set: the "Server info:" header at 18 plus six body lines at 16,
    // their leading, and the box's own 8 px top and bottom padding. 190 was measured too
    // tight by one line's descender -- "Last seen: 13s ago" touched the frame
    // (browser_row_skin_a.png, 2026-08-31).
    constexpr float kDetailsH = 204.f;
    void* col = SectionBody(parent, kPanel, L"Server info:", 0.f, kDetailsH);
    if (!col) return false;
    // The NAME is the panel's own subject and gets the emphasis the row gives it.
    g_dName    = Line{DetailLine(col, 20, kText), {}};
    g_dWorld   = Line{DetailLine(col, 16, kDim), {}};
    g_dVersion = Line{DetailLine(col, 16, kDim), {}};
    g_dPlayers = Line{DetailLine(col, 16, kDim), {}};
    g_dConn    = Line{DetailLine(col, 16, kDim), {}};
    g_dSeen    = Line{DetailLine(col, 16, kDim), {}};
    if (!g_dName.w || !g_dWorld.w || !g_dVersion.w || !g_dPlayers.w || !g_dConn.w ||
        !g_dSeen.w) {
        UE_LOGE("server_browser_panels: the details panel could not be built -- the screen "
                "would ship with a hole where the chosen server's facts go");
        return false;
    }
    return true;
}

bool BuildStatus(void* parent) {
    // NO TITLE. The save browser's black pane carries text and nothing else; a section
    // header over four status lines would be labelling the obvious.
    void* col = SectionBody(parent, kBlack, nullptr, 1.f, 0.f);
    if (!col) return false;
    g_sCount  = Line{DetailLine(col, 16, kText), {}};
    // ITS OWN LINE, not a clause on the count. "12 servers   -- updated just now" is 31
    // monospace glyphs and the pane is ~350 px, so the tail clipped
    // (browser_row_skin_a.png, 2026-08-31) -- and it would clip differently for every count
    // and every elapsed value. Two facts, two lines, and neither can crowd the other out.
    g_sFresh  = Line{DetailLine(col, 16, kDim), {}};
    g_sAlarm  = Line{DetailLine(col, 16, kBad), {}};
    g_sNotice = Line{DetailLine(col, 16, kAmber), {}};
    g_sUpdate = Line{DetailLine(col, 16, kAmber), {}};
    g_sNick   = Line{DetailLine(col, 16, kDim), {}};
    if (!g_sCount.w || !g_sFresh.w || !g_sAlarm.w || !g_sNotice.w || !g_sUpdate.w ||
        !g_sNick.w) {
        UE_LOGE("server_browser_panels: the status pane could not be built");
        return false;
    }
    return true;
}

void Forget() {
    g_dName = g_dWorld = g_dVersion = g_dPlayers = g_dConn = g_dSeen = Line{};
    g_sCount = g_sFresh = g_sAlarm = g_sNotice = g_sUpdate = g_sNick = Line{};
    g_notice.clear();
    g_noticeUntilMs = 0;
    g_lastPaintMs = 0;
}

void SetNotice(const char* utf8) {
    if (!utf8) return;
    g_notice = utf8;
    g_noticeUntilMs = ::GetTickCount64() + kNoticeMs;
    Sync(true);
}

void Sync(bool force) {
    const uint64_t now = ::GetTickCount64();
    if (!force && now - g_lastPaintMs < kPaintEveryMs) return;
    g_lastPaintMs = now;

    // ---- details -----------------------------------------------------------------
    coop::net::lobby::LobbyRow r;
    if (!rows::Selected(r)) {
        // EMPTY IS A STATE WITH ITS OWN SENTENCE, not five blank lines. A panel of empty
        // labels reads as a panel that failed to load.
        g_dName.SetColor(kDim);
        g_dName.Set("Select a server");
        g_dWorld.Set("");
        g_dVersion.Set("");
        g_dPlayers.Set("");
        g_dConn.Set("");
        g_dSeen.Set("");
    } else {
        g_dName.SetColor(kText);
        g_dName.Set(r.name);
        g_dWorld.Set("World: " + (r.world.empty() ? std::string("(unnamed)") : r.world));

        // THE VERSION LINE SAYS WHICH SIDE MUST UPDATE, because "version mismatch" alone
        // leaves the player with nothing to do about it. The pair is compared exactly (the
        // join gate is byte-equality per lobby, CLAUDE.md's versioning section), so there
        // are three distinct answers and they are not interchangeable:
        //   * a different GAME COOK cannot be ordered -- neither side is "behind", they are
        //     targeting different VOTV builds and one of them has the wrong mod release;
        //   * a LOWER build number on the host means the HOST is behind;
        //   * a HIGHER one means WE are.
        const bool gameBad = !r.game.empty() && r.game != sm::GameTarget();
        const int ourProto = static_cast<int>(coop::net::kProtocolVersion);
        const bool protoBad = r.proto > 0 && r.proto != ourProto;
        std::string ver = "Version: ";
        ver += r.game.empty() ? (r.version.empty() ? std::string("unknown") : r.version)
                              : r.game;
        if (r.proto > 0) ver += " b" + std::to_string(r.proto);
        if (gameBad) {
            ver += "  -- built for a different game version";
        } else if (protoBad) {
            ver += r.proto < ourProto ? "  -- the host must update"
                                      : "  -- you must update";
        }
        g_dVersion.SetColor(gameBad || protoBad ? kBad : kDim);
        g_dVersion.Set(ver);

        g_dPlayers.Set("Players: " + std::to_string(r.playersCur) + "/" +
                       std::to_string(r.playersMax));
        // `direct` is already parsed off the wire (lobby_client.cpp) and had no reader on
        // this screen: a direct host is port-forwarded UDP, an AUTO host is brokered P2P.
        // It is the difference between "this may need my NAT to cooperate" and "this will
        // not", which is worth one word.
        g_dConn.Set(std::string("Connection: ") + (r.direct ? "direct" : "p2p") +
                    (r.locked ? "   (locked)" : ""));
        g_dSeen.Set("Last seen: " + Sec(rows::AgeNowSec(r)));
    }

    // ---- status ------------------------------------------------------------------
    const int count = rows::Count();
    const uint64_t sinceMs = rows::MsSinceFetch();
    g_sCount.Set(std::to_string(count) + (count == 1 ? " server" : " servers"));
    // ALWAYS SAY WHEN, and say "just now" for the sub-second case rather than dropping the
    // clause. The first version wrote it only when `sinceMs > 0`, and both lab captures were
    // taken in the same millisecond as a fetch -- so the line vanished entirely and the pane
    // looked like it had lost half its sentence. A clause that appears and disappears is a
    // worse instrument than one that is always there.
    g_sFresh.Set(std::string("updated ") +
                 (sinceMs < 1000 ? "just now" : Sec(static_cast<int>(sinceMs / 1000u))));

    // THE ALARM KEYS ON CONSECUTIVE FAILURES, NEVER ON A CLOCK. Two failed attempts is the
    // master not answering either of the last two tries; "it has been 30 s since a success"
    // is also true of a player who alt-tabbed, and telling them the master is down would be
    // a claim the UI invented. See LobbyClient::ConsecutiveFailures.
    const int fails = sm::FetchFailures();
    g_sAlarm.Set(fails >= 2 ? "Cannot reach the server list. Check your connection."
                            : std::string());

    if (now >= g_noticeUntilMs) g_notice.clear();
    g_sNotice.Set(g_notice);

    bool outdated = false;
    const std::string latest = sm::LatestVersionLine(&outdated);
    g_sUpdate.Set(outdated ? latest : std::string());

    // PERSISTENT, not a transient. Which name you are about to appear under is a standing
    // fact about this screen, and it is the one the "Change name" action exists to alter.
    g_sNick.Set("Playing as " + sm::Nickname());
}

}  // namespace ui::server_browser_panels
