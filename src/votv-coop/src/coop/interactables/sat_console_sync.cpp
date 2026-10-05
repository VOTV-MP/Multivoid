// coop/interactables/sat_console_sync.cpp -- see coop/interactables/sat_console_sync.h.

#include "coop/interactables/sat_console_sync.h"

#include "coop/interactables/sat_console_table.h"
#include "coop/items/save_record_wire.h"  // the blob's read and append primitives
#include "coop/net/blob_chunks.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/session/player_handshake.h"  // GuidForSlot: a typist's durable identity

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/gc_pin.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/console_desk.h"  // Instance: the desk a host terminal names as its panel
#include "ue_wrap/desk/dish.h"          // IndexOf, DishByIndex, Count
#include "ue_wrap/desk/sat_console.h"
#include "ue_wrap/engine/world_identity.h"

#include <atomic>
#include <chrono>
#include <deque>
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

// The blobs. A dish is named by its gamemode.dishs index, a panel by its placed actor's name.
constexpr uint8_t kOpLine = 0;  // typist -> host: [i32 dish][str name][str used panel][str line]
constexpr uint8_t kOpLog  = 1;  // host -> typist: [u8 bracketsType][str text]
constexpr uint8_t kOpBusy = 2;  // host -> typist: [u8 busy], the level, on each change and as a seed

constexpr int32_t kNoDish = -1;  // init with no dish: the ROOT console

constexpr size_t kMaxLineChars   = 255;   // MTA's clamp too; every command name is short
constexpr size_t kMaxNameChars   = 64;
constexpr size_t kMaxLogChars    = 8192;  // `debug rules` prints the game rules as JSON, a few thousand
constexpr int    kLinesPerWindow = 4;     // a person types slower: lines a typist may run per window
constexpr auto   kWindow         = std::chrono::seconds(1);
constexpr auto   kSpawnEvery     = std::chrono::seconds(2);  // a typist's spawn-on-every-call commands
constexpr size_t kMaxTerminals   = 16;    // kept terminals, by identity, while the session lasts
constexpr size_t kOutboxCap      = 512;   // blobs owed to one slot while its sends are refused
constexpr auto   kResendEvery    = std::chrono::milliseconds(250);  // a refused slot is tried this often
const wchar_t* const kBusyLine = L"<r>The terminal is busy</>";  // the game's answer to a line it cannot take
const wchar_t* const kErrLine  = L"<r>err</>";                   // the game's answer to a line it cannot run

constexpr int kTagEnter = 0x53434E45;  // 'SCNE'
constexpr int kTagWrite = 0x53434E57;  // 'SCNW'
constexpr int kTagGraph = 0x53434E47;  // 'SCNG'

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched = false;
coop::blob_chunks::Assembler g_asm;
uint32_t g_seq = 1;
Clock::time_point g_nextSweep{};
Counts g_counts{};
bool g_saidOffThread = false;

// Blobs owed to a slot whose send was refused. A blob goes whole or not at all, and one refused waits
// at the head, so a slot's blobs arrive in the order they were made. The host keeps one per typist's
// slot; a client keeps slot 0's, to the host. At its cap an outbox gives up its oldest blob that is not a
// busy level, which the typist's terminal needs to stop being busy.
struct Outbox {
    std::deque<std::vector<uint8_t>> q;
    Clock::time_point nextTry{};
    bool saidFull = false;
};
Outbox g_out[kMaxPeers];

// HOST: the terminal each typist runs on, keyed by its durable identity, not its slot: a typist that
// leaves mid-command leaves its terminal running that command, as the game's terminal runs on when its
// player walks away, and a typist that comes back finds it again. A terminal lasts while the session
// does and its world stays current; the last client's leave ends the session, and each terminal is
// then discarded, its command stopping where it stands.
struct Terminal {
    std::string guid;
    ue_wrap::GcPin pin;
    void*   world = nullptr;  // the world it was made in, when known
    int     slot = -1;        // its typist's slot while connected, -1 once it left
    bool    busySent = false; // the busy level its typist last got
    bool    haveContext = false;
    int32_t dish = kNoDish;
    std::wstring name;
    int     lines = 0;        // lines run in the current window
    Clock::time_point windowStart{};
    Clock::time_point nextSpawnOk{};
};
std::vector<Terminal> g_terms;
bool g_saidRefused[kMaxPeers] = {};
bool g_saidMalformed[kMaxPeers] = {};
bool g_saidNoTerminal[kMaxPeers] = {};
bool g_saidNoContext[kMaxPeers] = {};

// CLIENT: the host's busy level for this terminal, and whether a line this terminal ran itself holds
// the flag: the mirror sets it while the host's terminal is busy and clears only what it set.
bool g_mirrorBusy = false;
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

void SendBusy(uint8_t slot, bool busy) {
    std::vector<uint8_t> b;
    W::AppU8(b, kOpBusy);
    W::AppU8(b, busy ? 1 : 0);
    Enqueue(slot, std::move(b));
}

// A line the host will not run is answered as the game answers one it cannot take or run: its echo,
// then the reason's line. The typist already consumed it.
void AnswerRefused(uint8_t slot, const std::wstring& line, const wchar_t* why) {
    SendLog(slot, line, 1);
    SendLog(slot, why, 0);
}

// ---- the typist ----

// A line whose command rests on the shared world goes to the host, with the terminal's context as it
// stands: the dish its panel pointed it at (or ROOT), its name, and the panel last used. A busy
// terminal answers the line itself, as it would alone; so does a line the terminal owns. The body is
// refused, which leaves its own tail unrun, so the input is consumed here as that tail does
// (ui_console.enterCommand: `command = ""`, the text box emptied); the echo and every answer come back
// from the host.
sg::Verdict OnEnterPre(const sg::Call& c) {
    if (!SessionIf(coop::net::Role::Client)) return sg::Verdict::Run;
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
    std::vector<uint8_t> b;
    W::AppU8(b, kOpLine);
    W::AppI32(b, dishIdx < 0 ? kNoDish : dishIdx);
    W::AppWStr(b, name);
    W::AppWStr(b, used);
    W::AppWStr(b, line);
    Enqueue(0, std::move(b));
    SC::ConsumeInput(term);
    ++g_counts.linesSent;
    UE_LOGI("sat_console: CLIENT '%ls' runs on the host's terminal (dish %d)", line.c_str(), dishIdx);
    return sg::Verdict::Cancel;
}

// After this terminal ran a body of its own. While the host's terminal is busy the flag is the mirror's:
// a line this terminal ran itself may have cleared it, and it is set again, the line's hold over. Else
// the flag is the line's own.
void ClientAfterLocalBody(void* term) {
    bool busy = false;
    if (!SC::ReadProcessing(term, busy)) return;
    if (g_mirrorBusy) {
        if (!busy) {
            g_localHolds = false;
            SC::WriteProcessing(term, true);
        }
    } else {
        g_localHolds = busy;
    }
}

void ClientOnBlob(const std::vector<uint8_t>& blob) {
    size_t o = 0;
    uint8_t op = 0;
    if (!W::RdU8(blob, o, op)) return;
    void* term = SC::LocalTerminal();
    if (op == kOpLog) {
        uint8_t type = 0;
        std::wstring text;
        if (!W::RdU8(blob, o, type) || !W::RdWStr(blob, o, text) || text.size() > kMaxLogChars) {
            UE_LOGW("sat_console: a malformed line from the host -- dropped");
            return;
        }
        if (!term || !SC::CallWriteToLog(term, text, type))
            UE_LOGW("sat_console: a line from the host found no terminal here -- dropped");
        return;
    }
    if (op == kOpBusy) {
        uint8_t busy = 0;
        if (!W::RdU8(blob, o, busy)) return;
        g_mirrorBusy = busy != 0;
        if (term && (g_mirrorBusy || !g_localHolds)) SC::WriteProcessing(term, g_mirrorBusy);
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

Terminal* ByGuid(const std::string& guid) {
    for (Terminal& t : g_terms)
        if (t.guid == guid) return &t;
    return nullptr;
}

// Unpin a terminal and stop what it runs; its typist, if still here, is told it is no longer busy.
void Discard(Terminal& t) {
    void* term = t.pin.Raw();
    if (t.slot >= 0 && t.busySent) SendBusy(static_cast<uint8_t>(t.slot), false);
    t.pin.Release();
    SC::DiscardTerminal(term);
}

void DiscardAt(size_t i) {
    Discard(g_terms[i]);
    g_terms.erase(g_terms.begin() + static_cast<std::ptrdiff_t>(i));
}

// A typist back at the terminal it left: bound to it again and told its busy level, so its own terminal
// is busy while the command it left runs on, and gets that command's lines from here on.
void Rebind(Terminal& t, uint8_t slot) {
    t.slot = slot;
    bool busy = false;
    SC::ReadProcessing(t.pin.Raw(), busy);
    t.busySent = busy;
    SendBusy(slot, busy);
    ++g_counts.rebinds;
    if (busy) ++g_counts.busyRebinds;
    UE_LOGI("sat_console: HOST slot %u is back at its terminal (busy=%d)", static_cast<unsigned>(slot), busy ? 1 : 0);
}

// The typist's terminal, made on its first line. At the cap, a departed typist's idle terminal gives way;
// with none, the line is refused.
Terminal* TerminalFor(uint8_t slot, const std::string& guid) {
    if (Terminal* t = ByGuid(guid)) {
        if (t->slot != slot) Rebind(*t, slot);
        return t;
    }
    if (g_terms.size() >= kMaxTerminals) {
        for (size_t i = 0; i < g_terms.size(); ++i) {
            bool busy = true;
            if (g_terms[i].slot < 0 && SC::ReadProcessing(g_terms[i].pin.Raw(), busy) && !busy) {
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
    made.guid = guid;
    made.world = WI::CurrentWorld();  // null while unknown: the world term then fails open
    made.slot = slot;
    g_terms.push_back(std::move(made));
    UE_LOGI("sat_console: HOST made slot %u's terminal (%zu kept)", static_cast<unsigned>(slot), g_terms.size());
    return &g_terms.back();
}

// Whether a terminal stands in the context a line names. A dish index that does not resolve is not
// ROOT: the line waits for its dish rather than running against none, and nothing is kept, so the
// next line with the same index tries again. An index past the host's dishes is no dish at all.
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
    const std::string& guid = coop::player_handshake::GuidForSlot(slot);
    Terminal* t = guid.empty() ? nullptr : TerminalFor(slot, guid);
    if (!t) {
        if (!g_saidNoTerminal[slot]) {
            g_saidNoTerminal[slot] = true;
            UE_LOGW("sat_console: HOST no terminal for slot %u (%s) -- its lines are answered err",
                    static_cast<unsigned>(slot), guid.empty() ? "its identity not in yet" : "none could be made");
        }
        AnswerRefused(slot, line, kErrLine);
        return;
    }
    const auto now = Clock::now();
    if (now - t->windowStart >= kWindow) {
        t->windowStart = now;
        t->lines = 0;
    }
    const bool spawns = T::SpawnsEveryCall(line);
    if (++t->lines > kLinesPerWindow || (spawns && now < t->nextSpawnOk)) {
        AnswerRefused(slot, line, kBusyLine);  // the terminal cannot take the line, as the game says it
        return;
    }
    // A terminal still running a command keeps its context: its latent continuation reads the dish,
    // name and panel it started with. The game's own terminal refuses a line while busy, so does this.
    bool busy = true;
    if (!SC::ReadProcessing(t->pin.Raw(), busy) || busy) {
        AnswerRefused(slot, line, busy ? kBusyLine : kErrLine);
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
    // The panel the typist last used: none is a real answer (ROOT), a name that does not resolve here
    // is not, and the line does not run against no panel in its place.
    void* panel = used.empty() ? nullptr : SC::FindPanelByName(used);
    if ((!used.empty() && !panel) || !SC::WriteUsed(t->pin.Raw(), panel)) {
        if (!g_saidNoContext[slot]) {
            g_saidNoContext[slot] = true;
            UE_LOGW("sat_console: HOST no panel '%ls' for slot %u's line -- answered err",
                    Printable(used).c_str(), static_cast<unsigned>(slot));
        }
        AnswerRefused(slot, line, kErrLine);
        return;
    }
    if (spawns) t->nextSpawnOk = now + kSpawnEvery;
    UE_LOGI("sat_console: HOST runs '%ls' for slot %u (dish %d)", Printable(line).c_str(),
            static_cast<unsigned>(slot), dish);
    if (!SC::RunCommand(t->pin.Raw(), line)) {
        AnswerRefused(slot, line, kErrLine);
        return;
    }
    ++g_counts.linesRun;
}

// Every write of a terminal's busy flag lies in its enterCommand or its ubergraph, the latent
// continuations included, so the level is read after each and sent when it moved.
void HostAfterBody(Terminal& t) {
    bool busy = false;
    if (t.slot < 0 || !SC::ReadProcessing(t.pin.Raw(), busy) || busy == t.busySent) return;
    t.busySent = busy;
    SendBusy(static_cast<uint8_t>(t.slot), busy);
}

sg::Verdict OnWritePre(const sg::Call& c) {
    if (!SessionIf(coop::net::Role::Host)) return sg::Verdict::Run;
    Terminal* t = ByObject(c.object);
    if (!t || t->slot < 0) return sg::Verdict::Run;  // the host's own terminal, or no typist to show it to
    std::wstring text;
    uint8_t type = 0;
    if (!SC::ReadWriteToLogArgs(c.locals, text, type)) return sg::Verdict::Run;
    if (text.size() > kMaxLogChars) text.resize(kMaxLogChars);
    SendLog(static_cast<uint8_t>(t->slot), text, type);
    ++g_counts.linesReturned;
    return sg::Verdict::Run;
}

void OnBodyPost(const sg::Call& c) {
    if (SessionIf(coop::net::Role::Host)) {
        if (Terminal* t = ByObject(c.object)) HostAfterBody(*t);
    } else if (SessionIf(coop::net::Role::Client)) {
        if (c.object == SC::LocalTerminal()) ClientAfterLocalBody(c.object);
    }
}

// The three watches stand or fall together: a line refused on a client whose answers no watch would send
// back would vanish.
void EnsureWatched() {
    if (g_watched) return;
    g_watched = true;
    const bool enter = sg::WatchClassName(SC::kTerminalClass, SC::kEnterCommand, kTagEnter, OnEnterPre, OnBodyPost);
    const bool graph = sg::WatchClassName(SC::kTerminalClass, SC::kUbergraph, kTagGraph, nullptr, OnBodyPost);
    const bool write = sg::WatchClassName(SC::kTerminalClass, SC::kWriteToLog, kTagWrite, OnWritePre, nullptr);
    if (enter && graph && write) return;
    if (enter) sg::UnwatchClassName(SC::kTerminalClass, SC::kEnterCommand, kTagEnter, OnEnterPre, OnBodyPost);
    if (graph) sg::UnwatchClassName(SC::kTerminalClass, SC::kUbergraph, kTagGraph, nullptr, OnBodyPost);
    if (write) sg::UnwatchClassName(SC::kTerminalClass, SC::kWriteToLog, kTagWrite, OnWritePre, nullptr);
    UE_LOGE("sat_console: a terminal watch did not register -- none kept, the SAT console's lines run where typed");
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
    const std::string& guid = coop::player_handshake::GuidForSlot(slot);
    if (guid.empty()) return;
    if (Terminal* t = ByGuid(guid))
        if (t->slot != slot) Rebind(*t, static_cast<uint8_t>(slot));
}

void OnPeerGone(uint8_t slot) {
    if (slot >= kMaxPeers) return;
    g_asm.ClearSlot(slot);
    g_out[slot] = Outbox{};
    g_saidRefused[slot] = g_saidMalformed[slot] = g_saidNoTerminal[slot] = g_saidNoContext[slot] = false;
    for (Terminal& t : g_terms) {
        if (t.slot != slot) continue;
        t.slot = -1;  // its command, if any, runs to its end; the terminal waits for its typist
        UE_LOGI("sat_console: HOST slot %u left -- its terminal kept", static_cast<unsigned>(slot));
    }
}

void OnDisconnect() {
    // A busy flag this terminal holds only as the host's mirror goes with the session.
    if (g_mirrorBusy && !g_localHolds)
        if (void* term = SC::LocalTerminal()) SC::WriteProcessing(term, false);
    g_mirrorBusy = g_localHolds = false;
    while (!g_terms.empty()) DiscardAt(g_terms.size() - 1);
    for (auto& o : g_out) o = Outbox{};
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
