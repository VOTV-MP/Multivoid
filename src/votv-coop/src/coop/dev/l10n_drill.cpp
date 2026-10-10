// coop/dev/l10n_drill.cpp -- see coop/dev/l10n_drill.h.

#include "coop/dev/l10n_drill.h"

#include "coop/comms/chat_feed.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "coop/session/player_handshake.h"
#include "coop/text/utf8_codec.h"
#include "l10n/l10n.h"
#include "ui/atlas_watch.h"
#include "ui/fonts.h"
#include "ui/dev_menu.h"
#include "ui/imgui_overlay.h"

#include "ue_wrap/core/log.h"

#include <cstring>
#include <functional>
#include <string>

namespace coop::dev::l10n_drill {
namespace {

enum class Expect : uint8_t { Off, Translated, English, Atlas };

Expect ExpectNow() {
    static const Expect e = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::l10n_drill);
        return v == "translated" ? Expect::Translated
             : v == "english"    ? Expect::English
             : v == "atlas"      ? Expect::Atlas
                                 : Expect::Off;
    }();
    return e;
}

const char* ExpectName() { return ExpectNow() == Expect::Translated ? "translated" : "english"; }

enum class Step : uint8_t { WaitWorld, OpenRules, WaitRules, OpenStats, WaitStats, WaitJoinLine, Flood, Done };
Step g_step = Step::WaitWorld;
bool g_menuOpened = false;

// The join line each role composes, as player_handshake looks it up: the host's names the client
// that joined, the client's the host whose game it joined.
constexpr const char* kHostJoinLine = "%1$s joined the game";
constexpr const char* kClientJoinLine = "Joined %1$s's game";

void CloseMenu() {
    if (!g_menuOpened) return;
    ui::imgui_overlay::SetVisible(false);
    g_menuOpened = false;
}

// Which form of the join line the feed holds: the line is an event line, one colour, so a row's text
// is the whole line. Waiting for either exact form is the readiness (the "connecting" line names the
// same peer and must not count), and which one arrived is the verdict on the line's composition.
enum class Line : uint8_t { None, English, Translated };
Line JoinLineInFeed(const char* msgid, const std::string& nick) {
    char english[256], translated[256];
    if (nick.empty() || l10n::Fmt(english, sizeof(english), msgid, nick.c_str()) < 0) return Line::None;
    const char* tr = l10n::T(msgid);
    const bool differs = tr != msgid && l10n::Fmt(translated, sizeof(translated), tr, nick.c_str()) >= 0 &&
                         std::strcmp(translated, english) != 0;
    Line seen = Line::None;
    coop::chat_feed::ForEachRow([&](const coop::chat_feed::RowView& r) {
        if (differs && r.line == translated) seen = Line::Translated;
        else if (seen == Line::None && r.line == english) seen = Line::English;
    });
    return seen;
}

void Judge(Line join) {
    const char* probes[] = {"Rules", "Stats"};
    int found = 0;
    const char* firstMissing = nullptr;
    const char* firstFound = nullptr;
    for (const char* p : probes) {
        if (l10n::WasFound(nullptr, p)) {
            ++found;
            if (!firstFound) firstFound = p;
        } else if (!firstMissing) {
            firstMissing = p;
        }
    }
    if (join == Line::Translated) ++found;
    else if (!firstMissing) firstMissing = "the join line";
    if (join == Line::Translated && !firstFound) firstFound = "the join line";
    constexpr int kProbes = 3;
    const char* locale = l10n::ActiveLocale();
    g_step = Step::Done;
    if (ExpectNow() == Expect::Translated && found != kProbes) {
        UE_LOGW("[L10N-DRILL] FAIL expect=translated locale=%s missing=%s", locale[0] ? locale : "-", firstMissing);
        return;
    }
    if (ExpectNow() == Expect::English && (locale[0] || found != 0)) {
        UE_LOGW("[L10N-DRILL] FAIL expect=english locale=%s found=%s", locale[0] ? locale : "-",
                firstFound ? firstFound : "-");
        return;
    }
    UE_LOGI("[L10N-DRILL] DONE expect=%s locale=%s found=%d/%d distinct=%zu", ExpectName(), locale[0] ? locale : "-",
            found, kProbes, l10n::FoundCount());
}

// The atlas arm: the chat flooded with distinct ideographs, a line at a time, until a pack failure or
// the end of the block -- how much peer text the atlas holds under a script face, since the chat
// baked keeps every glyph it ever drew. After each line two censuses, so a frame has drawn the line
// before the second answers; a progress line each kFloodReportEvery ideographs.
constexpr uint32_t kFloodFirst = 0x4E00, kFloodLast = 0x9FFF;
constexpr uint32_t kFloodPerLine = 40;
constexpr uint32_t kFloodReportEvery = 2000;
uint32_t g_floodNext = kFloodFirst;
uint32_t g_floodRequest = 0;
bool g_floodSecond = false;

void FloodStep() {
    if (g_floodRequest == 0) {   // the next line
        std::wstring line;
        for (uint32_t k = 0; k < kFloodPerLine && g_floodNext <= kFloodLast; ++k)
            line.push_back(static_cast<wchar_t>(g_floodNext++));
        coop::chat_feed::Push(line, coop::chat_feed::Keep::Transient);
        g_floodRequest = ui::atlas_watch::RequestCensus();
        g_floodSecond = false;
        return;
    }
    ui::atlas_watch::Census c;
    if (!ui::atlas_watch::CensusFor(g_floodRequest, &c)) return;
    if (!g_floodSecond) {
        g_floodRequest = ui::atlas_watch::RequestCensus();
        g_floodSecond = true;
        return;
    }
    g_floodRequest = 0;
    const uint32_t drawn = g_floodNext - kFloodFirst;
    const double full = c.maxW > 0 && c.maxH > 0 ? 100.0 * c.packedPx / (double(c.maxW) * c.maxH) : 0.0;
    if (c.failures == 0 && g_floodNext <= kFloodLast) {
        if (drawn % kFloodReportEvery < kFloodPerLine)
            UE_LOGI("[L10N-DRILL] atlas: %u ideographs drawn, atlas %dx%d, packed %d px (%.1f%% of %dx%d), chat %.1f px "
                    "at scale %.2f", drawn, c.texW, c.texH, c.packedPx, full, c.maxW, c.maxH, c.chatPx, c.uiScale);
        return;
    }
    g_step = Step::Done;
    UE_LOGI("[L10N-DRILL] DONE expect=atlas locale=%s pushed=%u atlas=%dx%d packed=%d px (%.1f%% of %dx%d) "
            "failures=%d first=U+%04X chat=%.1fpx scale=%.2f", l10n::ActiveLocale(), drawn, c.texW, c.texH, c.packedPx,
            full, c.maxW, c.maxH, c.failures, c.firstFailure, c.chatPx, c.uiScale);
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (ExpectNow() == Expect::Off || g_step == Step::Done || !session || !session->running()) return;
    const bool isHost = session->role() == coop::net::Role::Host;
    switch (g_step) {
    case Step::WaitWorld:
        if (isHost ? !session->IsSlotWorldReady(1) : !coop::net_pump::HasAnnouncedWorldReady()) return;
        if (ExpectNow() == Expect::Atlas) {
            if (!ui::fonts::ScriptFaceActive()) {
                g_step = Step::Done;
                UE_LOGW("[L10N-DRILL] FAIL expect=atlas locale=%s -- the arm floods ideographs and needs a "
                        "language whose script a Windows face draws", l10n::ActiveLocale()[0] ? l10n::ActiveLocale() : "-");
                return;
            }
            g_step = Step::Flood;
            return;
        }
        ui::imgui_overlay::SetVisible(true);
        g_menuOpened = true;
        g_step = Step::OpenRules;
        return;
    case Step::OpenRules:
        ui::dev_menu::RequestSelect("World", "Rules");
        g_step = Step::WaitRules;
        return;
    case Step::WaitRules:
        if (!ui::dev_menu::DrawnSelection("World", "Rules")) return;
        g_step = Step::OpenStats;
        return;
    case Step::OpenStats:
        ui::dev_menu::RequestSelect("Network", "Stats");
        g_step = Step::WaitStats;
        return;
    case Step::WaitStats:
        if (!ui::dev_menu::DrawnSelection("Network", "Stats")) return;
        CloseMenu();
        g_step = Step::WaitJoinLine;
        return;
    case Step::WaitJoinLine: {
        const std::wstring other = coop::player_handshake::NicknameForSlot(isHost ? 1 : 0);
        const Line join = JoinLineInFeed(isHost ? kHostJoinLine : kClientJoinLine, coop::text::ToUtf8(other));
        if (join == Line::None) return;
        Judge(join);
        return;
    }
    case Step::Flood:
        FloodStep();
        return;
    case Step::Done:
        return;
    }
}

void OnDisconnect() {
    CloseMenu();
    g_step = Step::WaitWorld;
    g_floodNext = kFloodFirst;
    g_floodRequest = 0;
}

}  // namespace coop::dev::l10n_drill
