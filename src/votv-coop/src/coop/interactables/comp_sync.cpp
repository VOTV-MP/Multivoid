// coop/comp_sync.cpp -- see coop/comp_sync.h.

#include "coop/interactables/comp_sync.h"

#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/interactables/signal_wire.h"

#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/desk/signal_dynamic.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <vector>

namespace coop::comp_sync {
namespace {

namespace CD = ue_wrap::console_desk;
namespace SD = ue_wrap::signal_dynamic;
using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr auto kPollInterval = std::chrono::milliseconds(1000);
constexpr auto kAssemblyTTL  = std::chrono::seconds(20);

Clock::time_point g_nextPoll{};
bool g_worldDown = true;     // start "down" so the first resolve runs the world-up edge
bool g_lastLocalFlag = false;

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
    const uint8_t* p = static_cast<const uint8_t*>(CD::CompDataPtr());
    if (!p) return false;
    std::memcpy(&out.namePtr, p + SD::kOff_name, sizeof(out.namePtr));
    std::memcpy(&out.idPtr, p + SD::kOff_id, sizeof(out.idPtr));
    std::memcpy(&out.level, p + SD::kOff_level, sizeof(out.level));
    std::memcpy(&out.sizeBits, p + SD::kOff_size, sizeof(out.sizeBits));
    return true;
}
DataKey g_lastDataKey;

// Mirror-side wire state (cue/paint edge tracking).
bool g_wireActive = false;
uint8_t g_simSlot = 0xFF;
int g_lastPaintedPhase = -1;  // -1 none, 0..2 level phases, 3 finished, 4 idle

coop::blob_chunks::Assembler g_assembler;
uint32_t g_nextSeq = 1;
bool g_splitBrainWarned = false;

const wchar_t* PhaseText(int phase) {
    switch (phase) {
    case 0: return L"denoising";
    case 1: return L"filtering";
    case 2: return L"conversion";
    case 3: return L"finished";
    default: return L"idle";
    }
}

int DerivePhase(bool active, float progress) {
    if (active) {
        SD::Row row;
        const void* p = CD::CompDataPtr();
        int32_t level = 0;
        if (p) std::memcpy(&level, static_cast<const uint8_t*>(p) + SD::kOff_level,
                           sizeof(level));
        return level < 0 ? 0 : (level > 2 ? 2 : level);
    }
    return progress >= 100.0f ? 3 : 4;
}

void PaintPhaseIfChanged(int phase) {
    if (phase == g_lastPaintedPhase) return;
    if (CD::PaintCompProcess(PhaseText(phase))) g_lastPaintedPhase = phase;
}

void SendState(coop::net::Session* s, const CD::CompScalars& cs, uint8_t adopt, int toSlot,
               uint8_t isFinalLevel = 0) {
    coop::net::CompStatePayload p{};
    p.decodeActive = cs.decodeActive ? 1 : 0;
    p.adopt = adopt;
    p.isFinalLevel = isFinalLevel;
    p.progress = cs.progress;
    p.downloading = cs.downloading;
    if (toSlot < 0) s->SendReliable(coop::net::ReliableKind::CompState, &p, sizeof(p));
    else s->SendReliableToSlot(toSlot, coop::net::ReliableKind::CompState, &p, sizeof(p));
}

// Audit I-1: the SIMULATOR derives "this completion hit the cap" from its own
// post-increment level + maxLevel at the falling edge -- the mirror must never
// derive it (its comp_data is one CompData behind the CompState on the lane).
uint8_t ComputeFinalLevel(const CD::CompScalars& cs) {
    if (cs.progress < 100.0f) return 0;  // manual stop, not a completion
    const void* base = CD::CompDataPtr();
    if (!base) return 0;
    int32_t level = 0;
    std::memcpy(&level, static_cast<const uint8_t*>(base) + SD::kOff_level, sizeof(level));
    CD::Scalars desk;
    if (!CD::ReadScalars(desk)) return 0;
    return level >= desk.compMaxLevel + 1 ? 1 : 0;
}

bool SendData(coop::net::Session* s, bool adopt) {
    const void* base = CD::CompDataPtr();
    if (!base) return false;
    SD::Row row;
    if (!SD::ReadStruct(base, row)) return false;
    const std::vector<uint8_t> blob = coop::signal_wire::Serialize(row, adopt);
    return coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::CompData,
                                       g_nextSeq++, blob);
}

void ApplyData(const std::vector<uint8_t>& blob, uint8_t senderSlot) {
    SD::Row row;
    bool adopt = false;
    if (!coop::signal_wire::Deserialize(blob, row, adopt)) {
        UE_LOGW("comp_sync: malformed comp_data blob from slot %u -- dropped",
                static_cast<unsigned>(senderSlot));
        return;
    }
    if (adopt && senderSlot != 0) return;  // adopt is host-only
    void* base = CD::CompDataPtr();
    if (!base) return;
    if (!row.hasData) {
        // Eject: the owner zeroed the struct natively; mirror that shape.
        SD::Row empty;
        SD::WriteStructLive(base, empty);
    } else if (!SD::WriteStructLive(base, row)) {
        UE_LOGW("comp_sync: comp_data apply failed (slot %u, '%ls')",
                static_cast<unsigned>(senderSlot), row.name.c_str());
        return;
    }
    CD::UpdComp(row.hasData);
    // Echo guard: prime the edge detector so our own poll doesn't rebroadcast.
    ReadDataKey(g_lastDataKey);
    UE_LOGI("comp_sync: comp_data applied from slot %u ('%ls' lvl %d, hasData=%d)",
            static_cast<unsigned>(senderSlot), row.name.c_str(), row.level,
            row.hasData ? 1 : 0);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!CD::EnsureResolved()) return;
    const auto now = Clock::now();
    if (now < g_nextPoll) return;
    g_nextPoll = now + kPollInterval;

    g_assembler.Sweep(now, kAssemblyTTL);

    CD::CompScalars cs;
    if (!CD::ReadCompScalars(cs)) {
        if (!g_worldDown) {
            g_worldDown = true;
            g_lastDataKey = {};
            g_lastPaintedPhase = -1;
        }
        return;
    }
    if (g_worldDown) {
        // World-up edge. CLIENT: kill the setData->comp_start auto-resume
        // (the v56 joiner double-simulation bug) BEFORE any stream goes out.
        // The host keeps its natural latch -- it owns the world's decode.
        g_worldDown = false;
        if (s->role() == coop::net::Role::Client && cs.decodeActive) {
            CD::UnlatchDecode();
            cs.decodeActive = false;
            UE_LOGI("comp_sync: client world-up unlatch (save-load auto-decode killed; "
                    "the simulator's stream mirrors it)");
        }
        g_lastLocalFlag = cs.decodeActive;
        ReadDataKey(g_lastDataKey);  // prime silently (save transfer converged it)
        return;
    }

    // Simulator stream: ~1 Hz while latched + both edges. The falling edge
    // carries the Done-vs-prog verdict (audit I-1).
    if (s->connected() && (cs.decodeActive || g_lastLocalFlag != cs.decodeActive)) {
        const bool falling = g_lastLocalFlag && !cs.decodeActive;
        SendState(s, cs, /*adopt=*/0, /*toSlot=*/-1,
                  falling ? ComputeFinalLevel(cs) : 0);
    }
    g_lastLocalFlag = cs.decodeActive;

    // comp_data change edge (upload/eject/level-up on THIS machine).
    DataKey k;
    if (ReadDataKey(k) && !(k == g_lastDataKey)) {
        g_lastDataKey = k;
        if (s->connected()) SendData(s, /*adopt=*/false);
    }
}

void OnState(const coop::net::CompStatePayload& p, uint8_t senderSlot) {
    if (senderSlot >= coop::net::kMaxPeers) return;
    if (!CD::EnsureResolved()) return;
    CD::CompScalars local;
    if (!CD::ReadCompScalars(local)) return;
    if (p.adopt && senderSlot != 0) return;  // adopt is host-only
    if (local.decodeActive) {
        // WE simulate -- never apply a foreign stream over the local sim.
        // (Claims make two simulators near-impossible; log the split-brain.)
        if (p.decodeActive && !g_splitBrainWarned) {
            g_splitBrainWarned = true;
            UE_LOGW("comp_sync: foreign CompState while local decode active "
                    "(slot %u) -- local sim wins", static_cast<unsigned>(senderSlot));
        }
        return;
    }
    CD::WriteCompScalars(p.progress, p.downloading);
    const bool active = p.decodeActive != 0;
    if (active) g_simSlot = senderSlot;
    if (!g_wireActive && active) CD::CompCueStart();
    if (g_wireActive && !active) {
        CD::CompCueStop();
        if (p.progress >= 100.0f) {
            // Completion beep: the SIMULATOR stamped the Done-vs-prog verdict
            // (audit I-1/I-5: the mirror's local level is pre-CompData stale
            // here, and a desk-read fallback guessed maxLevel=0 -- both wrong).
            CD::CompBeepDone(p.isFinalLevel != 0);
        }
        g_simSlot = 0xFF;
    }
    g_wireActive = active;
    if (active || p.progress > 0.0f) CD::PaintCompProgress(p.progress);
    PaintPhaseIfChanged(DerivePhase(active, p.progress));
}

void OnDataChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    if (senderSlot >= coop::net::kMaxPeers) return;
    std::vector<uint8_t> blob;
    if (g_assembler.OnChunk(p, senderSlot, blob))
        ApplyData(blob, senderSlot);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (!CD::EnsureResolved()) return;
    CD::CompScalars cs;
    if (!CD::ReadCompScalars(cs)) return;
    // The host's view is current even when a CLIENT simulates (its stream
    // raw-writes the host's fields) -- but the FLAG is only true when the
    // host itself simulates; pass the wire-known active state instead.
    if (!cs.decodeActive && g_wireActive) cs.decodeActive = true;
    SendState(s, cs, /*adopt=*/1, peerSlot);
    SendData(s, /*adopt=*/true);  // chunked: broadcast (idempotent echo-guarded applies)
}

void OnPeerDisconnect(uint8_t slot) {
    if (slot != g_simSlot || g_simSlot == 0xFF) return;
    // The streaming simulator left: wind the mirror down; the decode pauses
    // (manual restart from the mirrored progress is the native resume path).
    g_simSlot = 0xFF;
    if (g_wireActive) {
        g_wireActive = false;
        CD::CompCueStop();
        PaintPhaseIfChanged(4);
        UE_LOGI("comp_sync: simulator (slot %u) left mid-decode -- paused at the "
                "last mirrored progress", static_cast<unsigned>(slot));
    }
}

void OnDisconnect() {
    g_assembler.Clear();
    if (g_wireActive) {
        g_wireActive = false;
        if (CD::EnsureResolved()) {
            CD::CompCueStop();
            PaintPhaseIfChanged(4);
        }
    }
    g_simSlot = 0xFF;
    g_splitBrainWarned = false;
    g_nextSeq = 1;
    g_nextPoll = {};
    // g_worldDown/g_lastLocalFlag/g_lastDataKey persist -- they track the
    // LOCAL world, which outlives the session.
}

}  // namespace coop::comp_sync
