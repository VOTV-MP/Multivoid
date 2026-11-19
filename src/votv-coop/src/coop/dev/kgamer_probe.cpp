// coop/dev/kgamer_probe.cpp -- [dev] a gamer Kerfur Omega on every peer (VOTVCOOP_KGAMER_PROBE=1 on every peer).
//
// The gamer Omega (kerfurOmega_col_gamer_C) has no colour of its own: its ReceiveTick writes a rainbow into its mesh's
// materials' `color` each frame. HOST, after it armed: at 100 s spawns the catalog's gamer Omega prop 1.5 m before its
// player, at 110 s turns it on (the prop's actionOptionIndex, action 8, as the E-press), at 130 s off (the Omega's dropKerfurProp), at 145 s on again.
// EVERY PEER, every 2 s from 95 s to 175 s: each gamer Omega it holds, in either form, with its hidden flag, whether it
// ticks, its `hsv` and its mesh's first material's `color` (`[KGAMER]`). VOTVCOOP_KGAMER_PROP and _NPC name other
// classes (the custom-colour Omega, a Kerfus); _COLOR=1 has client slot 1 pick green for the prop 3 s after it holds
// one, through the prop's intComs_applyColor as the picker calls it; _T0 moves the host's steps (seconds, 100).

#include "coop/dev/kgamer_probe.h"

#include "coop/cheats/item_catalog.h"
#include "coop/config/config.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::kgamer_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
using ue_wrap::FVector;

std::wstring EnvW(const char* name, const wchar_t* dflt) {
    const std::string v = coop::config::ReadEnv(name);
    return v.empty() ? std::wstring(dflt) : std::wstring(v.begin(), v.end());
}
// The prop and Omega classes, VOTVCOOP_KGAMER_PROP and VOTVCOOP_KGAMER_NPC (the gamer pair by default).
const std::wstring& PropCls() {
    static const std::wstring c = EnvW("VOTVCOOP_KGAMER_PROP", L"prop_kerfurOmega_col_gamer_C");
    return c;
}
const std::wstring& NpcCls() {
    static const std::wstring c = EnvW("VOTVCOOP_KGAMER_NPC", L"kerfurOmega_col_gamer_C");
    return c;
}

bool On() {
    static const bool on = coop::config::ReadEnv("VOTVCOOP_KGAMER_PROBE") == "1";
    return on;
}

uint64_t Now() { return ::GetTickCount64(); }

std::vector<void*> All(const wchar_t* cls) {
    std::vector<void*> out;
    void* const c = OI::ClassByName(cls);
    if (c)
        OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &out);
    return out;
}

void* ObjField(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const char*>(obj) + off) : nullptr;
}

bool CallNoArgs(void* obj, const wchar_t* fn) {
    void* const f = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fn) : nullptr;
    ue_wrap::ParamFrame frame(f);
    return f && frame.valid() && ue_wrap::Call(obj, frame);
}

// The mesh's first material and its `color`, as text; "-" when unread.
std::string ColorOf(void* actor) {
    void* const mesh = ObjField(actor, L"Mesh");
    void* const getMat = mesh ? R::FindDispatchFunctionCached(R::ClassOf(mesh), L"GetMaterial") : nullptr;
    ue_wrap::ParamFrame gm(getMat);
    if (!getMat || !gm.valid() || !gm.Set<int32_t>(L"ElementIndex", 0) || !ue_wrap::Call(mesh, gm)) return "-";
    void* const mat = gm.Get<void*>(L"ReturnValue");
    if (!mat) return "no material";
    const std::wstring cls = R::ClassNameOf(mat);
    std::string out(cls.begin(), cls.end());
    void* const getVec = R::FindDispatchFunctionCached(R::ClassOf(mat), L"K2_GetVectorParameterValue");
    ue_wrap::ParamFrame gv(getVec);
    const R::FName name = ue_wrap::fname_utils::StringToFName(L"color");
    if (getVec && gv.valid() && gv.SetRaw(L"ParameterName", &name, sizeof(name)) && ue_wrap::Call(mat, gv)) {
        float c[4] = {};
        gv.GetRaw(L"ReturnValue", c, sizeof(c));
        char buf[96];
        std::snprintf(buf, sizeof(buf), " color=(%.2f,%.2f,%.2f)", c[0], c[1], c[2]);
        out += buf;
    } else {
        out += " color=?";
    }
    return out;
}

void Report() {
    for (const wchar_t* cls : {NpcCls().c_str(), PropCls().c_str()})
        for (void* a : All(cls)) {
            FVector p{};
            E::TryGetActorLocation(a, p);
            const unsigned eid = static_cast<unsigned>(coop::element::Registry::Get().EidForActor(a));
            const int32_t hidOff = R::FindPropertyOffset(R::ClassOf(a), L"bHidden");
            const int hidden = hidOff >= 0 ? (*(static_cast<const uint8_t*>(a) + hidOff) != 0) : -1;
            const int32_t hsvOff = R::FindPropertyOffset(R::ClassOf(a), L"hsv");
            float hsv[3] = {-1.f, -1.f, -1.f};
            if (hsvOff >= 0) std::memcpy(hsv, static_cast<const char*>(a) + hsvOff, sizeof(hsv));
            UE_LOGI("[KGAMER] %ls eid=%u at (%.0f,%.0f,%.0f) tick=%d hiddenByte=%d hsv=(%.2f,%.2f,%.2f) mat=%s", cls, eid, p.X,
                    p.Y, p.Z, E::IsActorTickEnabled(a) ? 1 : 0, hidden, hsv[0], hsv[1], hsv[2], ColorOf(a).c_str());
        }
}

std::wstring GamerRow() {
    const auto names = coop::cheats::item_catalog::Names();
    if (!names) return L"";
    for (const std::string& n : *names) {
        std::string low = n;
        for (char& c : low) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        if (low.find("gamer") != std::string::npos) UE_LOGI("[KGAMER] catalog row '%s'", n.c_str());
    }
    for (const std::string& n : *names) {
        std::string low = n;
        for (char& c : low) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        if (low.find("gamer") != std::string::npos && low.find("kerfur") != std::string::npos)
            return std::wstring(n.begin(), n.end());
    }
    return L"";
}

bool SpawnRow(const std::wstring& row, const FVector& at) {
    void* const gm = ue_wrap::world_singleton::Gamemode();
    ue_wrap::ParamFrame f(gm ? R::FindDispatchFunctionCached(R::ClassOf(gm), L"spawnPropThroughGamemode") : nullptr);
    if (!f.valid()) return false;
    const R::FName name = ue_wrap::fname_utils::StringToFName(row.c_str());
    ue_wrap::FTransform xf{};
    xf.TX = at.X;
    xf.TY = at.Y;
    xf.TZ = at.Z;
    return f.SetRaw(L"prop", &name, sizeof(name)) && f.SetRaw(L"InputPin", &xf, sizeof(xf)) &&
           f.Set<int32_t>(L"amount", 1) && ue_wrap::Call(gm, f);
}

uint64_t g_armedAt = 0, g_reportAt = 0;
int g_step = 0;

void* First(const wchar_t* cls) {
    const std::vector<void*> v = All(cls);
    return v.empty() ? nullptr : v.front();
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_armedAt) {
        g_armedAt = Now();
        coop::cheats::item_catalog::RequestRefresh();
        UE_LOGI("[KGAMER] probe armed role=%s", host ? "host" : "client");
    }
    const uint64_t t = Now() - g_armedAt;
    // VOTVCOOP_KGAMER_T0 (seconds, 100 by default) moves the host's steps, so they can end before a late joiner.
    static const uint64_t t0 = [] {
        const std::string v = coop::config::ReadEnv("VOTVCOOP_KGAMER_T0");
        return v.empty() ? 100000ull : static_cast<uint64_t>(std::strtoul(v.c_str(), nullptr, 10)) * 1000ull;
    }();
    if (host) {
        if (g_step == 0 && t >= t0) {
            g_step = 1;
            // No catalog row names it, so the prop is spawned by its class, as the game's own spawns of it are.
            const std::wstring row = GamerRow();
            void* const me = coop::players::Registry::Get().Local();
            void* const cls = OI::ClassByName(PropCls().c_str());
            FVector at{};
            bool ran = false;
            if (cls && me && E::TryGetActorLocation(me, at)) {
                const FVector fwd = E::GetActorForwardVector(me);
                const FVector to{at.X + fwd.X * 150.f, at.Y + fwd.Y * 150.f, at.Z};
                void* const a = E::BeginDeferredSpawn(cls, to, {});
                ran = a && E::FinishDeferredSpawn(a, to, {});
            } else if (!row.empty() && me && E::TryGetActorLocation(me, at)) {
                ran = SpawnRow(row, {at.X + 150.f, at.Y, at.Z});
            }
            UE_LOGI("[KGAMER] host: spawned the gamer Omega prop (class %d, ran=%d)", cls ? 1 : 0, ran ? 1 : 0);
        } else if ((g_step == 1 && t >= t0 + 10000) || (g_step == 3 && t >= t0 + 45000)) {
            ++g_step;
            // As the player's E-press turns it on: the prop's own actionOptionIndex with action 8.
            void* const prop = First(PropCls().c_str());
            void* const fn = prop ? R::FindDispatchFunctionCached(R::ClassOf(prop), L"actionOptionIndex") : nullptr;
            ue_wrap::ParamFrame f(fn);
            const bool ran = fn && f.valid() && f.Set<void*>(L"player", coop::players::Registry::Get().Local()) &&
                             f.Set<uint8_t>(L"action", 8) && ue_wrap::Call(prop, f);
            UE_LOGI("[KGAMER] host: turned on (ran=%d)", ran ? 1 : 0);
        } else if (g_step == 2 && t >= t0 + 30000) {
            g_step = 3;
            UE_LOGI("[KGAMER] host: turned off (ran=%d)", CallNoArgs(First(NpcCls().c_str()), L"dropKerfurProp") ? 1 : 0);
        } else if (g_step == 4 && t >= t0 + 60000) {
            g_step = 5;
            UE_LOGI("[KGAMER] DONE");
        }
    }
    // A client (slot 1) picks green for the prop 3 s after it first holds it, as the colour picker's apply does.
    static bool s_coloured = false;
    static uint64_t s_seenAt = 0;
    void* const seen = (!host && !s_coloured) ? First(PropCls().c_str()) : nullptr;
    if (seen && !s_seenAt) s_seenAt = Now();
    if (seen && Now() - s_seenAt >= 3000 && coop::players::Registry::Get().LocalPeerId() == 1 &&
        coop::config::ReadEnv("VOTVCOOP_KGAMER_COLOR") == "1") {
        s_coloured = true;
        void* const prop = seen;
        void* const fn = prop ? R::FindDispatchFunctionCached(R::ClassOf(prop), L"intComs_applyColor") : nullptr;
        ue_wrap::ParamFrame f(fn);
        const float green[4] = {0.f, 1.f, 0.f, 1.f};
        const bool ran = fn && f.valid() && f.SetRaw(L"color", green, sizeof(green)) && ue_wrap::Call(prop, f);
        UE_LOGI("[KGAMER] client: picked green for the prop (ran=%d)", ran ? 1 : 0);
    }
    if (t < 95000 || t > 175000 || Now() < g_reportAt) return;
    g_reportAt = Now() + 2000;
    Report();
}

}  // namespace coop::dev::kgamer_probe
