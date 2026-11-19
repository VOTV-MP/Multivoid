// coop/dev/prop_accuracy_drill.cpp -- [dev] a client handles props the ways players do. See the header.
//
// SLOT 1 does it, from VOTVCOOP_PROP_ACC_DELAY ms (default 30000) after its session connects, at the table
// VOTVCOOP_PROP_ACC_TABLE names by key (default the metal table with seven drives and a box on it, 19 m from the base's
// spawn). Each step is logged `[ACC] step` with the wall clock the drift census samples by, so the rig's drift script
// can judge each step's props once they settle:
//   carry: grabs an item off the table, runs 3 m away and back at a sprint (30 cm every 50 ms) and lets go over the table;
//   put:   grabs another, steps 50 cm along the table's edge, looks down at the top and lets go;
//   throw: grabs a third and throws it away from the table as the fire key's grab branch does (traceThrow, throwShit,
//          thrown, dropGrabObject);
//   table: grabs the table itself, with what is left on it, pulls it 150 cm at a walk (10 cm every 50 ms) and lets go.
// The player stands 120 cm off each target on the side facing where the player stood when the drill began (the way it
// came in, so open floor). Each grab is the director's aimed grab; a step that cannot grab is logged and skipped. 12 s
// separate the steps, for the props to come to rest.

#include "coop/dev/prop_accuracy_drill.h"

#include "coop/config/config.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_key_index.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/engine_physics.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::prop_accuracy_drill {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace DR = coop::director;
using ue_wrap::FRotator;
using ue_wrap::FVector;

bool On() {
    static const bool v = coop::config::ReadEnv("VOTVCOOP_PROP_ACC") == "1";
    return v;
}

uint64_t Now() { return ::GetTickCount64(); }
uint64_t WallMs() {
    FILETIME ft{};
    ::GetSystemTimePreciseAsFileTime(&ft);
    return ((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000ull;
}

std::string KeyOf(void* actor) {
    std::vector<coop::prop_key_index::DebugEntry> all;
    coop::prop_key_index::DebugCollect(all);
    for (const auto& e : all)
        if (e.actor == actor) return std::string(e.key.begin(), e.key.end());
    return "?";
}

// The keyed props on `table`'s top: within 80 cm of its middle across, 40 to 200 cm over its origin.
std::vector<void*> ItemsOn(void* table) {
    std::vector<void*> out;
    FVector t{};
    if (!E::TryGetActorLocation(table, t)) return out;
    std::vector<coop::prop_key_index::DebugEntry> all;
    coop::prop_key_index::DebugCollect(all);
    for (const auto& e : all) {
        if (!e.actor || e.actor == table || !R::IsLiveByIndex(e.actor, e.idx)) continue;
        FVector at{};
        if (!E::TryGetActorLocation(e.actor, at)) continue;
        if (std::fabs(at.X - t.X) < 80.f && std::fabs(at.Y - t.Y) < 80.f && at.Z - t.Z > 40.f && at.Z - t.Z < 200.f)
            out.push_back(e.actor);
    }
    return out;
}

bool Throw(void* player, void* item) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(player), L"grabbing_component");
    void* comp = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(player) + off) : nullptr;
    FVector com{};
    if (!comp || !R::IsLive(comp) || !E::GetComponentCenterOfMass(comp, com)) return false;
    void* traceFn = R::FindDispatchFunctionCached(R::ClassOf(player), L"traceThrow");
    ue_wrap::ParamFrame trace(traceFn);
    FVector vel{};
    if (!traceFn || !trace.valid() || !trace.Set<bool>(L"debug", false) || !trace.Set<FVector>(L"weaponLocation", com) ||
        !trace.Set<FVector>(L"cameraLocation", com) || !ue_wrap::Call(player, trace) ||
        !trace.GetRaw(L"velocity_fromCamera", &vel, sizeof(vel)))
        return false;
    void* shitFn = R::FindDispatchFunctionCached(R::ClassOf(player), L"throwShit");
    ue_wrap::ParamFrame shit(shitFn);
    if (!shitFn || !shit.valid() || !shit.Set<void*>(L"InputPin", comp) || !shit.Set<FVector>(L"NewVel", vel) ||
        !ue_wrap::Call(player, shit))
        return false;
    DR::CallWithPlayer(item, L"thrown", player);
    return DR::CallOnPlayer(player, L"dropGrabObject");
}

// The body's collision as the engine holds it: enabled (0 none .. 3 both), object type, response (0 ignore, 1 overlap,
// 2 block) to channels 0..7, simulating, awake. What a copy's items fall through, if they do.
void* g_table = nullptr;

std::string Collision(void* actor) {
    void* mesh = actor ? ue_wrap::prop::GetStaticMesh(actor) : nullptr;
    if (!mesh) return "no mesh";
    char buf[160];
    uint8_t type = 0xFF;
    if (void* fn = R::FindDispatchFunctionCached(R::ClassOf(mesh), L"GetCollisionObjectType")) {
        ue_wrap::ParamFrame f(fn);
        if (f.valid() && ue_wrap::Call(mesh, f)) f.GetRaw(L"ReturnValue", &type, 1);
    }
    std::string resp;
    void* rfn = R::FindDispatchFunctionCached(R::ClassOf(mesh), L"GetCollisionResponseToChannel");
    for (uint8_t ch = 0; ch < 8; ++ch) {
        uint8_t r = 9;
        if (rfn) {
            ue_wrap::ParamFrame f(rfn);
            if (f.valid() && f.Set<uint8_t>(L"Channel", ch) && ue_wrap::Call(mesh, f)) f.GetRaw(L"ReturnValue", &r, 1);
        }
        resp += static_cast<char>('0' + r);
    }
    std::snprintf(buf, sizeof(buf), "enabled=%d type=%u resp[0..7]=%s sim=%d awake=%d", E::GetComponentCollisionEnabled(mesh),
                  type, resp.c_str(), E::IsComponentSimulatingPhysics(mesh) ? 1 : 0, E::ReadActorRootBodyAwake(actor));
    return buf;
}

void ProbeCollision(const char* when) {
    UE_LOGI("[ACC] collision %s: table %s", when, Collision(g_table).c_str());
    for (void* it : ItemsOn(g_table)) {
        UE_LOGI("[ACC] collision %s: item %ls %s", when, R::ClassNameOf(it).c_str(), Collision(it).c_str());
        break;
    }
}

void Look(void* me, const FVector& at) {
    if (void* ctrl = E::GetController(me)) E::SetControlRotation(ctrl, DR::LookAt(E::GetCameraLocation(), at));
}

enum class Phase { Wait, Stand, Grab, Act, Rest, Done };
const char* const kSteps[] = {"carry", "put", "throw", "table"};
constexpr int kStepCount = 4;

uint64_t g_startAt = 0, g_phaseAt = 0, g_nextMove = 0;
Phase g_phase = Phase::Wait;
int g_step = 0, g_moves = 0;
int g_probes = 0;
int g_retries = 0;   // items tried for this step after a grab failed
std::vector<void*> g_used;
void* g_target = nullptr;
std::unique_ptr<DR::AimedGrab> g_grab;
FVector g_came{};     // the horizontal unit from the table towards where the player stood at the start
FVector g_stand{};    // where the player stood for this step's grab

void Log(const char* what, void* target) {
    FVector at{};
    if (target) E::TryGetActorLocation(target, at);
    UE_LOGI("[ACC] step %s: %s %ls %s at (%.0f,%.0f,%.0f) wall=%llu", kSteps[g_step], what,
            target ? R::ClassNameOf(target).c_str() : L"-", target ? KeyOf(target).c_str() : "-", at.X, at.Y, at.Z,
            static_cast<unsigned long long>(WallMs()));
}

void Next(Phase p) {
    g_phase = p;
    g_phaseAt = Now();
}

void* PickTarget() {
    if (g_step == 3) return g_table;
    for (void* it : ItemsOn(g_table)) {
        bool used = false;
        for (void* u : g_used) used = used || u == it;
        if (!used) {
            g_used.push_back(it);
            return it;
        }
    }
    return nullptr;
}

void Move(void* me, float along, float side) {
    const FVector perp{-g_came.Y, g_came.X, 0.f};
    E::SetActorLocation(me, FVector{g_stand.X + g_came.X * along + perp.X * side, g_stand.Y + g_came.Y * along + perp.Y * side,
                                    g_stand.Z});
}

void TickClient(void* me) {
    const uint64_t t = Now() - g_startAt;
    switch (g_phase) {
    case Phase::Wait: {
        static const uint64_t delay = [] {
            const std::string v = coop::config::ReadEnv("VOTVCOOP_PROP_ACC_DELAY");
            return v.empty() ? 30000ull : std::strtoull(v.c_str(), nullptr, 10);
        }();
        if (t < delay) return;
        std::string key = coop::config::ReadEnv("VOTVCOOP_PROP_ACC_TABLE");
        if (key.empty()) key = "-0ec4XnrmHRnZ5J-nv9LDQ";
        g_table = coop::prop_key_index::Find(std::wstring(key.begin(), key.end()));
        FVector tt{}, me0{};
        if (!g_table || !E::TryGetActorLocation(g_table, tt) || !E::TryGetActorLocation(me, me0)) {
            UE_LOGI("[ACC] table %s not found -- the drill stops", key.c_str());
            Next(Phase::Done);
            return;
        }
        const float dx = me0.X - tt.X, dy = me0.Y - tt.Y, d = std::sqrt(dx * dx + dy * dy);
        g_came = d > 1.f ? FVector{dx / d, dy / d, 0.f} : FVector{-1.f, 0.f, 0.f};
        UE_LOGI("[ACC] table %s at (%.0f,%.0f,%.0f), %zu items on it; the player came from (%.2f,%.2f)", key.c_str(), tt.X,
                tt.Y, tt.Z, ItemsOn(g_table).size(), g_came.X, g_came.Y);
        Next(Phase::Stand);
        return;
    }
    case Phase::Stand: {
        g_target = PickTarget();
        if (!g_target) {
            Log("SKIPPED -- no target", nullptr);
            Next(Phase::Rest);
            return;
        }
        FVector at{}, tt{};
        E::TryGetActorLocation(g_target, at);
        E::TryGetActorLocation(g_table, tt);
        const float off = g_step == 3 ? 150.f : 120.f;
        g_stand = FVector{at.X + g_came.X * off, at.Y + g_came.Y * off, tt.Z + 100.f};
        E::SetActorLocation(me, g_stand);
        Look(me, at);
        g_grab = std::make_unique<DR::AimedGrab>(me, g_target);
        Log("goes for", g_target);
        if (g_step == 3) ProbeCollision("before the grab");
        Next(Phase::Grab);
        return;
    }
    case Phase::Grab: {
        if (Now() - g_phaseAt < 800u) return;  // the stand lands
        const DR::GrabState st = g_grab->Tick();
        if (st == DR::GrabState::Working) return;
        if (st == DR::GrabState::Failed) {
            UE_LOGI("[ACC] grab failed: %s", g_grab->Why());
            if (g_step != 3 && ++g_retries < 3) {   // another item off the table
                Log("the grab failed, trying another item", g_target);
                Next(Phase::Stand);
                return;
            }
            Log("SKIPPED -- the grab failed", g_target);
            Next(Phase::Rest);
            return;
        }
        Log("grabbed", g_target);
        g_retries = 0;
        g_moves = 0;
        g_nextMove = Now() + 300;
        Next(Phase::Act);
        return;
    }
    case Phase::Act: {
        if (Now() < g_nextMove) return;
        const std::string name = kSteps[g_step];
        if (name == "carry") {
            // 3 m away from the table and back, 30 cm every 50 ms: a sprint.
            if (g_moves < 20) {
                Move(me, 30.f * static_cast<float>(g_moves < 10 ? g_moves + 1 : 19 - g_moves), 0.f);
                ++g_moves;
                g_nextMove = Now() + 50;
                return;
            }
            Log("lets go over the table after the run", g_target);
            DR::CallOnPlayer(me, L"dropGrabObject");
        } else if (name == "put") {
            // Half a metre along the edge, the eyes on the top, then the hand opens.
            if (g_moves < 5) {
                Move(me, 0.f, 10.f * static_cast<float>(g_moves + 1));
                FVector tt{};
                E::TryGetActorLocation(g_table, tt);
                Look(me, FVector{tt.X, tt.Y, tt.Z + 90.f});
                ++g_moves;
                g_nextMove = Now() + 150;
                return;
            }
            Log("puts it down on the table", g_target);
            DR::CallOnPlayer(me, L"dropGrabObject");
        } else if (name == "throw") {
            if (void* ctrl = E::GetController(me)) {
                FRotator view = DR::LookAt(E::GetCameraLocation(),
                                           FVector{g_stand.X + g_came.X * 500.f, g_stand.Y + g_came.Y * 500.f, g_stand.Z + 100.f});
                E::SetControlRotation(ctrl, view);
            }
            if (g_moves++ < 3) {   // the turn lands, the held prop swings round
                g_nextMove = Now() + 150;
                return;
            }
            Log(Throw(me, g_target) ? "threw" : "THROW FAILED", g_target);
        } else {
            // The table: pulled 150 cm in fifteen 10 cm steps, 50 ms apart.
            if (g_moves < 15) {
                Move(me, 10.f * static_cast<float>(g_moves + 1), 0.f);
                ++g_moves;
                g_nextMove = Now() + 50;
                return;
            }
            ProbeCollision("held");
            Log("lets go of the table", g_target);
            DR::CallOnPlayer(me, L"dropGrabObject");
            g_probes = 0;
        }
        Next(Phase::Rest);
        return;
    }
    case Phase::Rest: {
        static const uint64_t kProbeAt[] = {300, 1500, 3000, 5000, 8000, 11500};
        if (g_step == 3 && g_probes < 6 && Now() - g_phaseAt >= kProbeAt[g_probes]) {
            char when[32];
            std::snprintf(when, sizeof(when), "%llu ms after the release", static_cast<unsigned long long>(kProbeAt[g_probes]));
            ProbeCollision(when);
            ++g_probes;
        }
        if (Now() - g_phaseAt < 12000u) return;
        Log("rested", g_target);
        g_retries = 0;
        if (++g_step >= kStepCount) {
            UE_LOGI("[ACC] DONE wall=%llu", static_cast<unsigned long long>(WallMs()));
            Next(Phase::Done);
        } else {
            Next(Phase::Stand);
        }
        return;
    }
    case Phase::Done:
        return;
    }
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected()) return;
    if (coop::players::Registry::Get().LocalPeerId() != 1) return;
    void* me = coop::players::Registry::Get().Local();
    if (!me) return;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[ACC] drill armed");
    }
    TickClient(me);
}

}  // namespace coop::dev::prop_accuracy_drill
