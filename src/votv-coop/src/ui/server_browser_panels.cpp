// ui/server_browser_panels.cpp -- see ui/server_browser_panels.h.

#include "ui/server_browser_panels.h"

#include "l10n/l10n.h"

#include "coop/net/lobby_client.h"
#include "coop/net/master_slots.h"      // the list the alarm names, and whether another exists
#include "coop/net/protocol.h"            // kProtocolVersion -- which side must update
#include "coop/session/session_manager.h"
#include "coop/text/utf8_codec.h"         // the one owner of text encoding; names and status sentences arrive as UTF-8
#include "ui/native_screen.h"
#include "ui/server_browser_actions.h"    // LastOutcome -- the sentence a click reached
#include "ui/server_browser_rows.h"       // the selection, the count, the fetch clock
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <windows.h>

#include <cstring>
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

// One line of either pane: the widget, and the string it last rendered. The cache is the
// whole performance story of this module (see the header); a value member rather than a
// pointer into a table, so the did-this-change question cannot be asked of a different line
// than the one being written.
struct Line {
    void*       w = nullptr;
    std::string last;
    // Written only on a change; the return says whether the engine was touched, which the
    // one-shot build log uses and nothing else does.
    bool Set(const std::string& utf8) {
        if (!w || utf8 == last) return false;
        const bool wasEmpty = last.empty();
        last = utf8;
        const std::wstring wide = coop::text::FromUtf8Lossy(utf8.data(), utf8.size());
        E::SetWidgetText(w, wide.c_str());
        // An empty line is collapsed, not blank: a text block with no text still reports its font's
        // line height as its desired size, so the status lines that are silent most of the time
        // each held a strip of nothing and the pane read as a box with holes. Written on the
        // emptiness edge only, so a line whose text merely changed costs no visibility dispatch.
        // The visibility enum: visible 0, collapsed 1.
        if (wasEmpty != utf8.empty()) E::SetWidgetVisibility(w, utf8.empty() ? 1 : 0);
        return true;
    }
    // The colour changes on some lines (the version line goes red on a mismatch), and it is
    // cached for the same reason the text is: the colour setter is a dispatch too.
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

// The details panel: one label line per fact, in the order the save browser reads: what it
// is, then whether you can join it, then how busy and how fresh.
Line g_dName, g_dWorld, g_dVersion, g_dPlayers, g_dConn, g_dSeen;

// The status pane. The name line reads as a question answered (the player's own nick) rather
// than an activity claim: the player is looking at a list, not playing, and the value is
// their own configured nick, not a role the browser assigned.
Line g_sCount, g_sFresh, g_sAlarm, g_sNotice, g_sUpdate, g_sNick;

uint64_t g_noticeUntilMs = 0;
std::string g_notice;
uint64_t g_lastPaintMs = 0;
// A second, because the fastest thing on either pane is a whole-seconds counter. Anything
// faster would repaint identical text; anything slower would visibly lag the clock.
constexpr uint64_t kPaintEveryMs = 1000;
constexpr uint64_t kNoticeMs     = 6000;

// A titled section inside a framed box: the header, then the caller's lines. A positive
// fixed height wraps the box in a size box. The details panel wants that and the status pane
// does not: the details panel's line count changes with the selection (two lines when
// nothing is chosen, seven when something is), and letting the box breathe would slide the
// black pane under it up and down every time the player clicked a different row; a pane that
// moves when you use it is harder to read than one with space in it.
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

// One detail line, auto-sized in a vertical box, so no weight. Born collapsed, because it is
// born empty and the setter only toggles visibility on the emptiness edge; a line that
// starts blank and stays blank would never reach that edge and would hold a line height of
// nothing forever. `wrap` decides which failure mode a too-long line takes: a detail is a
// labelled value in a narrow pane and clips (a wrapped long world name would push every
// line under it down and make the pane jump); a status line is a sentence and wraps, since a
// clipped sentence reads as a rendering fault rather than an instruction.
void* DetailLine(void* col, int32_t size, const FLinearColor& c, bool wrap = false) {
    void* t = NS::AddText(col, L"", size, c, NS::kJustLeft, 0.f);
    if (t) {
        if (wrap) U::SetAutoWrapText(t, true);
        else      U::SetClipping(t, 1);
        E::SetWidgetVisibility(t, 1);   // ESlateVisibility::Collapsed
    }
    return t;
}

// How the host's players reach it, counted by the link the host measures on each: the counts behind the
// row's word, as a tail on the connection line. None when the host reports no one, which says nothing about
// who is in: a host or master older than the counts reports no one either. Each count is a phrase of its
// own; the ", " between them and the " -- " before them are punctuation.
std::string LinksTail(const coop::net::lobby::LobbyRow& r) {
    std::string parts;
    char one[256];
    const auto join = [&parts](const char* piece) {
        if (!parts.empty()) parts += ", ";
        parts += piece;
    };
    if (r.links.relayed > 0) {
        l10n::Fmt(one, sizeof(one), l10n::T("%d relayed"), r.links.relayed);
        join(one);
    }
    if (r.links.direct > 0) {
        l10n::Fmt(one, sizeof(one), l10n::T("%d direct"), r.links.direct);
        join(one);
    }
    if (r.links.lan > 0) {
        l10n::Fmt(one, sizeof(one), l10n::T("%d LAN"), r.links.lan);
        join(one);
    }
    return parts.empty() ? std::string() : " -- " + parts;
}

}  // namespace

bool BuildDetails(void* parent) {
    // Equal halves with the status pane: both take fill weight 1, so the column splits whatever
    // is left after the Connect button between them and neither is sized by a constant. That
    // also removes the fixed height this once carried, which existed because the panel's line
    // count changes with the selection and an auto-sized box would have slid the pane below it
    // on every click; a fill slot cannot do that, since its height comes from the column, not
    // its content.
    void* col = SectionBody(parent, kPanel, coop::text::FromUtf8Lossy(l10n::T("Server info:")).c_str(), 1.f, 0.f);
    if (!col) return false;
    // The name is the panel's own subject and gets the emphasis the row gives it.
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
    // No title: the save browser's black pane carries text and nothing else, and a section
    // header over four status lines would be labelling the obvious.
    void* col = SectionBody(parent, kBlack, nullptr, 1.f, 0.f);
    if (!col) return false;
    g_sCount  = Line{DetailLine(col, 16, kText), {}};
    // Its own line, not a clause on the count: the count plus an updated-ago tail exceeds the
    // pane's width in monospace glyphs and clipped, differently for every count and every
    // elapsed value. Two facts, two lines, and neither can crowd the other out.
    g_sFresh  = Line{DetailLine(col, 16, kDim), {}};
    // These three are sentences and wrap; the two around them are short facts.
    g_sAlarm  = Line{DetailLine(col, 16, kBad,   true), {}};
    g_sNotice = Line{DetailLine(col, 16, kAmber, true), {}};
    g_sUpdate = Line{DetailLine(col, 16, kAmber, true), {}};
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

    // The details.
    coop::net::lobby::LobbyRow r;
    if (!rows::Selected(r)) {
        // Empty is a state with its own sentence, not five blank lines: a panel of empty labels
        // reads as a panel that failed to load.
        g_dName.SetColor(kDim);
        g_dName.Set(l10n::T("Select a server"));
        g_dWorld.Set("");
        g_dVersion.Set("");
        g_dPlayers.Set("");
        g_dConn.Set("");
        g_dSeen.Set("");
    } else {
        g_dName.SetColor(kText);
        g_dName.Set(r.name);
        char world[256];
        l10n::Fmt(world, sizeof(world), l10n::T("World: %s"),
                  r.world.empty() ? l10n::T("(unnamed)") : r.world.c_str());
        g_dWorld.Set(world);

        // The version line says which side must update, because a bare mismatch leaves the player
        // with nothing to do. The pair is compared exactly (the join gate is byte equality per
        // lobby), so there are three distinct answers: a different game cook cannot be ordered
        // (neither side is behind; they target different game builds and one has the wrong mod
        // release); a lower build number on the host means the host is behind; a higher one means
        // we are.
        const bool gameBad = !r.game.empty() && r.game != sm::GameTarget();
        const int ourProto = static_cast<int>(coop::net::kProtocolVersion);
        const bool protoBad = r.proto > 0 && r.proto != ourProto;
        // The game target, build number and master's version are identifiers; the word "unknown" is not.
        const std::string which =
            r.game.empty() ? (r.version.empty() ? std::string(l10n::T("unknown")) : r.version)
                           : r.game;
        const std::string build = r.proto > 0 ? " b" + std::to_string(r.proto) : std::string();
        // One msgid per verdict: the clause is a sentence's end and moves with the language.
        char ver[256];
        if (gameBad) {
            l10n::Fmt(ver, sizeof(ver),
                      l10n::T("Version: %1$s%2$s  -- built for a different game version"),
                      which.c_str(), build.c_str());
        } else if (protoBad && r.proto < ourProto) {
            l10n::Fmt(ver, sizeof(ver), l10n::T("Version: %1$s%2$s  -- the host must update"),
                      which.c_str(), build.c_str());
        } else if (protoBad) {
            l10n::Fmt(ver, sizeof(ver), l10n::T("Version: %1$s%2$s  -- you must update"),
                      which.c_str(), build.c_str());
        } else {
            l10n::Fmt(ver, sizeof(ver), l10n::T("Version: %1$s%2$s"), which.c_str(),
                      build.c_str());
        }
        g_dVersion.SetColor(gameBad || protoBad ? kBad : kDim);
        g_dVersion.Set(ver);

        char players[256];
        l10n::Fmt(players, sizeof(players), l10n::T("Players: %1$d/%2$d"), r.playersCur,
                  r.playersMax);
        g_dPlayers.Set(players);
        // The direct flag is parsed off the wire and had no reader on this screen: a direct host is
        // port-forwarded UDP, an automatic host is brokered peer-to-peer, the difference between a
        // NAT that may need to cooperate and one that does not, worth one word.
        char head[256];
        l10n::Fmt(head, sizeof(head), l10n::T("Connection: %s"),
                  r.direct ? l10n::T("direct") : l10n::T("p2p"));
        std::string conn = head;
        conn += LinksTail(r);
        if (r.locked) { conn += "   "; conn += l10n::T("(locked)"); }
        g_dConn.Set(conn);
        char seen[256];
        l10n::Fmt(seen, sizeof(seen), l10n::T("Last seen: %ds ago"), rows::AgeNowSec(r));
        g_dSeen.Set(seen);
    }

    // The status.
    namespace slots = coop::net::master_slots;
    if (!sm::RowsHaveData()) {
        // Nothing has answered for this list yet -- the browser just opened, or another master
        // was chosen -- so there is neither a count nor an age to state, and dating the empty
        // list would claim a fetch that never happened. The alarm below speaks if it stays silent.
        char waiting[256];
        l10n::Fmt(waiting, sizeof(waiting), l10n::T("Waiting for the %s list..."),
                  slots::Selected().label.c_str());
        g_sCount.Set(waiting);
        g_sFresh.Set(std::string());
    } else {
        const int count = rows::Count();
        const uint64_t sinceMs = rows::MsSinceFetch();
        char countLine[256];
        l10n::Fmt(countLine, sizeof(countLine), l10n::Tn("%llu server", "%llu servers",
                  static_cast<unsigned long long>(count)), static_cast<unsigned long long>(count));
        g_sCount.Set(countLine);
        // Always say when, and say just now for the sub-second case rather than dropping the
        // clause: written only for a positive elapsed time, the line vanished on a capture taken
        // in the same millisecond as a fetch, and a clause that appears and disappears is a
        // worse instrument than one that is always there.
        if (sinceMs < 1000) {
            g_sFresh.Set(l10n::T("updated just now"));
        } else {
            char fresh[256];
            l10n::Fmt(fresh, sizeof(fresh), l10n::T("updated %ds ago"),
                      static_cast<int>(sinceMs / 1000u));
            g_sFresh.Set(fresh);
        }
    }

    // The alarm keys on consecutive failures, never on a clock: two failed attempts means the
    // master did not answer either of the last two tries, whereas a long time since a success is
    // also true of a player who alt-tabbed, and telling them the master is down would be a claim
    // the UI invented. See the lobby client's consecutive-failure count.
    // It names the list, and with another master to pick it says so: the tabs above the list are
    // the answer when one master is unreachable from where the player sits, and they are the one
    // control on this screen a player in that state would not think to try.
    const int fails = sm::FetchFailures();
    char alarm[320];
    alarm[0] = '\0';
    if (fails >= 2) {
        const std::string listName = slots::Selected().label;
        if (slots::List().size() > 1) {
            l10n::Fmt(alarm, sizeof(alarm),
                      l10n::T("Cannot reach the %s server list. Try another list above, or check "
                              "your connection."), listName.c_str());
        } else {
            l10n::Fmt(alarm, sizeof(alarm),
                      l10n::T("Cannot reach the %s server list. Check your connection."),
                      listName.c_str());
        }
    }
    g_sAlarm.Set(alarm);

    if (now >= g_noticeUntilMs) g_notice.clear();
    g_sNotice.Set(g_notice);

    // The pane shows the update line only when a newer build exists, composed here in the player's
    // language; the stored line stays English for the menu's label and the log.
    const std::string superseding = sm::LatestSuperseding();
    std::string update;
    if (!superseding.empty()) {
        char line[256];
        if (l10n::Fmt(line, sizeof(line), l10n::T("%1$s -- UPDATE AVAILABLE: %2$s"), sm::DisplayVersion().c_str(),
                      superseding.c_str()) >= 0)
            update = line;
    }
    g_sUpdate.Set(update);

    // The name everyone else will see, phrased as an answer rather than as a claim about what
    // the player is doing (see the status pane's note). The Change name button edits exactly
    // this value.
    char nick[256];
    l10n::Fmt(nick, sizeof(nick), l10n::T("Your name: %s"), sm::Nickname().c_str());
    g_sNick.Set(nick);
}

}  // namespace ui::server_browser_panels
