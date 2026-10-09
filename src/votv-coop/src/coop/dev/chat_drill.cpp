// coop/dev/chat_drill.cpp -- see coop/dev/chat_drill.h.

#include "coop/dev/chat_drill.h"

#include "coop/comms/chat_feed.h"
#include "coop/comms/chat_sync.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "coop/session/player_handshake.h"
#include "coop/text/utf8_codec.h"

#include "ue_wrap/core/log.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>

namespace coop::dev::chat_drill {
namespace {

namespace CF = coop::chat_feed;

enum class Arm : uint8_t { Off, History, Seed, I18n, Span };

Arm ArmNow() {
    static const Arm arm = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::chat_drill);
        return v == "history" ? Arm::History : v == "seed" ? Arm::Seed : v == "i18n" ? Arm::I18n
             : v == "span" ? Arm::Span : Arm::Off;
    }();
    return arm;
}

// The i18n red: this peer announces its nickname with a character it does not have.
bool BadNick() {
    static const bool v = coop::config::ResolveFlag(::coop::config_registry::rows::chat_drill_bad_nick);
    return v;
}

// One sample per index, cycled: Latin with an astral emoji, Cyrillic, Chinese, and Hebrew with
// points (combining marks). The strings travel the chat lane and the feed; a name travels another.
constexpr int kSampleCount = 4;
constexpr const char* kSamples[kSampleCount] = {
    "hello everyone \U0001F600",
    "\u043F\u0440\u0438\u0432\u0435\u0442 \u0432\u0441\u0435\u043C",
    "\u4F60\u597D\u4E16\u754C",
    "\u05E9\u05B8\u05C1\u05DC\u05D5\u05B9\u05DD \u05E2\u05D5\u05DC\u05DD",
};

constexpr std::string_view kTag = "[chat-drill] ";
constexpr int kRoles = 4;  // host, c1, c2, c3: the role is the slot
constexpr const char* kRoleName[kRoles] = {"host", "c1", "c2", "c3"};
constexpr int kMaxN = 64;         // the numbered lines the scan tracks per role
constexpr int kHistoryLines = 6;  // the history arm's lines: one full live tier
constexpr int kWindowCap = 30;    // the most window lines the seed arm's host says
constexpr int kSeedLines = 4;     // c1's lines before the joiner exists
constexpr int kNickBuf = 256;
constexpr uint64_t kPollMs = 100;
constexpr uint64_t kWindowEveryMs = 2000;
constexpr uint64_t kHoldMs = 15000;  // longer than the feed's own 11 s expiry
constexpr unsigned kJoinWaitS = 300; // a wait that includes another peer's boot or join
constexpr unsigned kWaitS = 60;      // every other wait

enum class Phase : uint8_t {
    Ready,
    HWaitSlot,    // history, host: the joiner's world is ready
    HWaitAppear,  // history, host: the six lines are in the feed
    HWaitExpire,  // history, host: none of them is live any more
    HWaitC1,      // history, host: the joiner's line, with the history open
    HHold,        // history, host: the observation window
    CWaitOpen,    // history, c1: the host's OPEN line
    SWaitC1,      // seed, host: c1's four lines
    SWindow,      // seed, host: the joiner loads; the window lines
    JWaitEnd,     // seed, c2: the host's END line
    IWaitLines,   // i18n: the other roles' lines
    Done,
};

Phase g_phase = Phase::Ready;
bool g_isHost = false;
int g_role = 0;  // 0 host, 1..3 c<slot>
bool g_chatOpened = false;
uint64_t g_phaseStartMs = 0;
unsigned g_budgetS = 0;
const char* g_what = "";
uint64_t g_lastScanMs = 0;
uint64_t g_nextWindowMs = 0;
uint64_t g_holdStartMs = 0;
int g_windowN = 0;
unsigned g_appeared = 0;  // history: bit n set once `host n` has been seen in the feed
int g_i18nHeld = 0;       // i18n: the most expected lines held at once; a rise restarts the budget

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

const char* Me() { return kRoleName[g_role]; }

void Flush() { ue_wrap::log::Flush(); }

// One scan of the feed: what the drill's rows say, by role and number, per tier.
struct Scan {
    uint8_t live[kRoles][kMaxN];
    uint8_t kept[kRoles][kMaxN];
    bool    bad[kRoles][kMaxN];  // some copy of the line does not equal its rebuild
    bool    openSeen, openLive;
    int     endN;
    int     nickLen[kRoles];  // -1 when no nick line was seen
    char    nick[kRoles][kNickBuf];
};
Scan g_scan;

int RoleOf(std::string_view s) {
    for (int r = 0; r < kRoles; ++r)
        if (s == kRoleName[r]) return r;
    return -1;
}

// The line a numbered drill line must read, past its tag and role: "<n> <sample>".
bool EqualsRebuild(std::string_view rest, int n) {
    char buf[kNickBuf];
    const int w = std::snprintf(buf, sizeof(buf), "%d %s", n, kSamples[n % kSampleCount]);
    return w > 0 && rest == std::string_view(buf, static_cast<size_t>(w));
}

void Bump(uint8_t& c) { if (c < 255) ++c; }

void Absorb(const CF::RowView& row) {
    std::string_view s = row.line;
    if (!s.starts_with(kTag)) return;
    s.remove_prefix(kTag.size());
    const size_t sp = s.find(' ');
    if (sp == std::string_view::npos) return;
    const int role = RoleOf(s.substr(0, sp));
    if (role < 0) return;
    s.remove_prefix(sp + 1);
    if (role == 0 && s == "OPEN") {
        g_scan.openSeen = true;
        if (!row.retained) g_scan.openLive = true;
    } else if (role == 0 && s.starts_with("END ")) {
        int n = 0;
        const std::string_view num = s.substr(4);
        if (std::from_chars(num.data(), num.data() + num.size(), n).ec == std::errc()) g_scan.endN = n;
    } else if (s.starts_with("nick ")) {
        const std::string_view nick = s.substr(5);
        if (nick.size() < static_cast<size_t>(kNickBuf)) {
            std::memcpy(g_scan.nick[role], nick.data(), nick.size());
            g_scan.nickLen[role] = static_cast<int>(nick.size());
        }
    } else {
        int n = -1;
        const auto r = std::from_chars(s.data(), s.data() + s.size(), n);
        if (r.ec != std::errc() || n < 0 || n >= kMaxN) return;
        if (row.retained) Bump(g_scan.kept[role][n]); else Bump(g_scan.live[role][n]);
        if (!EqualsRebuild(s, n)) g_scan.bad[role][n] = true;
    }
}

void Rescan() {
    std::memset(&g_scan, 0, sizeof(g_scan));
    g_scan.endN = -1;
    for (int& l : g_scan.nickLen) l = -1;
    static const std::function<void(const CF::RowView&)> fn = [](const CF::RowView& r) { Absorb(r); };
    CF::ForEachRow(fn);
}

int Present(int role, int n) { return g_scan.live[role][n] + g_scan.kept[role][n]; }

void Enter(Phase p, unsigned budgetS, const char* what) {
    g_phase = p;
    g_phaseStartMs = NowMs();
    g_budgetS = budgetS;
    g_what = what;
}

bool BudgetOut() { return NowMs() - g_phaseStartMs >= static_cast<uint64_t>(g_budgetS) * 1000; }

// Every exit path ends here: a chat the history arm opened is closed again.
void Finish() {
    if (g_chatOpened) {
        CF::SetChatOpen(false);
        g_chatOpened = false;
    }
    g_phase = Phase::Done;
}

void Pass() {
    UE_LOGI("[CHAT-DRILL] %s PASS", Me());
    Flush();
    Finish();
}

void Fail(const char* why) {
    UE_LOGW("[CHAT-DRILL] %s FAIL: %s", Me(), why);
    Finish();
}

// "<what> <role> <n>" -- "missing c1 0", "text of host 3": roles and indices, never chat text.
void FailAt(const char* what, int role, int n) {
    char why[64];
    std::snprintf(why, sizeof(why), "%s %s %d", what, kRoleName[role], n);
    Fail(why);
}

void AbortBudget() {
    UE_LOGW("[CHAT-DRILL] %s ABORT: %s not reached in %u s", Me(), g_what, g_budgetS);
    Finish();
}

void AbortCase(const char* why) {
    UE_LOGW("[CHAT-DRILL] %s ABORT: %s", Me(), why);
    Finish();
}

// A line through the chat's own submit call, with this peer's role in it.
void Say(const std::string& rest) {
    coop::chat_sync::QueueSend(std::string(kTag) + Me() + " " + rest);
}

void SayNumbered(int n) {
    Say(std::to_string(n) + " " + kSamples[n % kSampleCount]);
}

bool ClientReady(coop::net::Session* s) {
    return s->connected() && coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

constexpr int kExpectedPerRole = 1 + kSampleCount;  // a role's nick line and its numbered lines

// How many of the other roles' expected lines the feed holds: the i18n wait's progress.
int HeldOthers() {
    int held = 0;
    for (int r = 0; r < kRoles; ++r) {
        if (r == g_role) continue;
        if (g_scan.nickLen[r] >= 0) ++held;
        for (int n = 0; n < kSampleCount; ++n)
            if (Present(r, n) > 0) ++held;
    }
    return held;
}

// A slot's roster name comes with the join and can land after the role's chat lines. The
// ledger's display name is never empty (a placeholder stands in), so this reads the row.
bool RosterNameKnown(int slot) { return !coop::roster_ledger::Get(slot).nick.empty(); }

bool RosterNamesKnown() {
    for (int r = 0; r < kRoles; ++r)
        if (r != g_role && !RosterNameKnown(r)) return false;
    return true;
}

// What the i18n wait still lacks, by role and index: "c1 nick, c2 0, c3 name".
std::string MissingOthers() {
    std::string out;
    const auto add = [&out](int role, const std::string& what) {
        if (!out.empty()) out += ", ";
        out += kRoleName[role];
        out += ' ';
        out += what;
    };
    for (int r = 0; r < kRoles; ++r) {
        if (r == g_role) continue;
        if (g_scan.nickLen[r] < 0) add(r, "nick");
        for (int n = 0; n < kSampleCount; ++n)
            if (Present(r, n) == 0) add(r, std::to_string(n));
        if (!RosterNameKnown(r)) add(r, "name");
    }
    return out;
}

void JudgeI18n() {
    for (int r = 0; r < kRoles; ++r) {
        if (r == g_role) continue;
        for (int n = 0; n < kSampleCount; ++n)
            if (g_scan.bad[r][n]) { FailAt("text of", r, n); return; }
        const std::string shown = coop::text::ToUtf8(coop::player_handshake::NicknameForSlot(r));
        if (shown != std::string_view(g_scan.nick[r], static_cast<size_t>(g_scan.nickLen[r]))) {
            Fail((std::string("nick of ") + kRoleName[r]).c_str());
            return;
        }
    }
    Pass();
}

// c2's verdict: c1's lines first (a suppressed seed reads as c1 0 missing), then the host's window
// lines, each exactly once; then every one of them equals its rebuild.
void JudgeSeed() {
    const int windowN = g_scan.endN;
    for (int pass = 0; pass < 2; ++pass) {
        const int role = pass == 0 ? 1 : 0;
        const int count = pass == 0 ? kSeedLines : windowN;
        for (int n = 0; n < count && n < kMaxN; ++n) {
            const int seen = Present(role, n);
            if (seen == 0) { FailAt("missing", role, n); return; }
            if (seen > 1) { FailAt("duplicate", role, n); return; }
        }
    }
    for (int pass = 0; pass < 2; ++pass) {
        const int role = pass == 0 ? 1 : 0;
        const int count = pass == 0 ? kSeedLines : windowN;
        for (int n = 0; n < count && n < kMaxN; ++n)
            if (g_scan.bad[role][n]) { FailAt("text of", role, n); return; }
    }
    Pass();
}

// The start of every arm, once this peer is ready. False while it is not.
// The span arm, on every peer and in memory: a peer-action line whose nick a translated sentence put
// mid-line keeps that span through the feed and reads back as it was pushed; a span the 255-byte cut
// reaches is dropped, never kept as a coloured tail; a chat row's prefix reads as before. It tests the
// feed, not a catalogue: the lines are pushed directly, as the peer-action feed pushes its own.
void JudgeSpan() {
    const std::string mid = "[chat-drill] span: before Nick after";
    // The nick begins at byte 252 and ends at 256, one past the 255 bytes a line keeps: the feed
    // formats into a 256-byte buffer, so no span begins past 255.
    std::string cut = "[chat-drill] spancut ";
    cut += std::string(252 - cut.size(), 'x') + "Nick";
    const auto at = static_cast<uint8_t>(mid.find("Nick"));
    CF::PushAction(mid, at, 4, 0xFFFFFFFFu);
    CF::PushAction(cut, static_cast<uint8_t>(cut.size() - 4), 4, 0xFFFFFFFFu);
    bool midOk = false, cutOk = false;
    CF::ForEachRow([&](const CF::RowView& r) {
        if (r.line.starts_with("[chat-drill] span: "))
            midOk = r.nick == "Nick" && r.nickBegin == at && r.line == mid;
        else if (r.line.starts_with("[chat-drill] spancut "))
            cutOk = r.nick.empty() && r.nickBegin == 0;
    });
    if (!midOk) Fail("a mid-line nick did not read back as its span");
    else if (!cutOk) Fail("a nick past the cut kept a span");
    else Pass();
}

bool Begin(coop::net::Session* session) {
    g_isHost = session->role() == coop::net::Role::Host;
    if (!g_isHost && !ClientReady(session)) return false;
    g_role = g_isHost ? 0 : static_cast<int>(coop::players::Registry::Get().LocalPeerId());
    g_appeared = 0;
    g_windowN = 0;
    g_lastScanMs = 0;
    if (g_role >= kRoles || (!g_isHost && g_role < 1)) {
        // Only a client reaches this (the host's role is 0), and a slot past the cast has no
        // kRoleName entry: its line is the normal "c<slot>" shape.
        UE_LOGW("[CHAT-DRILL] c%d ABORT: this slot has no role in the drill", g_role);
        g_phase = Phase::Done;
        return false;
    }
    g_i18nHeld = 0;
    switch (ArmNow()) {
    case Arm::History:
        if (g_role == 0) Enter(Phase::HWaitSlot, kJoinWaitS, "slot 1 world-ready");
        else if (g_role == 1) Enter(Phase::CWaitOpen, kJoinWaitS, "host OPEN");
        else AbortCase("the history arm has no part for this role");
        break;
    case Arm::Seed:
        if (g_role == 0) {
            Enter(Phase::SWaitC1, kJoinWaitS, "c1 0..3 in the host's feed");
        } else if (g_role == 1) {
            for (int n = 0; n < kSeedLines; ++n) SayNumbered(n);
            Finish();
        } else if (g_role == 2) {
            Enter(Phase::JWaitEnd, kWaitS, "host END");
        } else {
            AbortCase("the seed arm has no part for this role");
        }
        break;
    case Arm::I18n: {
        const std::string nick = coop::text::ToUtf8(coop::config::ReadNickname());
        Say("nick " + nick + (BadNick() ? "~" : ""));
        for (int n = 0; n < kSampleCount; ++n) SayNumbered(n);
        Enter(Phase::IWaitLines, kJoinWaitS, "the other roles' lines");
        break;
    }
    case Arm::Span:
        JudgeSpan();
        break;
    case Arm::Off:
        g_phase = Phase::Done;
        break;
    }
    return true;
}

bool NeedsScan(Phase p) {
    switch (p) {
    case Phase::HWaitAppear: case Phase::HWaitExpire: case Phase::HWaitC1:
    case Phase::CWaitOpen: case Phase::SWaitC1: case Phase::JWaitEnd: case Phase::IWaitLines:
        return true;
    default:
        return false;
    }
}

// The history arm's host phases that read the feed (the others need the session).
void StepHistoryHost() {
    switch (g_phase) {
    case Phase::HWaitAppear: {
        for (int n = 0; n < kHistoryLines; ++n)
            if (Present(0, n) > 0) g_appeared |= 1u << n;
        if (g_appeared == (1u << kHistoryLines) - 1) Enter(Phase::HWaitExpire, kWaitS, "the six lines out of the live tier");
        else if (BudgetOut()) AbortBudget();
        break;
    }
    case Phase::HWaitExpire: {
        int live = 0, kept = 0;
        for (int n = 0; n < kHistoryLines; ++n) {
            if (g_scan.live[0][n] > 0) ++live;
            if (g_scan.kept[0][n] > 0) ++kept;
        }
        if (live > 0) {
            if (BudgetOut()) AbortBudget();
            break;
        }
        if (kept < kHistoryLines) {
            char why[32];
            std::snprintf(why, sizeof(why), "history %d/%d", kept, kHistoryLines);
            Fail(why);
            break;
        }
        CF::SetChatOpen(true);
        g_chatOpened = true;
        Say("OPEN");
        Enter(Phase::HWaitC1, kWaitS, "c1 0");
        break;
    }
    case Phase::HWaitC1:
        if (Present(1, 0) > 0) {
            g_holdStartMs = NowMs();
            Enter(Phase::HHold, 0, "");
        } else if (BudgetOut()) {
            AbortBudget();
        }
        break;
    default:
        break;
    }
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (ArmNow() == Arm::Off || !session || !session->running() || g_phase == Phase::Done) return;
    if (g_phase == Phase::Ready && !Begin(session)) return;
    const uint64_t now = NowMs();
    if (NeedsScan(g_phase)) {
        if (now - g_lastScanMs < kPollMs) return;
        g_lastScanMs = now;
        Rescan();
    }
    switch (g_phase) {
    case Phase::HWaitSlot:
        if (session->IsSlotWorldReady(1)) {
            for (int n = 0; n < kHistoryLines; ++n) SayNumbered(n);
            Enter(Phase::HWaitAppear, kWaitS, "the six lines in the feed");
        } else if (BudgetOut()) {
            AbortBudget();
        }
        return;
    case Phase::HWaitAppear:
    case Phase::HWaitExpire:
    case Phase::HWaitC1:
        StepHistoryHost();
        return;
    case Phase::HHold:
        if (now - g_holdStartMs < kHoldMs) return;
        Rescan();
        if (g_scan.live[1][0] > 0 && g_scan.openLive) Pass();
        else Fail("expired while open");
        return;
    case Phase::CWaitOpen:
        if (g_scan.openSeen) {
            SayNumbered(0);
            Finish();
        } else if (BudgetOut()) {
            AbortBudget();
        }
        return;
    case Phase::SWaitC1: {
        bool all = true;
        for (int n = 0; n < kSeedLines; ++n) all = all && Present(1, n) > 0;
        if (all) {
            Say("HAS c1 4");
            UE_LOGI("[CHAT-DRILL] host HAS c1 4");
            Flush();
            g_nextWindowMs = 0;
            Enter(Phase::SWindow, kJoinWaitS, "slot 2 world-ready");
        } else if (BudgetOut()) {
            AbortBudget();
        }
        return;
    }
    case Phase::SWindow:
        if (session->IsSlotWorldReady(2)) {
            if (g_windowN == 0) {
                AbortCase("no window line sent");
            } else {
                Say("END " + std::to_string(g_windowN));
                Finish();
            }
        } else if (session->IsSlotConnected(2) && g_windowN < kWindowCap && now >= g_nextWindowMs) {
            SayNumbered(g_windowN);
            ++g_windowN;
            g_nextWindowMs = now + kWindowEveryMs;
        } else if (BudgetOut()) {
            AbortBudget();
        }
        return;
    case Phase::JWaitEnd:
        if (g_scan.endN >= 0) JudgeSeed();
        else if (BudgetOut()) AbortBudget();
        return;
    case Phase::IWaitLines: {
        // A progress budget: each new expected line restarts it; a roster name turning up does not.
        const int held = HeldOthers();
        if (held > g_i18nHeld) {
            g_i18nHeld = held;
            g_phaseStartMs = now;
        }
        if (held == (kRoles - 1) * kExpectedPerRole && RosterNamesKnown()) {
            JudgeI18n();
        } else if (BudgetOut()) {
            AbortCase(("no new line in " + std::to_string(g_budgetS) + " s; missing " +
                       MissingOthers()).c_str());
        }
        return;
    }
    case Phase::Ready:
    case Phase::Done:
        return;
    }
}

void OnDisconnect() {
    if (g_chatOpened) CF::SetChatOpen(false);
    g_chatOpened = false;
    g_phase = Phase::Ready;
    g_windowN = 0;
    g_appeared = 0;
    g_i18nHeld = 0;
}

}  // namespace coop::dev::chat_drill
