// coop/creatures/kerfus_brain.cpp -- see coop/creatures/kerfus_brain.h.

#include "coop/creatures/kerfus_brain.h"

#include "coop/net/session.h"
#include "coop/props/rider_hold.h"

#include "ue_wrap/actors/kerfus.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_physics.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <unordered_map>

namespace coop::kerfus_brain {
namespace {

namespace E  = ue_wrap::engine;
namespace UK = ue_wrap::kerfus;
namespace sg = ue_wrap::script_gate;


// The brain's bodies (p_kerfus.cpp, bp_cfg): the tick (energy drain and charge, the wheel's
// torque, the stuck jump), the two timer events that re-path and unstick it, the movement functions,
// the server job, the laptop's jump, the haunting, and the cord events, whose one effect is `charging`.
// The water events set only the body's damping, and upd() only its looks and sounds, so both stay.
UE_FACT_FN(kRowReceiveTick, "kerfus_brain", L"p_kerfus_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowCheckJump, "kerfus_brain", L"p_kerfus_C", L"checkJump", Declared);
UE_FACT_FN(kRowUpdatePath, "kerfus_brain", L"p_kerfus_C", L"updatePath", Declared);
UE_FACT_FN(kRowMovePawnTo, "kerfus_brain", L"p_kerfus_C", L"movePawnTo", Declared);
UE_FACT_FN(kRowFindBrokenServer, "kerfus_brain", L"p_kerfus_C", L"findBrokenServer", Declared);
UE_FACT_FN(kRowTask, "kerfus_brain", L"p_kerfus_C", L"task", Declared);
UE_FACT_FN(kRowJump, "kerfus_brain", L"p_kerfus_C", L"jump", Declared);
UE_FACT_FN(kRowRCjump, "kerfus_brain", L"p_kerfus_C", L"RCjump", Declared);
UE_FACT_FN(kRowPossess, "kerfus_brain", L"p_kerfus_C", L"possess", Declared);
UE_FACT_FN(kRowPossessTimer, "kerfus_brain", L"p_kerfus_C", L"possessTimer", Declared);
UE_FACT_FN(kRowCordPlugged, "kerfus_brain", L"p_kerfus_C", L"cordPlugged", Declared);
UE_FACT_FN(kRowCordUnplugged, "kerfus_brain", L"p_kerfus_C", L"cordUnplugged", Declared);
const ue_wrap::facts::Function* const kBrain[] = {
    &kRowReceiveTick, &kRowCheckJump, &kRowUpdatePath, &kRowMovePawnTo, &kRowFindBrokenServer, &kRowTask,
    &kRowJump, &kRowRCjump, &kRowPossess, &kRowPossessTimer, &kRowCordPlugged, &kRowCordUnplugged,
};
constexpr size_t kBrainCount = sizeof(kBrain) / sizeof(kBrain[0]);
constexpr int kTagBrainBase = 0x4B465300;  // 'KFS' + the row

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched[kBrainCount] = {};
bool g_saidLive = false;

// Per session: the refusals by row, and whether a row's first refusal was said.
unsigned long long g_refused[kBrainCount] = {};
bool g_said[kBrainCount] = {};

// The navigation pawns this client stilled, by Kerfus: the Kerfus and the pawn's movement, by slot and serial, and
// whether this lane holds that movement (rider_hold.h), false when its owner had stopped it already.
struct Stilled {
    ue_wrap::CachedObjRef kerfus;
    ue_wrap::CachedObjRef movement;
    bool held = false;
};
std::unordered_map<void*, Stilled> g_stilled;

bool OnClient() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == coop::net::Role::Client;
}

// The brain's navigation pawn (UK::NavPawn), welded onto the Kerfus's body, moves while the body does not simulate
// -- asleep, frozen, parked -- and every second the brain's checkJump, refused here, puts it back on the body. On
// a client it would fall without end, and the body's centre of mass with it. So a client seats it where the
// host's brain keeps it, on the body's origin, upright, and holds its movement still for the session: from the
// first refused tick after it spawned, one map lookup per tick after that. A scheduler kill
// (docs/coop-sync-doctrine.md, step 4) that removes a second author: the pawn's place is the host brain's, which
// holds it at the origin. Nothing restarts it while the hold is out -- the pawn's Blueprint has no graph, checkJump
// holds the Kerfus's one write to its movement, and a receiver's park takes a hold of its own beside this one --
// and the session's end gives it back.
void StillPawn(void* k) {
    auto it = g_stilled.find(k);
    if (it != g_stilled.end()) {
        if (it->second.kerfus.Get() == k && it->second.movement.Alive()) return;
        if (it->second.held) coop::rider_hold::Give(it->second.movement.Raw());  // a Kerfus or pawn that died
        g_stilled.erase(it);
    }
    void* pawn = UK::NavPawn(k);
    void* movement = pawn ? E::GetCharacterMovementComponent(pawn) : nullptr;
    ue_wrap::FVector at{};
    if (!movement || !E::TryGetActorLocation(k, at)) return;  // not spawned yet: the next tick asks again
    E::SetActorLocation(pawn, at);
    E::SetActorRotation(pawn, ue_wrap::FRotator{0.f, 0.f, 0.f});
    Stilled& st = g_stilled[k];
    st.kerfus.Set(k);
    st.movement.Set(movement);
    st.held = coop::rider_hold::Take(movement);
    UE_LOGI("kerfus_brain: stilled the Kerfus %p's navigation pawn on this client -- seated on the body, its "
            "movement %s", k, st.held ? "held" : "stopped already by its owner");
}

// The wheel is an actor of its own (UK::ReadWheel), held to the body by a physics constraint the host's tick drives. On a
// client the body is moved by the host's drive stream with its physics off, and the constraint does not drag a sleeping
// wheel after a body that is set into place: the rig's clients kept the wheel where the Kerfus was spawned while the
// body drove 18 to 43 m away (Ghezra/Multivoid issue 20). So a client stops the wheel's own physics and seats the wheel
// actor on its child-actor slot, where the Kerfus's Blueprint placed it, attached: it moves with the body the host
// streams, and the host's tick, refused here, would be its only other mover. One map lookup per tick once seated; the
// session's end gives the wheel back its physics.
struct Seated {
    ue_wrap::CachedObjRef kerfus;
    ue_wrap::CachedObjRef wheel;
    ue_wrap::CachedObjRef body;
};
std::unordered_map<void*, Seated> g_seated;

void SeatWheel(void* k) {
    auto it = g_seated.find(k);
    if (it != g_seated.end() && it->second.kerfus.Get() == k && it->second.wheel.Alive()) return;
    UK::Wheel w;
    if (!UK::ReadWheel(k, w)) return;   // not spawned yet: the next tick asks again
    ue_wrap::FVector was{}, slot{};
    E::TryGetActorLocation(w.actor, was);
    E::TryGetComponentLocation(w.slot, slot);
    E::SetComponentSimulatePhysics(w.body, false);
    const bool attached = E::AttachActorToComponentSocket(w.actor, w.slot, nullptr);
    Seated& s = g_seated[k];
    s.kerfus.Set(k);
    s.wheel.Set(w.actor);
    s.body.Set(w.body);
    const float dx = was.X - slot.X, dy = was.Y - slot.Y, dz = was.Z - slot.Z;
    UE_LOGI("kerfus_brain: seated the Kerfus %p's wheel on its slot on this client -- physics off, attached=%d, it was "
            "%.0f cm off", k, attached ? 1 : 0, std::sqrt(dx * dx + dy * dy + dz * dz));
}

sg::Verdict OnBrainPre(const sg::Call& c) {
    if (!OnClient()) return sg::Verdict::Run;
    const size_t row = static_cast<size_t>(c.tag - kTagBrainBase);
    if (row == 0 && c.object) {  // the tick
        StillPawn(c.object);
        SeatWheel(c.object);
    }
    if (row < kBrainCount) {
        ++g_refused[row];
        if (!g_said[row]) {
            g_said[row] = true;
            UE_LOGI("kerfus_brain: refused the Kerfus's %ls on this client -- the host runs its brain (said once "
                    "a session)", kBrain[row]->Name());
        }
    }
    return sg::Verdict::Cancel;
}

void Register() {
    for (size_t i = 0; i < kBrainCount; ++i)
        if (!g_watched[i])
            g_watched[i] = sg::WatchName(*kBrain[i], kTagBrainBase + static_cast<int>(i), &OnBrainPre, nullptr);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    Register();
}

void Tick() {
    if (g_saidLive) return;
    Register();
    sg::ResolvePendingNames();
    for (size_t i = 0; i < kBrainCount; ++i)
        if (!g_watched[i] || !sg::NameWatchLive(*kBrain[i], kTagBrainBase + static_cast<int>(i)))
            return;
    g_saidLive = true;
    UE_LOGI("kerfus_brain: the Kerfus's %zu brain bodies are watched -- a client refuses them", kBrainCount);
}

void OnDisconnect() {
    unsigned long long total = 0;
    for (size_t i = 0; i < kBrainCount; ++i) total += g_refused[i];
    if (total)
        UE_LOGI("kerfus_brain: session end -- %llu brain bodies refused on this client (tick %llu)", total,
                g_refused[0]);
    for (size_t i = 0; i < kBrainCount; ++i) { g_refused[i] = 0; g_said[i] = false; }
    // The holds go back: the brain runs here once the session is over.
    for (const auto& entry : g_stilled)
        if (entry.second.held) coop::rider_hold::Give(entry.second.movement.Raw());
    g_stilled.clear();
    for (const auto& entry : g_seated) {
        void* const wheel = entry.second.wheel.Get();
        void* const body = entry.second.body.Get();
        if (wheel) E::DetachActorFromParent(wheel);
        if (body) E::SetComponentSimulatePhysics(body, true);
    }
    g_seated.clear();
}

}  // namespace coop::kerfus_brain
