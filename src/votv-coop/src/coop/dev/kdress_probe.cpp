// coop/dev/kdress_probe.cpp -- [dev] an Omega's outfit and its pat (VOTVCOOP_KDRESS_PROBE=client|host, the player who
// acts, on both peers), for Ghezra/Multivoid#38.
//
// BOTH PEERS, from 20 s after a client is connected: each Omega's outfit is logged every 5 s (`[KDRESS] eid=`).
// THE ACTOR: at 20 s it logs list_kerfurdrip's rows and, unless it is already within 180 cm, stands 150 cm from the
// nearest Omega (teleportWObackrooms); at
// 24 s it takes VOTVCOOP_KDRESS_ROW's row, or else the first that is not the pants, into its hand and puts it on the Omega through the Omega's own
// equipItem, as its use key does; at 34 s it pats it (actionName "pat"); at 44 s it takes the item off through the
// Omega's own upgradeTake, as the equipment screen does. The lane's own lines (`kerfur_dress:`) say what crossed.

#include "coop/dev/kdress_probe.h"

#include "coop/config/config.h"
#include "coop/creatures/npc_sync.h"
#include "coop/dev/drill_hand.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/npc.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/actors/kerfur_dress.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/votv_lib.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::kdress_probe {
namespace {

namespace R = ue_wrap::reflection;
namespace K = ue_wrap::kerfur;
namespace KD = ue_wrap::kerfur_dress;

uint64_t Now() { return ::GetTickCount64(); }

const std::string& Who() {
    static const std::string v = coop::config::ReadEnv("VOTVCOOP_KDRESS_PROBE");
    return v;
}

uint64_t g_startAt = 0;
uint64_t g_nextLog = 0;
int g_step = 0;
void* g_omega = nullptr;
std::wstring g_row;

uint32_t EidOf(void* actor) {
    std::vector<coop::element::Npc*> elems;
    coop::element::MirrorManager<coop::element::Npc>::Instance().Snapshot(elems);
    for (coop::element::Npc* el : elems)
        if (el && el->GetActor() == actor) return el->GetId();
    return coop::npc_sync::GetNpcIdForActor(actor);
}

std::vector<std::wstring> DripRows() {
    std::vector<std::wstring> out;
    void* const table = R::FindObject(L"list_kerfurdrip", L"DataTable");
    void* const cdo = R::FindClassDefaultObject(L"DataTableFunctionLibrary");
    void* const fn = cdo ? R::FindFunction(R::ClassOf(cdo), L"GetDataTableRowNames") : nullptr;
    if (!table || !fn) return out;
    ue_wrap::ParamFrame f(fn);
    struct { void* data; int32_t num; int32_t max; } arr{nullptr, 0, 0};
    if (!f.valid() || !f.Set<void*>(L"Table", table) || !f.SetRaw(L"OutRowNames", &arr, sizeof(arr)) ||
        !ue_wrap::Call(cdo, f) || !f.GetRaw(L"OutRowNames", &arr, sizeof(arr)))
        return out;
    for (int32_t i = 0; i < arr.num && i < 256; ++i) out.push_back(R::ToString(static_cast<R::FName*>(arr.data)[i]));
    if (arr.data) R::EngineFree(arr.data);
    return out;
}

void LogOutfits() {
    for (void* k : K::AllKerfurs()) {
        std::vector<std::wstring> drip;
        KD::ReadDrip(k, drip);
        std::wstring line;
        for (const auto& n : drip) line += n + L" ";
        UE_LOGI("[KDRESS] eid=%u wears %zu: %ls", EidOf(k), drip.size(), line.c_str());
    }
}

// The player to `at` facing `yaw` through its own teleportWObackrooms: a plain teleport is a clip the game answers with a
// trip to the backrooms.
bool PlaceMe(void* me, const ue_wrap::FVector& at, float yaw) {
    void* f = R::FindFunction(R::ClassOf(me), L"teleportWObackrooms");
    ue_wrap::ParamFrame pf(f);
    ue_wrap::FTransform t{};
    const float half = yaw * 0.5f * 0.01745329252f;
    t.RotZ = std::sin(half);
    t.RotW = std::cos(half);
    t.TX = at.X; t.TY = at.Y; t.TZ = at.Z;
    t.SX = t.SY = t.SZ = 1.f;
    return f && pf.valid() && pf.Set<ue_wrap::FTransform>(L"newTransform", t) && pf.Set<bool>(L"useRotation", true) &&
           ue_wrap::Call(me, pf);
}

void* NearestOmega(void* me) {
    ue_wrap::FVector at{};
    if (!ue_wrap::engine::TryGetActorLocation(me, at)) return nullptr;
    void* best = nullptr;
    float bestD = 1e12f;
    for (void* k : K::AllKerfurs()) {
        ue_wrap::FVector kl{};
        if (!ue_wrap::engine::TryGetActorLocation(k, kl)) continue;
        const float d = (kl.X - at.X) * (kl.X - at.X) + (kl.Y - at.Y) * (kl.Y - at.Y);
        if (d < bestD) { bestD = d; best = k; }
    }
    return best;
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (Who().empty() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[KDRESS] probe armed role=%s actor=%s", host ? "host" : "client", Who().c_str());
    }
    const uint64_t t = Now() - g_startAt;
    if (t >= 20000u && Now() >= g_nextLog) {
        g_nextLog = Now() + 5000;
        LogOutfits();
    }
    if (Who() != (host ? "host" : "client")) return;
    void* const me = coop::players::Registry::Get().Local();
    if (!me) return;
    if (g_step == 0 && t >= 20000u) {
        g_step = 1;
        const std::vector<std::wstring> rows = DripRows();
        std::wstring all;
        const std::string want = coop::config::ReadEnv("VOTVCOOP_KDRESS_ROW");
        for (const auto& r : rows) {
            all += r + L" ";
            if (!want.empty() && r == std::wstring(want.begin(), want.end())) g_row = r;
        }
        for (const auto& r : rows)
            if (g_row.empty() && r.find(L"pant") == std::wstring::npos) g_row = r;
        UE_LOGI("[KDRESS] list_kerfurdrip has %zu rows: %ls", rows.size(), all.c_str());
        g_omega = NearestOmega(me);
        ue_wrap::FVector kl{}, ml{};
        const bool read = g_omega && ue_wrap::engine::TryGetActorLocation(g_omega, kl) &&
                          ue_wrap::engine::TryGetActorLocation(me, ml);
        const float dx = kl.X - ml.X, dy = kl.Y - ml.Y;
        // An Omega already in reach is not walked to: a teleport into a wall is a clip the game drops below the world.
        const bool placed = read && (dx * dx + dy * dy <= 180.f * 180.f || PlaceMe(me, {kl.X - 150.f, kl.Y, kl.Z + 20.f}, 0.f));
        UE_LOGI("[KDRESS] actor: Omega eid=%u at (%.0f,%.0f,%.0f), placed=%d, row '%ls'", g_omega ? EidOf(g_omega) : 0,
                kl.X, kl.Y, kl.Z, placed ? 1 : 0, g_row.c_str());
    }
    if (!g_omega || !R::IsLive(g_omega)) return;
    if (g_step == 1 && t >= 24000u) {
        g_step = 2;
        ue_wrap::votv_lib::PropObject po;
        ue_wrap::votv_lib::PropToObject(ue_wrap::fname_utils::StringToFName(g_row), me, po);
        const std::wstring cls = po.objectClass ? R::ToString(R::NameOf(po.objectClass)) : std::wstring(L"prop_C");
        const bool held = coop::dev::drill_hand::TakeIntoHandNow(g_row.c_str(), cls.c_str());
        ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(g_omega), L"equipItem"));
        const bool ran = held && f.valid() && f.Set<void*>(L"inputPin", me) && ue_wrap::Call(g_omega, f);
        UE_LOGI("[KDRESS] actor: '%ls' (%ls) in the hand=%d, equipItem ran=%d", g_row.c_str(), cls.c_str(), held ? 1 : 0,
                ran ? 1 : 0);
    }
    if (g_step == 2 && t >= 34000u) {
        g_step = 3;
        UE_LOGI("[KDRESS] actor: pat ran=%d", K::RunActionName(g_omega, me, L"pat") ? 1 : 0);
    }
    if (g_step == 3 && t >= 44000u) {
        g_step = 4;
        ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(g_omega), L"upgradeTake"));
        const R::FName item = ue_wrap::fname_utils::StringToFName(g_row);
        const bool ran = f.valid() && f.SetRaw(L"item", &item, sizeof(item)) && ue_wrap::Call(g_omega, f);
        UE_LOGI("[KDRESS] actor: upgradeTake '%ls' ran=%d", g_row.c_str(), ran ? 1 : 0);
    }
}

}  // namespace coop::dev::kdress_probe
