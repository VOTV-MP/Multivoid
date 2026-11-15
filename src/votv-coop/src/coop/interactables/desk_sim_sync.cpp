// coop/interactables/desk_sim_sync.cpp -- see coop/interactables/desk_sim_sync.h.

#include "coop/interactables/desk_sim_sync.h"

#include "coop/interactables/desk_input_sync.h"

#include "coop/net/blob_chunks.h"  // Fnv64
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/desk_detector.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>

namespace coop::desk_sim_sync {
namespace {

namespace CD = ue_wrap::console_desk;
namespace DD = ue_wrap::desk_detector;
namespace sg = ue_wrap::script_gate;

std::atomic<coop::net::Session*> g_session{nullptr};

uint64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// The interp window: 1.5x the 100 ms (10 Hz) send interval -- the jitter bridge.
constexpr uint64_t kInterpWindowMs = 150;

// PER-CHANNEL interp. ONE shared LerpWindow reopened by EVERY packet leaves a channel whose
// target has stopped moving (the detector needle latched at exactly 1.0) never ARRIVING: the
// window rebase re-derives err on each packet and the ease asymptotes a hair under the host's
// bitwise-exact 1.0, so the client's own detector block re-crosses the <1.0 gate every frame --
// the stuck beep loop. Each channel therefore keeps its OWN deadline: an incoming target that is
// bitwise-unchanged KEEPS the deadline -> the channel arrives -> cur[i] = target[i] EXACT SNAP;
// only a CHANGED target rebases err + deadline.
struct SimInterp {
    static constexpr int N = 7;
    float    cur[N] = {};
    float    target[N] = {};
    float    err[N] = {};
    uint64_t deadline[N] = {};   // 0 = arrived/idle
    uint64_t lastAdvance = 0;
    bool primed = false;

    void SetTarget(const float* t, uint64_t now) {
        if (!primed) {
            for (int i = 0; i < N; ++i) { cur[i] = target[i] = t[i]; err[i] = 0.f; deadline[i] = 0; }
            lastAdvance = now;
            primed = true;
            return;
        }
        Advance(now);  // advance-before-rebase (the interp-starvation fix, world_actor shape)
        for (int i = 0; i < N; ++i) {
            if (t[i] == target[i]) continue;  // unchanged -> KEEP the deadline (arrive + snap)
            target[i] = t[i];
            err[i] = t[i] - cur[i];
            deadline[i] = now + kInterpWindowMs;
        }
    }

    void Advance(uint64_t now) {
        if (!primed) return;
        const uint64_t dtMs = now > lastAdvance ? now - lastAdvance : 0;
        lastAdvance = now;
        for (int i = 0; i < N; ++i) {
            if (deadline[i] == 0) continue;          // arrived -- cur[i] IS target[i]
            if (now >= deadline[i]) {                // EXACT snap at arrival
                cur[i] = target[i];
                err[i] = 0.f;
                deadline[i] = 0;
                continue;
            }
            const uint64_t leftMs = deadline[i] - now;
            const float frac = static_cast<float>(dtMs) / static_cast<float>(leftMs + dtMs);
            cur[i] += err[i] * frac;
            err[i] = target[i] - cur[i];
        }
    }

    // A channel set to `v` at once: across a crossing the needle may not pass through values below it.
    void Snap(int i, float v) {
        cur[i] = target[i] = v;
        err[i] = 0.f;
        deadline[i] = 0;
    }

    void Reset() { primed = false; for (int i = 0; i < N; ++i) deadline[i] = 0; }
};

SimInterp g_interp;
constexpr int kDecoded = 0, kNeedle = 1;  // SimInterp's channels, in DeskSimSnapshot's order
constexpr int kFrData = 3, kPoData = 4, kFrOffset = 5, kPoOffset = 6;
bool g_ownedFr = false, g_ownedPo = false;   // this peer's own dial held its channels last tick

CD::SimOutputs ToOutputs(const float* c) {
    CD::SimOutputs o;
    o.decoded = c[0]; o.resDetec = c[1]; o.rate = c[2]; o.frData = c[3];
    o.poData  = c[4]; o.frOffset = c[5]; o.poOffset = c[6];
    return o;
}

// The caught signal's identity, the same on every peer: its coordinates and frequency, which every copy holds as the
// catcher's bytes, and its object's name as text (an FName's index is this process's own). 0 with none caught.
uint64_t CaughtIdentity() {
    CD::CoordSignal sig;
    if (!CD::ReadCoordSignal(sig) || sig.objectName.empty() || sig.objectName == L"None") return 0;
    constexpr size_t kNameChars = 64;
    uint8_t buf[16 + 2 * kNameChars] = {};
    std::memcpy(buf, &sig.x, 4);
    std::memcpy(buf + 4, &sig.y, 4);
    std::memcpy(buf + 8, &sig.z, 4);
    std::memcpy(buf + 12, &sig.frequency, 4);
    const size_t chars = sig.objectName.size() < kNameChars ? sig.objectName.size() : kNameChars;
    std::memcpy(buf + 16, sig.objectName.data(), chars * 2);
    const uint64_t h = coop::blob_chunks::Fnv64(buf, 16 + chars * 2);
    return h ? h : 1;
}

bool AllFinite(const coop::net::DeskSimSnapshot& s) {
    const float v[] = { s.decoded, s.resDetec, s.rate, s.frData,
                        s.poData, s.frOffset, s.poOffset };
    for (float f : v) if (!std::isfinite(f)) return false;
    return true;
}

// ---- the needle's crossings ----------------------------------------------------------------------------------

UE_FACT_FN(kRowUbergraph, "desk_sim_sync", L"analogDScreenTest_C", L"ExecuteUbergraph_analogDScreenTest", Declared);
UE_FACT_FN(kRowAutoSave, "desk_sim_sync", L"analogDScreenTest_C", L"autoSave", Declared);
UE_FACT_FN(kRowFormDownload, "desk_sim_sync", L"analogDScreenTest_C", L"formDownload", Declared);
constexpr int kTagUbergraph = 0x44534C30;  // 'DSL0'
constexpr int kTagAutoSave = 0x44534C31;   // 'DSL1'
constexpr int kTagForm = 0x44534C32;       // 'DSL2'

bool RoleIs(coop::net::Role role) {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == role;
}

// The host's crossings since its session began: a loop resume (kLoopEntry) whose step took the needle from below 1
// to 1 or above, which only the step can do. The latest one's caught signal is read as it lands and carried with the
// count, so a catch that follows at once cannot rename the crossing before its snapshot goes out. The ubergraph's
// locals are one persistent frame every entry shares, and the nested download_repeatPlay entry rewrites EntryPoint
// inside a stepping call, so the entry and the needle are read at pre and carried to post by the gate's depth; a
// post that never comes leaves a slot the next pre rewrites.
uint32_t g_crossings = 0;
uint64_t g_crossingIdentity = 0;
struct Before {
    float needle = 0.f;
    bool  loop = false;
};
constexpr int kMaxDepth = 32;
Before g_before[kMaxDepth];

sg::Verdict OnUbergraphPre(const sg::Call& c) {
    if (c.depth <= 0 || c.depth >= kMaxDepth) return sg::Verdict::Run;
    Before& b = g_before[c.depth];
    int32_t entry = -1;
    b.loop = RoleIs(coop::net::Role::Host) && DD::ReadEntry(c.function, c.locals, entry) && entry == DD::kLoopEntry &&
             DD::ReadNeedle(c.object, b.needle);
    return sg::Verdict::Run;
}

void OnUbergraphPost(const sg::Call& c) {
    if (c.depth <= 0 || c.depth >= kMaxDepth) return;
    Before& b = g_before[c.depth];
    if (!b.loop) return;
    b.loop = false;
    float after = 0.f;
    if (b.needle < 1.f && DD::ReadNeedle(c.object, after) && after >= 1.f) {
        g_crossingIdentity = CaughtIdentity();
        ++g_crossings;
    }
}

// A client's autoSave, reached only from its looker_behind crossing, is refused: that save is the host's, and its
// row reaches the client by signal_sync.
sg::Verdict OnAutoSavePre(const sg::Call&) {
    if (!RoleIs(coop::net::Role::Client)) return sg::Verdict::Run;
    return sg::Verdict::Cancel;
}

void OnFormPost(const sg::Call& c);

// The lane's three watches, each registered once and named by one pointer, since the gate knows a watch by the
// literals it was registered with. One the gate refuses, or one that settles dead (its name resolved into a full
// table), is said once and ends the attempts: the gate says why, and a retry each tick would scan its table for nothing.
struct DeskWatch {
    const ue_wrap::facts::Function* row;
    int            tag;
    sg::PreFn      pre;
    sg::PostFn     post;
    bool           registered;
};
DeskWatch g_watches[] = {
    {&kRowUbergraph, kTagUbergraph, &OnUbergraphPre, &OnUbergraphPost, false},
    {&kRowAutoSave, kTagAutoSave, &OnAutoSavePre, nullptr, false},
    {&kRowFormDownload, kTagForm, nullptr, &OnFormPost, false},
};
bool g_saidLive = false;
bool g_refused = false;

void Register() {
    if (g_saidLive || g_refused) return;
    for (DeskWatch& w : g_watches) {
        if (w.registered) continue;
        w.registered = sg::WatchName(*w.row, w.tag, w.pre, w.post);
        if (!w.registered) {
            g_refused = true;
            UE_LOGE("desk_sim: the gate took no watch on %ls::%ls -- the needle's crossing is not the host's alone",
                    w.row->Class(), w.row->Name());
            return;
        }
    }
    sg::ResolvePendingNames();
    for (const DeskWatch& w : g_watches) {
        if (sg::NameWatchLive(*w.row, w.tag)) continue;
        if (sg::NameWatchSettled(*w.row, w.tag)) {
            g_refused = true;
            UE_LOGE("desk_sim: the watch on %ls::%ls settled dead -- the needle's crossing is not the host's alone",
                    w.row->Class(), w.row->Name());
        }
        return;
    }
    g_saidLive = true;
    UE_LOGI("desk_sim: the needle's loop, the desk's autoSave and its formDownload are watched");
}

// ---- the client: its parked step, and the host's edges painted ------------------------------------------------

// The client's step is parked at its only input: DL_detectorMultiplier, which only the step reads and no code
// writes, is 0 on a client's desk from the moment the desk exists, so the client never crosses on its own. The
// value read first (a placed desk may carry its own) is put back when the session ends.
struct Parked {
    void* desk = nullptr;
    float value = 0.f;
    bool  set = false;
    bool  saidRewritten = false;
};
Parked g_parked;

void Park(void* desk) {
    float now = 0.f;
    if (!DD::ReadMultiplier(desk, now)) return;
    if (g_parked.set && g_parked.desk == desk) {
        if (now == 0.f) return;
        DD::WriteMultiplier(desk, 0.f);
        if (!g_parked.saidRewritten) {
            g_parked.saidRewritten = true;
            UE_LOGW("desk_sim: the desk's detector multiplier read %.3f again -- parked again", now);
        }
        return;
    }
    g_parked = Parked{desk, now, true, false};
    DD::WriteMultiplier(desk, 0.f);
    UE_LOGI("desk_sim: CLIENT parked the desk's detector step (its multiplier %.3f -> 0); the host's crossings drive "
            "its painters", now);
}

void Unpark() {
    if (!g_parked.set) return;
    if (g_parked.desk && ue_wrap::reflection::IsLive(g_parked.desk)) DD::WriteMultiplier(g_parked.desk, g_parked.value);
    g_parked = Parked{};
}

// What a snapshot carries beside the outputs: the first one this client applies to a desk is its seed; after it, a
// moved crossing count and a changed canDL are the host's edges. The seen state is the desk's: a desk that replaces
// it (a level reload) is seeded afresh, and while there is no desk no edge is taken.
enum Edge : uint8_t { kSeed = 1, kCrossed = 2, kCanDL = 4 };
struct Seen {
    uint32_t crossings = 0;
    uint8_t  canDL = 0;
    bool     seeded = false;
    uint64_t crossed = 0;  // the caught signal the host last crossed on, by identity
};
Seen g_seen;
void* g_seenDesk = nullptr;    // the desk g_seen belongs to
bool g_formedCrossed = false;  // this client formed a download for the signal the host last crossed on

// A download formed on a client for the signal the host already crossed on takes the crossing's lasting painters:
// a joiner's object renderer spawns the signal object in its own formDownload, after the crossing, the object starts
// with fullyProcessed false, and a loop at 1 never reaches setFullyProcessedSignalObject again. Painted by the tick.
void OnFormPost(const sg::Call&) {
    if (!RoleIs(coop::net::Role::Client) || !g_seen.seeded || !g_seen.crossed) return;
    if (CaughtIdentity() == g_seen.crossed) g_formedCrossed = true;
}
uint32_t g_postSteps = 0;
bool     g_saidShut = false;

uint8_t EdgesOf(const coop::net::DeskSimSnapshot& snap) {
    uint8_t e = 0;
    if (!g_seen.seeded) e = kSeed;
    else {
        if (snap.crossings != g_seen.crossings) e |= kCrossed;
        if (snap.canDL != g_seen.canDL) e |= kCanDL;
    }
    g_seen = Seen{snap.crossings, snap.canDL, true, snap.downloadKey};
    return e;
}

// Run after the snapshot's outputs are written. The seed paints the lasting effects of a crossing already past (its
// beep is transient, its save the host's). A crossing runs this desk's own path after the step, from 4128, when
// the desk holds the download the host crossed. A changed canDL runs canSaveSignal, which repaints only on a change.
void Paint(void* desk, uint8_t e, const coop::net::DeskSimSnapshot& snap) {
    if (e & kSeed) {
        DD::CallCanSaveSignal(desk);
        DD::CallSetFullyProcessedSignalObject(desk);
        UE_LOGI("desk_sim: CLIENT seeded from the host's first snapshot (%u crossings, canDL %u) -- the lasting "
                "painters ran", snap.crossings, static_cast<unsigned>(snap.canDL));
        return;
    }
    if (e & kCrossed) {
        if (!snap.downloadKey || CaughtIdentity() != snap.downloadKey) {
            UE_LOGI("desk_sim: CLIENT passed over the host's crossing #%u -- this desk does not hold that download",
                    snap.crossings);
        } else if (DD::EnterPostStep(desk)) {
            ++g_postSteps;
            UE_LOGI("desk_sim: CLIENT ran the desk's post-step path on the host's crossing #%u", snap.crossings);
        } else if (!g_saidShut) {
            g_saidShut = true;
            UE_LOGW("desk_sim: CLIENT could not run the desk's post-step path on the host's crossing -- the entry is "
                    "shut; the finish beep and its painters are missed");
        }
    }
    if (e & kCanDL) DD::CallCanSaveSignal(desk);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    Register();
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;

    // ---- HOST: publish the live sim outputs (its BP is the authority), with its crossings and canDL. ----
    if (s->role() == coop::net::Role::Host) {
        if (CD::EnsureResolved()) {
            CD::SimOutputs cur;
            if (CD::ReadSimOutputs(cur)) {
                coop::net::DeskSimSnapshot snap{ cur.decoded, cur.resDetec, cur.rate, cur.frData,
                                                 cur.poData, cur.frOffset, cur.poOffset };
                snap.crossings = g_crossings;
                bool canDL = false;
                void* desk = CD::Instance();
                snap.canDL = (desk && DD::ReadCanDL(desk, canDL) && canDL) ? 1 : 0;
                snap.downloadKey = g_crossingIdentity;
                s->SetHostDeskSim(true, snap);
            }
        }
        g_interp.Reset();  // host is the source, never a mirror
        return;
    }

    // ---- CLIENT: interpolate the host's vector, overwrite the local sim, paint the host's edges. ----
    if (!s->connected() || !CD::EnsureResolved()) return;
    void* desk = CD::Instance();
    if (desk) Park(desk);
    if (desk != g_seenDesk) {
        g_seen = Seen{};
        g_formedCrossed = false;
        g_seenDesk = desk;
    }
    coop::net::DeskSimSnapshot snap;
    bool isNew = false;
    if (!s->TryGetHostDeskSim(snap, &isNew)) return;
    const uint64_t nowMs = NowMs();
    uint8_t edges = 0;
    if (isNew && AllFinite(snap)) {
        const float t[SimInterp::N] = { snap.decoded, snap.resDetec, snap.rate, snap.frData,
                                        snap.poData, snap.frOffset, snap.poOffset };
        g_interp.SetTarget(t, nowMs);
        edges = desk ? EdgesOf(snap) : 0;
        if (edges) {
            g_interp.Snap(kNeedle, snap.resDetec);
            g_interp.Snap(kDecoded, snap.decoded);
        }
    }
    if (!g_interp.primed) return;
    g_interp.Advance(nowMs);
    // The write is not the whole apply: these fields are painted by the desk's own tick, but a crossing's and
    // canDL's painters run from the host's edges (Paint).
    CD::SimOutputs out = ToOutputs(g_interp.cur);
    // A dial this peer is turning (or has just stopped) stays its own: its offset and the match its own tick derives
    // from it, until the host's stream shows where it stopped (desk_input_sync::OwnsDialOffset).
    const bool ownFr = coop::desk_input_sync::OwnsDialOffset(0, g_interp.target[kFrOffset]);
    const bool ownPo = coop::desk_input_sync::OwnsDialOffset(1, g_interp.target[kPoOffset]);
    // At the handoff the stream's target already shows where this peer stopped, but the eased value may still be on
    // its way from an older one: the dial's channels snap to the target, so the dial does not dip back and slide.
    if (g_ownedFr && !ownFr) {
        g_interp.Snap(kFrOffset, g_interp.target[kFrOffset]);
        g_interp.Snap(kFrData, g_interp.target[kFrData]);
        out = ToOutputs(g_interp.cur);
    }
    if (g_ownedPo && !ownPo) {
        g_interp.Snap(kPoOffset, g_interp.target[kPoOffset]);
        g_interp.Snap(kPoData, g_interp.target[kPoData]);
        out = ToOutputs(g_interp.cur);
    }
    g_ownedFr = ownFr;
    g_ownedPo = ownPo;
    if (ownFr || ownPo) {
        CD::SimOutputs mine;
        if (CD::ReadSimOutputs(mine)) {
            if (ownFr) { out.frOffset = mine.frOffset; out.frData = mine.frData; }
            if (ownPo) { out.poOffset = mine.poOffset; out.poData = mine.poData; }
        }
    }
    CD::WriteSimOutputs(out);
    if (desk && edges) Paint(desk, edges, snap);
    if (desk && g_formedCrossed) {
        g_formedCrossed = false;
        DD::CallCanSaveSignal(desk);
        DD::CallSetFullyProcessedSignalObject(desk);
        UE_LOGI("desk_sim: CLIENT formed a download for the signal the host crossed on (#%u) -- its lasting "
                "painters ran", g_seen.crossings);
    }
}

void OnDisconnect() {
    g_interp.Reset();
    g_ownedFr = g_ownedPo = false;
    Unpark();
    g_seen = Seen{};
    g_seenDesk = nullptr;
    g_formedCrossed = false;
    g_crossings = 0;
    g_crossingIdentity = 0;
    for (Before& b : g_before) b = Before{};
    g_saidShut = false;
    if (auto* s = g_session.load(std::memory_order_acquire))
        s->SetHostDeskSim(false, {});
}

uint32_t HostCrossings() { return g_crossings; }
uint32_t ClientPostSteps() { return g_postSteps; }

}  // namespace coop::desk_sim_sync
