// coop/interactables/sat_console_sync.cpp -- see coop/interactables/sat_console_sync.h.

#include "coop/interactables/sat_console_sync.h"

#include "coop/config/config.h"  // [dev] the lock's bound, shortened for a drill
#include "coop/interactables/sat_console_table.h"
#include "coop/items/save_record_wire.h"  // the blob's read and append primitives
#include "coop/net/blob_chunks.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/gc_pin.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/console_desk.h"  // Instance: the desk a host terminal names as its panel
#include "ue_wrap/desk/dish.h"          // IndexOf, DishByIndex, Count
#include "ue_wrap/desk/sat_console.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/engine/world_identity.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace coop::sat_console_sync {
namespace {

namespace SC = ue_wrap::sat_console;
namespace T  = coop::sat_console_table;
namespace W  = coop::save_record_wire;
namespace GT = ue_wrap::game_thread;
namespace WI = ue_wrap::world_identity;
namespace sg = ue_wrap::script_gate;
using Clock = std::chrono::steady_clock;
using coop::net::kMaxPeers;
using coop::net::ReliableKind;

// The blobs. A dish is named by its gamemode.dishs index, a console by its panel's placed actor's name.
constexpr uint8_t kOpLine = 0;  // typist -> host: [i32 dish][str name][str used panel][str line]
constexpr uint8_t kOpLog  = 1;  // host -> typist: [u8 bracketsType][str text], an answer to that typist alone
constexpr uint8_t kOpBusy = 2;  // host -> all: [str panel][u8 busy], a console's level, on each change and as a seed
constexpr uint8_t kOpShow = 3;  // host -> all: [str panel][str log], a console's printed lines, on change and as a seed

constexpr int32_t kNoDish = -1;  // init with no dish: the ROOT console

constexpr size_t kMaxLineChars   = 255;   // MTA's clamp too; every command name is short
constexpr size_t kMaxNameChars   = 64;
constexpr size_t kMaxLogChars    = 8192;  // fixLog keeps 3000; `debug rules` prints a few thousand at once
constexpr int    kLinesPerWindow = 4;     // a person types slower: lines a typist may run per window
constexpr auto   kWindow         = std::chrono::seconds(1);
constexpr auto   kSpawnEvery     = std::chrono::seconds(2);  // a typist's spawn-on-every-call commands
constexpr size_t kMaxTerminals   = 32;    // kept terminals, one a console, while the session lasts
constexpr size_t kOutboxCap      = 512;   // blobs owed to one slot while its sends are refused
constexpr auto   kResendEvery    = std::chrono::milliseconds(250);  // a refused slot is tried this often
constexpr auto   kShowEvery      = std::chrono::milliseconds(250);  // a console's lines are sent this often at most
// A console busy longer than this is stuck: its terminal is discarded, which stops its command and frees its lock.
// The longest command, sd.calall, takes well under a minute; VOTVCOOP_SAT_LOCK_TIMEOUT_S shortens it for a drill.
constexpr int64_t kStuckAfterSecs = 600;
const wchar_t* const kBusyLine = L"<r>The terminal is busy</>";  // the game's answer to a line it cannot take
const wchar_t* const kErrLine  = L"<r>err</>";                   // the game's answer to a line it cannot run

UE_FACT_FN(kRowEnterCommand, "sat_console_sync", L"ui_console_C", L"enterCommand", Declared);
UE_FACT_FN(kRowUbergraph, "sat_console_sync", L"ui_console_C", L"ExecuteUbergraph_ui_console", Declared);
UE_FACT_FN(kRowWriteToLog, "sat_console_sync", L"ui_console_C", L"writeToLog", Declared);
// The world's broken-device alerts: the shown terminal's own three, which its Construct binds, and the panel's.
UE_FACT_FN(kRowServerBroken, "sat_console_sync", L"ui_console_C", L"serverBroken", Declared);
UE_FACT_FN(kRowTowerBroken, "sat_console_sync", L"ui_console_C", L"radioTowerBroken", Declared);
UE_FACT_FN(kRowCoordBroken, "sat_console_sync", L"ui_console_C", L"coordDishBroken", Declared);
UE_FACT_FN(kRowServerAlert, "sat_console_sync", L"panel_SATconsole_C", L"Server Alert", Declared);
constexpr int kTagEnter = 0x53434E45;  // 'SCNE'
constexpr int kTagWrite = 0x53434E57;  // 'SCNW'
constexpr int kTagGraph = 0x53434E47;  // 'SCNG'
constexpr int kTagAlert = 0x53434E41;  // 'SCNA'

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched = false;
coop::blob_chunks::Assembler g_asm;
uint32_t g_seq = 1;
Clock::time_point g_nextSweep{};
Counts g_counts{};
bool g_saidOffThread = false;

// Blobs owed to a slot whose send was refused. A blob goes whole or not at all, and one refused waits
// at the head, so a slot's blobs arrive in the order they were made. The host keeps one per client's
// slot; a client keeps slot 0's, to the host. At its cap an outbox gives up its oldest blob that is not a
// busy level, which a console needs to stop being busy.
struct Outbox {
    std::deque<std::vector<uint8_t>> q;
    Clock::time_point nextTry{};
    bool saidFull = false;
};
Outbox g_out[kMaxPeers];

// HOST: the terminal each console runs on, keyed by its panel's name: a line typed at a console runs on
// that console's terminal whoever typed it, the host included, so two consoles never wait on each other,
// and a console's command and its lines are that console's whoever leaves. A terminal lasts while the
// session does and its world stays current; the session's end discards each, its command stopping there.
struct Terminal {
    std::wstring panel;
    ue_wrap::GcPin pin;
    void*   world = nullptr;  // the world it was made in, when known
    bool    busySent = false; // the busy level last told
    bool    haveContext = false;
    int32_t dish = kNoDish;
    std::wstring name;
    std::wstring logSent;     // the lines last told
    Clock::time_point nextShow{};
    std::wstring running;     // the line it runs, until its busy flag falls
    // What changed since the lane last read it, marked by the terminal's own watches: a print (writeToLog) marks
    // its lines, the end of a body (enterCommand or the ubergraph, which every latent continuation resumes) its
    // busy flag, the only places either changes.
    bool logDirty = false;
    bool busyDirty = true;
    Clock::time_point busySince{};
};
// HOST: a console's lines last told, kept when its terminal goes, so the one made after it starts from them.
std::map<std::wstring, std::wstring> g_history;
std::vector<Terminal> g_terms;

// HOST: each typist's pace, by slot (0 the host's own player).
struct Typist {
    int lines = 0;
    Clock::time_point windowStart{};
    Clock::time_point nextSpawnOk{};
};
Typist g_typists[kMaxPeers];
bool g_saidRefused[kMaxPeers] = {};
bool g_saidMalformed[kMaxPeers] = {};
bool g_saidNoTerminal[kMaxPeers] = {};
bool g_saidNoContext[kMaxPeers] = {};

// HOST: the host's own lines, refused at its terminal's enterCommand and run from Tick, never inside the watch.
struct HostLine {
    int32_t dish = kNoDish;
    std::wstring name, used, line;
};
std::deque<HostLine> g_hostLines;

// HOST: the alert lines owed to the ROOT console's terminal, written from Tick.
std::deque<std::pair<std::wstring, uint8_t>> g_rootLines;
int g_alertDepth = 0;  // inside an alert's body: its write is the ROOT console's

// EVERY PEER: what each console printed and whether it is busy, as the host tells it, and the console this
// machine's terminal shows: the panel its player last used. The shown console's lines go onto the terminal as
// they come, after whatever it printed itself; moving to another console shows that console's lines.
struct Screen {
    std::wstring log;
    bool busy = false;
};
std::map<std::wstring, Screen> g_screens;
std::wstring g_shown;
void* g_shownPanel = nullptr;  // the panel object g_shown was read from: its name is read again only when it moves
bool g_haveShown = false;
// Whether a line this terminal ran itself holds its busy flag: the shown console's level sets it while that
// console is busy and clears only what it set.
bool g_localHolds = false;

coop::net::Session* SessionIf(coop::net::Role role) {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->role() == role) ? s : nullptr;
}

// A client's text, as the host's log shows it: printable characters only, so a line cannot forge one.
std::wstring Printable(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t ch : s) out.push_back(ch < 0x20 || ch == 0x7F ? L'?' : ch);
    return out;
}

// What `now` printed after `before`: fixLog keeps the last 3000 characters, so `now` is a tail of `before`
// followed by the new lines. False when `now` holds no tail of `before` (a console whose lines were replaced).
bool TailAfter(const std::wstring& before, const std::wstring& now, std::wstring& added) {
    const size_t most = before.size() < now.size() ? before.size() : now.size();
    for (size_t j = most;; --j) {
        if (before.compare(before.size() - j, j, now, 0, j) == 0) {
            added = now.substr(j);
            return j > 0 || before.empty();
        }
        if (j == 0) return false;
    }
}

// ---- the shown console, on every peer ----

bool ShownBusy() {
    auto it = g_screens.find(g_shown);
    return g_haveShown && it != g_screens.end() && it->second.busy;
}

void PaintBusy(void* term) {
    const bool busy = ShownBusy();
    if (busy || !g_localHolds) SC::WriteProcessing(term, busy);
}

// The terminal moved to another console: that console's lines, and its level.
void Repaint(void* term) {
    auto it = g_screens.find(g_shown);
    SC::WriteLog(term, it != g_screens.end() ? it->second.log : std::wstring());
    PaintBusy(term);
}

void FollowShown() {
    void* term = SC::LocalTerminal();
    if (!term) return;
    void* const used = SC::ReadUsed(term);
    if (g_haveShown && used == g_shownPanel) return;  // the per-tick steady state: one pointer read
    g_shownPanel = used;
    const std::wstring panel = SC::PanelName(used);
    if (!g_haveShown) {
        g_haveShown = true;  // first sight: what it shows stays until a console's lines come
        g_shown = panel;
        PaintBusy(term);
        return;
    }
    if (panel == g_shown) return;
    g_shown = panel;
    Repaint(term);
}

void ApplyShow(const std::wstring& panel, const std::wstring& log) {
    Screen& sc = g_screens[panel];
    const std::wstring before = std::move(sc.log);
    sc.log = log;
    void* term = g_haveShown && panel == g_shown ? SC::LocalTerminal() : nullptr;
    if (!term) return;
    std::wstring added, cur;
    if (TailAfter(before, log, added) && SC::ReadLog(term, cur)) SC::WriteLog(term, cur + added);
    else SC::WriteLog(term, log);
}

void ApplyBusy(const std::wstring& panel, bool busy) {
    g_screens[panel].busy = busy;
    if (g_haveShown && panel == g_shown)
        if (void* term = SC::LocalTerminal()) PaintBusy(term);
}

// After this machine's terminal ran a body of its own. While the shown console is busy the flag is that
// console's: a line this terminal ran itself may have cleared it, and it is set again, the line's hold over.
// Else the flag is the line's own.
void AfterLocalBody(void* term) {
    bool busy = false;
    if (!SC::ReadProcessing(term, busy)) return;
    if (ShownBusy()) {
        if (!busy) {
            g_localHolds = false;
            SC::WriteProcessing(term, true);
        }
    } else {
        g_localHolds = busy;
    }
}

// ---- the wire ----

void Flush(uint8_t slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    Outbox& o = g_out[slot];
    while (!o.q.empty()) {
        if (!coop::blob_chunks::SendBlobToSlot(s, slot, ReliableKind::SatConsole, g_seq++, o.q.front())) {
            o.nextTry = Clock::now() + kResendEvery;
            return;
        }
        o.q.pop_front();
    }
    o.saidFull = false;
}

void Enqueue(uint8_t slot, std::vector<uint8_t> blob) {
    Outbox& o = g_out[slot];
    if (o.q.size() >= kOutboxCap) {
        for (auto it = o.q.begin(); it != o.q.end(); ++it) {
            if (!it->empty() && (*it)[0] != kOpBusy) {
                o.q.erase(it);
                break;
            }
        }
        if (!o.saidFull) {
            o.saidFull = true;
            UE_LOGW("sat_console: %zu blobs owed to slot %u and its sends still refused -- the oldest line dropped",
                    kOutboxCap, static_cast<unsigned>(slot));
        }
    }
    o.q.push_back(std::move(blob));
    if (Clock::now() >= o.nextTry) Flush(slot);
}

void SendLog(uint8_t slot, const std::wstring& text, uint8_t bracketsType) {
    std::vector<uint8_t> b;
    W::AppU8(b, kOpLog);
    W::AppU8(b, bracketsType);
    W::AppWStr(b, text);
    Enqueue(slot, std::move(b));
}

void SendBusy(uint8_t slot, const std::wstring& panel, bool busy) {
    std::vector<uint8_t> b;
    W::AppU8(b, kOpBusy);
    W::AppWStr(b, panel);
    W::AppU8(b, busy ? 1 : 0);
    Enqueue(slot, std::move(b));
}

void SendShow(uint8_t slot, const std::wstring& panel, const std::wstring& log) {
    std::vector<uint8_t> b;
    W::AppU8(b, kOpShow);
    W::AppWStr(b, panel);
    W::AppWStr(b, log);
    Enqueue(slot, std::move(b));
    ++g_counts.linesReturned;
}

// Every client whose world is ready.
template <class F>
void ToReadyClients(F&& f) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    for (int slot = 1; slot < kMaxPeers; ++slot)
        if (s->IsSlotWorldReady(slot)) f(static_cast<uint8_t>(slot));
}

// A line the host will not run is answered as the game answers one it cannot take or run: its echo, then the
// reason's line, on the typist's own terminal alone. The typist already consumed it.
void AnswerRefused(uint8_t slot, const std::wstring& line, const wchar_t* why) {
    if (slot == 0) {
        if (void* term = SC::LocalTerminal()) {
            SC::CallWriteToLog(term, line, 1);
            SC::CallWriteToLog(term, why, 0);
        }
        return;
    }
    SendLog(slot, line, 1);
    SendLog(slot, why, 0);
}

// ---- the typist ----

// A line whose command rests on the shared world goes to the console's terminal on the host, with the
// terminal's context as it stands: the dish its panel pointed it at (or ROOT), its name, and the panel last
// used, which names the console. A busy console answers the line itself, as it would alone; so does a line
// the terminal owns. The body is refused, which leaves its own tail unrun, so the input is consumed here as
// that tail does (ui_console.enterCommand: `command = ""`, the text box emptied); the echo and every line come
// back with the console's. The host's own player's lines go the same way, run from its Tick.
sg::Verdict OnEnterPre(const sg::Call& c) {
    const bool host = SessionIf(coop::net::Role::Host) != nullptr;
    if (!host && !SessionIf(coop::net::Role::Client)) return sg::Verdict::Run;
    void* term = c.object;
    if (term != SC::LocalTerminal()) return sg::Verdict::Run;
    bool busy = false;
    std::wstring line;
    if (!SC::ReadProcessing(term, busy) || busy || !SC::ReadEnterCommandLine(c.locals, line) ||
        T::Classify(line) != T::Runs::OnHost)
        return sg::Verdict::Run;
    std::wstring name;
    if (!SC::ReadName(term, name)) return sg::Verdict::Run;
    if (name.size() > kMaxNameChars) name.resize(kMaxNameChars);
    if (line.size() > kMaxLineChars) line.resize(kMaxLineChars);
    void* dish = SC::ReadActiveDish(term);
    const int32_t dishIdx = dish ? ue_wrap::dish::IndexOf(dish) : kNoDish;
    std::wstring used = SC::PanelName(SC::ReadUsed(term));
    if (used.size() > kMaxNameChars) used.clear();
    if (host) {
        g_hostLines.push_back(HostLine{dishIdx < 0 ? kNoDish : dishIdx, name, used, line});
    } else {
        std::vector<uint8_t> b;
        W::AppU8(b, kOpLine);
        W::AppI32(b, dishIdx < 0 ? kNoDish : dishIdx);
        W::AppWStr(b, name);
        W::AppWStr(b, used);
        W::AppWStr(b, line);
        Enqueue(0, std::move(b));
        ++g_counts.linesSent;
    }
    SC::ConsumeInput(term);
    UE_LOGI("sat_console: %s '%ls' runs on the host's terminal for console '%ls' (dish %d)", host ? "HOST" : "CLIENT",
            Printable(line).c_str(), used.c_str(), dishIdx);
    return sg::Verdict::Cancel;
}

void ClientOnBlob(const std::vector<uint8_t>& blob) {
    size_t o = 0;
    uint8_t op = 0;
    if (!W::RdU8(blob, o, op)) return;
    if (op == kOpLog) {
        uint8_t type = 0;
        std::wstring text;
        if (!W::RdU8(blob, o, type) || !W::RdWStr(blob, o, text) || text.size() > kMaxLogChars) {
            UE_LOGW("sat_console: a malformed line from the host -- dropped");
            return;
        }
        void* term = SC::LocalTerminal();
        if (!term || !SC::CallWriteToLog(term, text, type))
            UE_LOGW("sat_console: a line from the host found no terminal here -- dropped");
        return;
    }
    std::wstring panel;
    if (op == kOpBusy) {
        uint8_t busy = 0;
        if (!W::RdWStr(blob, o, panel) || panel.size() > kMaxNameChars || !W::RdU8(blob, o, busy)) return;
        ApplyBusy(panel, busy != 0);
        return;
    }
    if (op == kOpShow) {
        std::wstring log;
        if (!W::RdWStr(blob, o, panel) || panel.size() > kMaxNameChars || !W::RdWStr(blob, o, log) ||
            log.size() > kMaxLogChars) {
            UE_LOGW("sat_console: a malformed console from the host -- dropped");
            return;
        }
        ApplyShow(panel, log);
        return;
    }
    UE_LOGW("sat_console: op %u reached a client -- dropped", static_cast<unsigned>(op));
}

// ---- the host ----

Terminal* ByObject(void* obj) {
    if (!obj) return nullptr;
    for (Terminal& t : g_terms)
        if (t.pin.Raw() == obj) return &t;
    return nullptr;
}

Terminal* ByPanel(const std::wstring& panel) {
    for (Terminal& t : g_terms)
        if (t.panel == panel) return &t;
    return nullptr;
}

// Unpin a terminal and stop what it runs; its console, if it was busy, is told it is not.
void Discard(Terminal& t) {
    void* term = t.pin.Raw();
    if (t.busySent) {
        ToReadyClients([&](uint8_t slot) { SendBusy(slot, t.panel, false); });
        ApplyBusy(t.panel, false);
    }
    t.pin.Release();
    SC::DiscardTerminal(term);
}

void DiscardAt(size_t i) {
    Discard(g_terms[i]);
    g_terms.erase(g_terms.begin() + static_cast<std::ptrdiff_t>(i));
}

// A console's terminal, made on its first line. At the cap, an idle terminal gives way; with none, the line is
// refused.
Terminal* TerminalFor(const std::wstring& panel) {
    if (Terminal* t = ByPanel(panel)) return t;
    if (g_terms.size() >= kMaxTerminals) {
        for (size_t i = 0; i < g_terms.size(); ++i) {
            bool busy = true;
            if (SC::ReadProcessing(g_terms[i].pin.Raw(), busy) && !busy) {
                DiscardAt(i);
                break;
            }
        }
        if (g_terms.size() >= kMaxTerminals) return nullptr;
    }
    void* desk = ue_wrap::console_desk::Instance();
    void* term = desk ? SC::CreateTerminal(desk) : nullptr;
    Terminal made;
    if (!term || !made.pin.Pin(term) || !SC::WritePanel(term, desk)) {
        made.pin.Release();
        SC::DiscardTerminal(term);
        return nullptr;
    }
    made.panel = panel;
    made.world = WI::CurrentWorld();  // null while unknown: the world term then fails open
    // A console whose last terminal was discarded starts from that one's lines.
    if (auto it = g_history.find(panel); it != g_history.end() && SC::WriteLog(term, it->second))
        made.logSent = it->second;
    g_terms.push_back(std::move(made));
    UE_LOGI("sat_console: HOST made console '%ls''s terminal (%zu kept)", panel.c_str(), g_terms.size());
    return &g_terms.back();
}

// Whether a terminal stands in the context a line names. A dish index that does not resolve yet, or a
// context whose init fails, answers the line err rather than running it against none; nothing is kept,
// so the next line with the same index tries again. An index past the host's dishes is no dish at all.
enum class Context { Ready, NotYet, Bad };

Context ApplyContext(Terminal& t, int32_t dish, const std::wstring& name) {
    if (t.haveContext && dish == t.dish && name == t.name) return Context::Ready;
    void* d = nullptr;
    if (dish >= 0) {
        d = ue_wrap::dish::DishByIndex(dish);
        if (!d) {
            const int32_t count = ue_wrap::dish::Count();
            return count > 0 && dish >= count ? Context::Bad : Context::NotYet;
        }
    }
    // An init that fails may have set part of the context: nothing is kept, so the next line inits again.
    t.haveContext = false;
    if (!SC::CallInit(t.pin.Raw(), name, false, d)) return Context::NotYet;
    t.haveContext = true;
    t.dish = dish;
    t.name = name;
    return Context::Ready;
}

void HostRunLine(uint8_t slot, int32_t dish, const std::wstring& name, const std::wstring& used,
                 const std::wstring& line) {
    Typist& ty = g_typists[slot];
    const auto now = Clock::now();
    if (now - ty.windowStart >= kWindow) {
        ty.windowStart = now;
        ty.lines = 0;
    }
    const bool spawns = T::SpawnsEveryCall(line);
    if (++ty.lines > kLinesPerWindow || (spawns && now < ty.nextSpawnOk)) {
        AnswerRefused(slot, line, kBusyLine);  // the terminal cannot take the line, as the game says it
        return;
    }
    Terminal* t = TerminalFor(used);
    if (!t) {
        if (!g_saidNoTerminal[slot]) {
            g_saidNoTerminal[slot] = true;
            UE_LOGW("sat_console: HOST no terminal for console '%ls' (slot %u) -- answered err", used.c_str(),
                    static_cast<unsigned>(slot));
        }
        AnswerRefused(slot, line, kErrLine);
        return;
    }
    // A console still running a command keeps its context: its latent continuation reads the dish, name and
    // panel it started with. The game's own terminal refuses a line while busy, so does this.
    bool busy = false;
    const bool readOk = SC::ReadProcessing(t->pin.Raw(), busy);
    if (!readOk || busy) {
        AnswerRefused(slot, line, readOk ? kBusyLine : kErrLine);   // an unreadable flag is not "busy"
        return;
    }
    // A long command that writes what another console's running command writes waits for it: the host decides,
    // and the console that asked second is told which console runs it.
    for (const Terminal& o : g_terms) {
        if (&o == t || o.running.empty() || !T::Conflicts(line, o.running)) continue;
        const std::wstring who = o.name.empty() ? std::wstring(L"?") : o.name;
        const std::wstring why = L"<r>Console '" + who + L"' is running " + T::CommandOf(o.running) + L"</>";
        UE_LOGI("sat_console: HOST '%ls' at console '%ls' waits: console '%ls' runs '%ls'", Printable(line).c_str(),
                used.c_str(), o.panel.c_str(), Printable(o.running).c_str());
        AnswerRefused(slot, line, why.c_str());
        return;
    }
    // The line runs only in the context it names. One that cannot be set up is answered err and not
    // retried: a command with an effect in the world must not run twice.
    const Context ctx = ApplyContext(*t, dish < kNoDish ? kNoDish : dish, name);
    if (ctx != Context::Ready) {
        if (!g_saidNoContext[slot]) {
            g_saidNoContext[slot] = true;
            UE_LOGW("sat_console: HOST no context for slot %u's line (dish %d: %s) -- answered err",
                    static_cast<unsigned>(slot), dish,
                    ctx == Context::Bad ? "past the host's dishes" : "not resolved, or the init failed");
        }
        AnswerRefused(slot, line, kErrLine);
        return;
    }
    // The console's panel: none is a real answer (a terminal no panel named), a name that does not resolve here
    // is not, and the line does not run against no panel in its place.
    void* panel = used.empty() ? nullptr : SC::FindPanelByName(used);
    if ((!used.empty() && !panel) || !SC::WriteUsed(t->pin.Raw(), panel)) {
        if (!g_saidNoContext[slot]) {
            g_saidNoContext[slot] = true;
            UE_LOGW("sat_console: HOST no panel '%ls' for slot %u's line -- answered err", Printable(used).c_str(),
                    static_cast<unsigned>(slot));
        }
        AnswerRefused(slot, line, kErrLine);
        return;
    }
    if (spawns) ty.nextSpawnOk = now + kSpawnEvery;
    UE_LOGI("sat_console: HOST runs '%ls' for slot %u at console '%ls' (dish %d)", Printable(line).c_str(),
            static_cast<unsigned>(slot), used.c_str(), dish);
    if (!SC::RunCommand(t->pin.Raw(), line)) {
        AnswerRefused(slot, line, kErrLine);
        return;
    }
    t->running = line;
    t->logDirty = t->busyDirty = true;
    if (slot != 0) ++g_counts.linesRun;
}

void HostOnBlob(uint8_t slot, const std::vector<uint8_t>& blob) {
    size_t o = 0;
    uint8_t op = 0;
    int32_t dish = kNoDish;
    std::wstring name, used, line;
    if (!W::RdU8(blob, o, op) || op != kOpLine || !W::RdI32(blob, o, dish) || !W::RdWStr(blob, o, name) ||
        !W::RdWStr(blob, o, used) || !W::RdWStr(blob, o, line) || name.size() > kMaxNameChars ||
        used.size() > kMaxNameChars || line.size() > kMaxLineChars) {
        if (!g_saidMalformed[slot]) {
            g_saidMalformed[slot] = true;
            UE_LOGW("sat_console: HOST a malformed blob from slot %u -- dropped", static_cast<unsigned>(slot));
        }
        return;
    }
    // The host runs only a command a typist may send: a line the table keeps on its typist's machine
    // would run on the host's own compass, player or screen. A client that sends one was not built to.
    if (T::Classify(line) != T::Runs::OnHost) {
        if (!g_saidRefused[slot]) {
            g_saidRefused[slot] = true;
            UE_LOGW("sat_console: HOST refused '%ls' from slot %u -- it runs on its typist's machine",
                    Printable(line).c_str(), static_cast<unsigned>(slot));
        }
        return;
    }
    HostRunLine(slot, dish, name, used, line);
}

// The ROOT console's terminal, in its own context, for the alerts.
Terminal* RootTerminal() {
    void* root = SC::RootPanel();
    if (!root) return nullptr;
    Terminal* t = TerminalFor(SC::PanelName(root));
    if (!t) return nullptr;
    // A ROOT console made by a typist's line has its context already, and may be running that line.
    if (!t->haveContext && (ApplyContext(*t, kNoDish, L"root") != Context::Ready || !SC::WriteUsed(t->pin.Raw(), root)))
        return nullptr;
    return t;
}

void DrainHostWork() {
    while (!g_hostLines.empty()) {
        HostLine l = std::move(g_hostLines.front());
        g_hostLines.pop_front();
        HostRunLine(0, l.dish, l.name, l.used, l.line);
    }
    if (!g_rootLines.empty()) {
        Terminal* t = RootTerminal();
        UE_LOGI("sat_console: HOST %zu alert line(s) onto the ROOT console%s", g_rootLines.size(),
                t ? "" : " -- no ROOT console here, dropped");
        while (!g_rootLines.empty()) {
            if (t) SC::CallWriteToLog(t->pin.Raw(), g_rootLines.front().first, g_rootLines.front().second);
            g_rootLines.pop_front();
        }
    }
}

int64_t StuckAfterSecs() {
    static const int64_t v = [] {
        const std::string e = coop::config::ReadEnv("VOTVCOOP_SAT_LOCK_TIMEOUT_S");
        const long n = e.empty() ? 0 : std::strtol(e.c_str(), nullptr, 10);
        return n > 0 ? static_cast<int64_t>(n) : kStuckAfterSecs;
    }();
    return v;
}

// Each console's level and lines, read only when its watches marked them and sent as they change, its lines at most
// four times a second. A console busy past its bound is stuck: its terminal goes, its lines kept for the next.
void HostTellConsoles() {
    const auto now = Clock::now();
    for (size_t i = 0; i < g_terms.size();) {
        Terminal& t = g_terms[i];
        void* term = t.pin.Raw();
        if (t.busyDirty) {
            t.busyDirty = false;
            bool busy = false;
            if (SC::ReadProcessing(term, busy)) {
                if (!busy && !t.running.empty()) {
                    UE_LOGI("sat_console: HOST console '%ls' finished '%ls'", t.panel.c_str(), Printable(t.running).c_str());
                    t.running.clear();
                }
                if (busy != t.busySent) {
                    t.busySent = busy;
                    if (busy) t.busySince = now;
                    ToReadyClients([&](uint8_t slot) { SendBusy(slot, t.panel, busy); });
                    ApplyBusy(t.panel, busy);
                }
            }
        }
        if (t.logDirty && now >= t.nextShow) {
            t.logDirty = false;
            std::wstring log;
            if (SC::ReadLog(term, log) && log != t.logSent) {
                if (log.size() > kMaxLogChars) log.erase(0, log.size() - kMaxLogChars);
                t.logSent = log;
                g_history[t.panel] = log;
                t.nextShow = now + kShowEvery;
                ToReadyClients([&](uint8_t slot) { SendShow(slot, t.panel, log); });
                ApplyShow(t.panel, log);
            }
        }
        if (t.busySent && now - t.busySince >= std::chrono::seconds(StuckAfterSecs())) {
            UE_LOGW("sat_console: HOST console '%ls' busy for %lld s running '%ls' -- its terminal stopped, its lock freed",
                    t.panel.c_str(), static_cast<long long>(StuckAfterSecs()), Printable(t.running).c_str());
            DiscardAt(i);
            continue;
        }
        ++i;
    }
}

// An alert writes into the shown terminal, whichever console it shows: on the host the line is the ROOT
// console's, written there from Tick, and on a client the host's ROOT console carries it.
sg::Verdict OnWritePre(const sg::Call& c) {
    if (SessionIf(coop::net::Role::Host))
        if (Terminal* t = ByObject(c.object)) {
            t->logDirty = true;  // read once the write ran, from Tick
            return sg::Verdict::Run;
        }
    if (g_alertDepth <= 0 || !c.object || c.object != SC::LocalTerminal()) return sg::Verdict::Run;
    if (SessionIf(coop::net::Role::Host)) {
        std::wstring text;
        uint8_t type = 0;
        if (SC::ReadWriteToLogArgs(c.locals, text, type) && g_rootLines.size() < kOutboxCap) {
            if (text.size() > kMaxLogChars) text.resize(kMaxLogChars);
            g_rootLines.emplace_back(std::move(text), type);
        }
        return sg::Verdict::Cancel;
    }
    return SessionIf(coop::net::Role::Client) ? sg::Verdict::Cancel : sg::Verdict::Run;
}

sg::Verdict OnAlertPre(const sg::Call&) {
    ++g_alertDepth;
    return sg::Verdict::Run;
}

void OnAlertPost(const sg::Call&) {
    if (g_alertDepth > 0) --g_alertDepth;
}

void OnBodyPost(const sg::Call& c) {
    if (SessionIf(coop::net::Role::Host))
        if (Terminal* t = ByObject(c.object)) {
            t->busyDirty = true;
            return;
        }
    if (!SessionIf(coop::net::Role::Host) && !SessionIf(coop::net::Role::Client)) return;
    if (c.object && c.object == SC::LocalTerminal()) AfterLocalBody(c.object);
}

// The three terminal watches stand or fall together: a line refused at a terminal whose console no watch would
// show would vanish. The alert watches are their own: without them an alert shows on whichever console is up.
void EnsureWatched() {
    if (g_watched) return;
    g_watched = true;
    const bool enter = sg::WatchName(kRowEnterCommand, kTagEnter, OnEnterPre, OnBodyPost);
    const bool graph = sg::WatchName(kRowUbergraph, kTagGraph, nullptr, OnBodyPost);
    const bool write = sg::WatchName(kRowWriteToLog, kTagWrite, OnWritePre, nullptr);
    if (!(enter && graph && write)) {
        if (enter) sg::UnwatchName(kRowEnterCommand, kTagEnter, OnEnterPre, OnBodyPost);
        if (graph) sg::UnwatchName(kRowUbergraph, kTagGraph, nullptr, OnBodyPost);
        if (write) sg::UnwatchName(kRowWriteToLog, kTagWrite, OnWritePre, nullptr);
        UE_LOGE("sat_console: a terminal watch did not register -- none kept, the SAT console's lines run where typed");
        return;
    }
    const bool a1 = sg::WatchName(kRowServerBroken, kTagAlert, OnAlertPre, OnAlertPost);
    const bool a2 = sg::WatchName(kRowTowerBroken, kTagAlert, OnAlertPre, OnAlertPost);
    const bool a3 = sg::WatchName(kRowCoordBroken, kTagAlert, OnAlertPre, OnAlertPost);
    const bool a4 = sg::WatchName(kRowServerAlert, kTagAlert, OnAlertPre, OnAlertPost);
    if (!(a1 && a2 && a3 && a4))
        UE_LOGW("sat_console: an alert watch did not register (server=%d tower=%d coord=%d panel=%d) -- that alert "
                "shows on whichever console is up", a1 ? 1 : 0, a2 ? 1 : 0, a3 ? 1 : 0, a4 ? 1 : 0);
}

}  // namespace

void Install(coop::net::Session* s) {
    g_session.store(s, std::memory_order_release);
    EnsureWatched();
}

void Tick() {
    if (!GT::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    SC::EnsureResolved();
    const auto now = Clock::now();
    for (int i = 0; i < kMaxPeers; ++i)
        if (!g_out[i].q.empty() && now >= g_out[i].nextTry) Flush(static_cast<uint8_t>(i));
    // A terminal holds its world's desk, dish and panel: one made in a world that is known and no longer
    // the current one goes. An unknown world keeps it.
    if (void* world = WI::CurrentWorld()) {
        for (size_t i = 0; i < g_terms.size();) {
            if (!g_terms[i].world || g_terms[i].world == world) {
                ++i;
                continue;
            }
            UE_LOGI("sat_console: HOST a terminal discarded with its world");
            DiscardAt(i);
        }
    }
    if (s->connected()) {
        if (s->role() == coop::net::Role::Host) {
            DrainHostWork();
            HostTellConsoles();
        }
        FollowShown();
    }
    if (!g_asm.Idle() && now >= g_nextSweep) {
        g_nextSweep = now + std::chrono::seconds(1);
        g_asm.Sweep(now, std::chrono::seconds(10));
    }
}

void OnChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    if (!GT::IsGameThread()) {
        if (!g_saidOffThread) {
            g_saidOffThread = true;
            UE_LOGE("sat_console: a chunk arrived off the game thread -- dropped");
        }
        return;
    }
    if (senderSlot >= kMaxPeers) return;
    std::vector<uint8_t> blob;
    if (!g_asm.OnChunk(p, senderSlot, blob)) return;
    if (SessionIf(coop::net::Role::Host)) {
        if (senderSlot != 0) HostOnBlob(senderSlot, blob);
    } else if (SessionIf(coop::net::Role::Client)) {
        if (senderSlot == 0) ClientOnBlob(blob);
    }
}

void OnPeerWorldReady(int slot) {
    if (!SessionIf(coop::net::Role::Host) || slot < 1 || slot >= kMaxPeers) return;
    // Every console's lines and level, so a joiner's consoles show what they ran before it came.
    for (Terminal& t : g_terms) {
        SendShow(static_cast<uint8_t>(slot), t.panel, t.logSent);
        SendBusy(static_cast<uint8_t>(slot), t.panel, t.busySent);
        ++g_counts.rebinds;
        if (t.busySent) ++g_counts.busyRebinds;
    }
    if (!g_terms.empty())
        UE_LOGI("sat_console: HOST slot %d's world is ready -- %zu console(s) sent to it", slot, g_terms.size());
}

void OnPeerGone(uint8_t slot) {
    if (slot >= kMaxPeers) return;
    g_asm.ClearSlot(slot);
    g_out[slot] = Outbox{};
    g_typists[slot] = Typist{};
    g_saidRefused[slot] = g_saidMalformed[slot] = g_saidNoTerminal[slot] = g_saidNoContext[slot] = false;
}

void OnDisconnect() {
    // A busy flag this terminal holds only as the shown console's goes with the session.
    if (ShownBusy() && !g_localHolds)
        if (void* term = SC::LocalTerminal()) SC::WriteProcessing(term, false);
    g_localHolds = false;
    g_hostLines.clear();
    g_rootLines.clear();
    g_alertDepth = 0;
    while (!g_terms.empty()) DiscardAt(g_terms.size() - 1);
    g_screens.clear();
    g_history.clear();
    g_shown.clear();
    g_shownPanel = nullptr;
    g_haveShown = false;
    for (auto& o : g_out) o = Outbox{};
    for (auto& t : g_typists) t = Typist{};
    for (bool& b : g_saidRefused) b = false;
    for (bool& b : g_saidMalformed) b = false;
    for (bool& b : g_saidNoTerminal) b = false;
    for (bool& b : g_saidNoContext) b = false;
    g_asm.Clear();
    if (g_counts.linesSent || g_counts.linesRun)
        UE_LOGI("sat_console: session end -- %llu line(s) sent to the host, %llu run for clients",
                static_cast<unsigned long long>(g_counts.linesSent),
                static_cast<unsigned long long>(g_counts.linesRun));
    g_counts = Counts{};
}

Counts LaneCounts() {
    return g_counts;
}

size_t KeptTerminals(void** out, size_t cap) {
    size_t n = 0;
    for (const Terminal& t : g_terms)
        if (n < cap) out[n++] = t.pin.Raw();
    return n;
}

}  // namespace coop::sat_console_sync
