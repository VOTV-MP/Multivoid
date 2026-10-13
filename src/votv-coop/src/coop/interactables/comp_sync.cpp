// coop/interactables/comp_sync.cpp -- see coop/interactables/comp_sync.h.

#include "coop/interactables/comp_sync.h"

#include "coop/interactables/desk_verb_effects.h"  // the replay's presser; a client's decode's gloss and stat
#include "coop/interactables/signal_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/comp_pane.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/signal_dynamic.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/profile.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <vector>

namespace coop::comp_sync {
namespace {

namespace CD = ue_wrap::console_desk;
namespace CMP = ue_wrap::comp_pane;
namespace SD = ue_wrap::signal_dynamic;
namespace sg = ue_wrap::script_gate;
using Clock = std::chrono::steady_clock;
using coop::net::Role;

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr auto kPollInterval = std::chrono::milliseconds(1000);
constexpr auto kAssemblyTTL  = std::chrono::seconds(20);

Clock::time_point g_nextPoll{};
bool g_worldDown = true;     // start "down" so the first resolve runs the world-up edge
bool g_lastLocalFlag = false;

struct Counts {
    unsigned long long startsRefused = 0;   // CLIENT: comp_starts refused at the gate
    unsigned long long clientDecodes = 0;   // HOST: decodes a client's start began
    unsigned long long completions = 0;     // HOST: completions told to the mirrors
    unsigned long long processedSent = 0;   // HOST: signals_processed points put back and sent to their owner
};
Counts g_counts;

coop::net::Session* SessionAs(Role role) {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->role() == role) ? s : nullptr;
}

// comp_data_0 instance key (raw bytes; the email RowKey doctrine) for the
// change-edge detector.
struct DataKey {
    uint64_t namePtr = 0, idPtr = 0;
    int32_t level = 0;
    uint32_t sizeBits = 0;
    bool operator==(const DataKey& o) const {
        return namePtr == o.namePtr && idPtr == o.idPtr &&
               level == o.level && sizeBits == o.sizeBits;
    }
};
bool ReadDataKey(DataKey& out) {
    const uint8_t* p = static_cast<const uint8_t*>(CMP::CompDataPtr());
    if (!p) return false;
    std::memcpy(&out.namePtr, p + SD::kOff_name, sizeof(out.namePtr));
    std::memcpy(&out.idPtr, p + SD::kOff_id, sizeof(out.idPtr));
    std::memcpy(&out.level, p + SD::kOff_level, sizeof(out.level));
    std::memcpy(&out.sizeBits, p + SD::kOff_size, sizeof(out.sizeBits));
    return true;
}
DataKey g_lastDataKey;

bool ReadLevel(void* desk, int32_t& out) {
    const uint8_t* p = static_cast<const uint8_t*>(CMP::CompDataPtr(desk));
    if (!p) return false;
    std::memcpy(&out, p + SD::kOff_level, sizeof(out));
    return true;
}

// Mirror-side wire state (cue/paint edge tracking).
bool g_wireActive = false;
float g_wireProgress = 0.0f;
int g_lastPaintedPhase = -1;  // -1 none, 0..2 level phases, 3 finished, 4 idle

coop::blob_chunks::Assembler g_assembler;
uint32_t g_nextSeq = 1;

// comp_start's phase text by the row's level (analogDScreenTest.cpp :9787-9816), then the completion's and the stop's.
const wchar_t* PhaseText(int phase) {
    switch (phase) {
    case 0: return L"conversion";
    case 1: return L"filtering";
    case 2: return L"denoising";
    case 3: return L"finished";
    default: return L"idle";
    }
}

int DerivePhase(bool active, float progress) {
    if (active) {
        int32_t level = 0;
        ReadLevel(CD::Instance(), level);
        return level < 0 ? 0 : (level > 2 ? 2 : level);
    }
    return progress >= 100.0f ? 3 : 4;
}

void PaintPhaseIfChanged(int phase) {
    if (phase == g_lastPaintedPhase) return;
    if (CMP::PaintCompProcess(PhaseText(phase))) g_lastPaintedPhase = phase;
}

void SendState(coop::net::Session* s, const CMP::CompScalars& cs, bool completed, bool finalLevel, int toSlot) {
    coop::net::CompStatePayload p{};
    p.decodeActive = cs.decodeActive ? 1 : 0;
    p.completed = completed ? 1 : 0;
    p.isFinalLevel = finalLevel ? 1 : 0;
    p.progress = cs.progress;
    p.downloading = cs.downloading;
    if (toSlot < 0) s->SendReliable(coop::net::ReliableKind::CompState, &p, sizeof(p));
    else s->SendReliableToSlot(toSlot, coop::net::ReliableKind::CompState, &p, sizeof(p));
}

// The refiner's row, to every client, or to `toSlot` alone (a joiner's seed).
bool SendData(coop::net::Session* s, int toSlot = -1) {
    const void* base = CMP::CompDataPtr();
    if (!base) return false;
    SD::Row row;
    if (!SD::ReadStruct(base, row)) return false;
    const std::vector<uint8_t> blob = coop::signal_wire::Serialize(row);
    return toSlot < 0
               ? coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::CompData, g_nextSeq++, blob)
               : coop::blob_chunks::SendBlobToSlot(s, toSlot, coop::net::ReliableKind::CompData, g_nextSeq++, blob);
}

void ApplyData(const std::vector<uint8_t>& blob) {
    SD::Row row;
    if (!coop::signal_wire::Deserialize(blob, row)) {
        UE_LOGW("comp_sync: malformed comp_data blob from the host -- dropped");
        return;
    }
    void* base = CMP::CompDataPtr();
    if (!base) return;
    if (!row.hasData) {
        // Eject: the host zeroed the struct natively; mirror that shape.
        SD::Row empty;
        SD::WriteStructLive(base, empty);
    } else if (!SD::WriteStructLive(base, row)) {
        UE_LOGW("comp_sync: comp_data apply failed ('%ls')", row.name.c_str());
        return;
    }
    CMP::UpdComp(row.hasData);
    // updComp paints the process text "idle" (analogDScreenTest.cpp :10365): the phase the wire says goes back on,
    // a continued decode's next level at once.
    g_lastPaintedPhase = 4;
    PaintPhaseIfChanged(DerivePhase(g_wireActive, g_wireProgress));
    UE_LOGI("comp_sync: comp_data applied from the host ('%ls' lvl %d, hasData=%d)", row.name.c_str(), row.level,
            row.hasData ? 1 : 0);
}

// ---- the decode's owner (HOST) -------------------------------------------------------------------------------------

// The slot whose start latched the host's machine, 0 for the host, and the press that began it: set at the post of a
// comp_start that found the machine unlatched, outside calculate_comp's body (a start there is the completion's
// continue, the same decode going on), and left it latched. The replay's scope is still open at that post.
uint8_t  g_owner = 0;
uint32_t g_ownerSeq = 0;
void*    g_calcFn = nullptr;  // the desk's calculate_comp, as its own pre names it

// A start's pre, read at its post: a latch found there then is a decode the start began.
struct StartPre {
    bool valid = false;
    bool begins = false;
};
StartPre g_startPre;

void SetOwner(uint8_t slot, uint32_t seq) {
    if (slot != 0) ++g_counts.clientDecodes;
    if (slot != g_owner || slot != 0)
        UE_LOGI("comp_sync: HOST's refiner decode began as slot %u's (press #%u)", static_cast<unsigned>(slot), seq);
    g_owner = slot;
    g_ownerSeq = seq;
    coop::desk_verb_effects::ForwardGlossesInBody(g_calcFn, slot == 0 ? 0xFF : slot, seq);
}

void HostStartPre(void* desk) {
    g_startPre = StartPre{};
    g_startPre.valid = true;
    if (g_calcFn && sg::IsBodyActive(g_calcFn)) return;  // the completion's continue: the decode stays its owner's
    CMP::CompScalars cs;
    g_startPre.begins = CMP::ReadCompScalars(desk, cs) && !cs.decodeActive;  // it refuses while decoding
}

void OnStartPost(const sg::Call& c) {
    const StartPre pre = g_startPre;
    g_startPre = StartPre{};
    if (!pre.valid || !pre.begins || !SessionAs(Role::Host)) return;
    CMP::CompScalars cs;
    if (!CMP::ReadCompScalars(c.object, cs) || !cs.decodeActive) return;  // it refused: nothing began
    const uint8_t presser = coop::desk_verb_effects::ReplaySlot();
    if (presser == 0xFF) SetOwner(0, 0);  // the host's own press, or its load's restore
    else SetOwner(presser, coop::desk_verb_effects::ReplaySeq());
}

// ---- one step of the host's decode ---------------------------------------------------------------------------------

// Read at calculate_comp's pre while the host's machine decodes, compared at its post. Inside the body the row's
// level rises only by a completion (the upload and the eject refuse while decoding, comp_uploadData :9833), and
// the level-3 completion adds a point to the running machine's signals_processed (@71094).
struct Step {
    bool    latched = false;
    int32_t level = 0;
    bool    haveProcessed = false;  // a client owns the decode
    int32_t processed = 0;
};
Step g_step;
bool g_completed = false;       // a completion since the last state sent
bool g_completedFinal = false;  // it reached the cap

// ---- the watches ---------------------------------------------------------------------------------------------------

sg::Verdict OnStartPre(const sg::Call& c) {
    if (SessionAs(Role::Host)) {
        HostStartPre(c.object);
        return sg::Verdict::Run;
    }
    if (!SessionAs(Role::Client)) return sg::Verdict::Run;
    // A client's machine never latches: the host's decode is the one, mirrored here.
    const bool wrote = CMP::WriteStartFailed(c);
    float from = -1.0f;
    CMP::ReadStartFrom(c, from);
    if (++g_counts.startsRefused <= 3)
        UE_LOGI("comp_sync: CLIENT refused comp_start from %.3f%%%s -- the host's refiner decodes, this one mirrors "
                "it", from, wrote ? "" : " (its succ unwritten)");
    return sg::Verdict::Cancel;
}

sg::Verdict OnCalcPre(const sg::Call& c) {
    if (c.function != g_calcFn) {
        g_calcFn = c.function;
        if (g_owner != 0) coop::desk_verb_effects::ForwardGlossesInBody(g_calcFn, g_owner, g_ownerSeq);
    }
    g_step = Step{};
    if (!SessionAs(Role::Host)) return sg::Verdict::Run;
    CMP::CompScalars cs;
    if (!CMP::ReadCompScalars(c.object, cs) || !cs.decodeActive || !ReadLevel(c.object, g_step.level))
        return sg::Verdict::Run;
    g_step.latched = true;
    if (g_owner != 0) g_step.haveProcessed = ue_wrap::profile::ReadSignalsProcessed(g_step.processed);
    return sg::Verdict::Run;
}

void OnCalcPost(const sg::Call& c) {
    if (!g_step.latched) return;
    const Step step = g_step;
    g_step = Step{};
    int32_t level = 0;
    if (ReadLevel(c.object, level) && level != step.level) {
        CD::Scalars desk;
        g_completed = true;
        g_completedFinal = CD::ReadScalars(desk) && level >= desk.compMaxLevel + 1;  // the BP's done-versus-prog
    }
    int32_t now = 0;
    if (!step.haveProcessed || !ue_wrap::profile::ReadSignalsProcessed(now) || now <= step.processed) return;
    const int32_t delta = now - step.processed;
    if (!ue_wrap::profile::AddStat(L"signals_processed", -delta)) {
        UE_LOGW("comp_sync: HOST could not put back slot %u's %d signals processed -- they stay on this profile",
                static_cast<unsigned>(g_owner), delta);
        return;
    }
    coop::desk_verb_effects::SendStat(g_owner, g_ownerSeq, L"signals_processed", delta);
    ++g_counts.processedSent;
}

UE_FACT_FN(kRowCompStart, "comp_sync", L"analogDScreenTest_C", L"comp_start", Declared);
UE_FACT_FN(kRowCalculateComp, "comp_sync", L"analogDScreenTest_C", L"calculate_comp", Declared);

struct CompWatch {
    const ue_wrap::facts::Function* row;
    int            tag;
    sg::PreFn      pre;
    sg::PostFn     post;
    const char*    loss;
    bool           registered;
    bool           live;
    bool           dead;
};
CompWatch g_watches[] = {
    {&kRowCompStart, 0x434D5030, &OnStartPre, &OnStartPost,
     "a client's refiner latches on its restore and decodes beside the host's, and a client's decode is the host's",
     false, false, false},  // 'CMP0'
    {&kRowCalculateComp, 0x434D5031, &OnCalcPre, &OnCalcPost,
     "a completion reaches the mirrors only as an edge, and a client's gloss and processed signal stay on the "
     "host's profile", false, false, false},  // 'CMP1'
};
bool g_settled = false;  // every watch live or dead: the attempts end

void Register() {
    if (g_settled) return;
    for (CompWatch& w : g_watches) {
        if (w.registered || w.dead) continue;
        w.registered = sg::WatchName(*w.row, w.tag, w.pre, w.post);
        if (!w.registered) {
            w.dead = true;
            UE_LOGE("comp_sync: the gate took no watch on %ls::%ls -- %s", w.row->Class(), w.row->Name(), w.loss);
        }
    }
    sg::ResolvePendingNames();
    bool settled = true;
    for (CompWatch& w : g_watches) {
        if (w.dead || w.live) continue;
        if (sg::NameWatchLive(*w.row, w.tag)) {
            w.live = true;
        } else if (sg::NameWatchSettled(*w.row, w.tag)) {
            w.dead = true;
            UE_LOGE("comp_sync: the watch on %ls::%ls settled dead -- %s", w.row->Class(), w.row->Name(), w.loss);
        } else {
            settled = false;
        }
    }
    if (!settled) return;
    g_settled = true;
    UE_LOGI("comp_sync: the refiner's watches settled (start %d, decode step %d live)", g_watches[0].live ? 1 : 0,
            g_watches[1].live ? 1 : 0);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    Register();
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!CD::EnsureResolved()) return;
    const auto now = Clock::now();
    if (now < g_nextPoll) return;
    g_nextPoll = now + kPollInterval;

    g_assembler.Sweep(now, kAssemblyTTL);

    CMP::CompScalars cs;
    if (!CMP::ReadCompScalars(cs)) {
        if (!g_worldDown) {
            // The world's cue and texts went with it: the mirror's edge trackers start over.
            g_worldDown = true;
            g_lastDataKey = {};
            g_wireActive = false;
            g_lastPaintedPhase = -1;
        }
        return;
    }
    if (g_worldDown) {
        g_worldDown = false;
        // A client's refiner latched before its session met this world -- one it loaded itself, which a direct
        // join keeps (session_runtime's client start) -- would decode beside the host's: the gate refuses a latch
        // only inside a session.
        if (s->role() == Role::Client && cs.decodeActive && CMP::UnlatchDecode()) {
            cs.decodeActive = false;
            UE_LOGI("comp_sync: CLIENT's refiner held a latch from before its session -- cleared; the host's decode "
                    "is mirrored");
        }
        g_lastLocalFlag = cs.decodeActive;
        ReadDataKey(g_lastDataKey);  // prime silently: a joiner is seeded at its world's readiness
        return;
    }
    if (s->role() != Role::Host) return;

    // The host's stream: ~1 Hz while latched, both edges, and a completion, which a continue can hide from the edges.
    const bool completed = g_completed;
    const bool completedFinal = g_completedFinal;
    g_completed = g_completedFinal = false;
    if (s->connected() && (cs.decodeActive || g_lastLocalFlag != cs.decodeActive || completed)) {
        SendState(s, cs, completed, completedFinal, /*toSlot=*/-1);
        if (completed) ++g_counts.completions;
    }
    g_lastLocalFlag = cs.decodeActive;

    // comp_data change edge (an upload, an eject, a level-up on the host's machine).
    DataKey k;
    if (ReadDataKey(k) && !(k == g_lastDataKey)) {
        g_lastDataKey = k;
        if (s->connected()) SendData(s);
    }
}

void OnState(const coop::net::CompStatePayload& p) {
    if (!CD::EnsureResolved() || !CMP::WriteCompScalars(p.progress, p.downloading)) return;
    const bool active = p.decodeActive != 0;
    g_wireProgress = p.progress;
    if (!g_wireActive && active) CMP::CompCueStart();
    if (g_wireActive && !active) CMP::CompCueStop();
    if (p.completed) CMP::CompBeepDone(p.isFinalLevel != 0);
    g_wireActive = active;
    if (active || p.progress > 0.0f) CMP::PaintCompProgress(p.progress);
    PaintPhaseIfChanged(DerivePhase(active, p.progress));
}

void OnDataChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    std::vector<uint8_t> blob;
    if (g_assembler.OnChunk(p, senderSlot, blob)) ApplyData(blob);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = SessionAs(Role::Host);
    if (!s) return;
    if (!CD::EnsureResolved()) return;
    CMP::CompScalars cs;
    if (!CMP::ReadCompScalars(cs)) return;
    SendState(s, cs, /*completed=*/false, /*finalLevel=*/false, peerSlot);
    SendData(s, peerSlot);
}

void OnPeerLeft(uint8_t slot) {
    if (slot == 0 || slot != g_owner) return;
    UE_LOGI("comp_sync: HOST's refiner decode was slot %u's -- the host's now, its owner left",
            static_cast<unsigned>(slot));
    g_owner = 0;
    g_ownerSeq = 0;
    coop::desk_verb_effects::ForwardGlossesInBody(nullptr, 0xFF, 0);
}

void OnDisconnect() {
    const Counts& c = g_counts;
    if (c.startsRefused || c.clientDecodes || c.completions || c.processedSent)
        UE_LOGI("comp_sync: session end -- starts refused=%llu, client decodes=%llu, completions=%llu, processed "
                "sent=%llu", c.startsRefused, c.clientDecodes, c.completions, c.processedSent);
    g_counts = Counts{};
    g_assembler.Clear();
    if (g_wireActive) {
        g_wireActive = false;
        if (CD::EnsureResolved()) {
            CMP::CompCueStop();
            PaintPhaseIfChanged(4);
        }
    }
    g_owner = 0;
    g_ownerSeq = 0;
    g_step = Step{};
    g_startPre = StartPre{};
    g_completed = g_completedFinal = false;
    g_wireProgress = 0.0f;
    g_nextSeq = 1;
    g_nextPoll = {};
    // The next session begins with a world-up edge: its role may differ from this one's in the same world.
    g_worldDown = true;
}

bool MirrorActive() { return g_wireActive; }

}  // namespace coop::comp_sync
