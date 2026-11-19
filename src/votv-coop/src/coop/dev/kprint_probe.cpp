// coop/dev/kprint_probe.cpp -- [dev] a 3D print on every peer (VOTVCOOP_KPRINT_PROBE=host|client on every peer: who
// prints). The printer (3dPrinterAnim_C) spawns a printedObject_C with its `name` the model's folder and hands it the
// mesh it loaded (loadFromPrinter); coop/props/printer_sync carries the print and the object. Times are from the
// probe's arm. THE HOST at 100 s spawns a printer 2 m before its player when its world holds none. THE ACTOR (the host,
// or client slot 1) at 110 s selects the model VOTVCOOP_KPRINT_MODEL (mvtest by default) on its copy and prints; the
// host calls its printer's Finish at 125 s so the print ends at its next steps, lifts each printed object 80 cm at 150 s
// (it falls and rests) and toggles its light at 165 s. EVERY PEER, every 3 s from 95 s to 200 s: each printer and each
// printed object (element id, name, loaded, sections, physics, light, bites, where).

#include "coop/dev/kprint_probe.h"

#include "coop/config/config.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/fstring_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::kprint_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
using ue_wrap::FVector;

const std::string& Role() {
    static const std::string r = coop::config::ReadEnv("VOTVCOOP_KPRINT_PROBE");
    return r;
}
std::wstring Model() {
    const std::string v = coop::config::ReadEnv("VOTVCOOP_KPRINT_MODEL");
    return v.empty() ? std::wstring(L"mvtest") : std::wstring(v.begin(), v.end());
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

template <class T>
T Field(void* obj, const wchar_t* name, T dflt) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? *reinterpret_cast<const T*>(static_cast<const char*>(obj) + off) : dflt;
}

std::wstring Str(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? ue_wrap::field_io::ReadFStringAt(obj, off) : L"?";
}

int Sections(void* obj) {
    void* const mesh = Field<void*>(obj, L"proceduralMesh", nullptr);
    void* const fn = mesh ? R::FindDispatchFunctionCached(R::ClassOf(mesh), L"GetNumSections") : nullptr;
    ue_wrap::ParamFrame f(fn);
    return (fn && f.valid() && ue_wrap::Call(mesh, f)) ? f.Get<int32_t>(L"ReturnValue") : -1;
}

void Report() {
    for (void* p : All(L"3dPrinterAnim_C")) {
        UE_LOGI("[KPRINT] printer eid=%u active=%d canPrint=%d meshName='%ls'",
                static_cast<unsigned>(coop::element::Registry::Get().EidForActor(p)), Field<uint8_t>(p, L"active", 0),
                Field<uint8_t>(p, L"canPrint", 0), Str(p, L"meshName").c_str());
    }
    for (void* o : All(L"printedObject_C")) {
        FVector at{};
        E::TryGetActorLocation(o, at);
        UE_LOGI("[KPRINT] printed eid=%u name='%ls' loaded=%d sections=%d phys=%d light=%d bites=%d at (%.1f,%.1f,%.1f)",
                static_cast<unsigned>(coop::element::Registry::Get().EidForActor(o)), Str(o, L"name").c_str(),
                Field<uint8_t>(o, L"loaded", 0), Sections(o), ue_wrap::engine::IsActorRootSimulatingPhysics(o) ? 1 : 0,
                Field<uint8_t>(o, L"lightActive", 0), Field<int32_t>(o, L"bites", 0), at.X, at.Y, at.Z);
    }
}

bool CallNoArgs(void* obj, const wchar_t* fn) {
    void* const f = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fn) : nullptr;
    ue_wrap::ParamFrame frame(f);
    return f && frame.valid() && ue_wrap::Call(obj, frame);
}

// The game's Custom Content rule, in this process's rules and never saved: print and a printed object's load refuse
// without it. Every peer's, as a save that allows it gives every peer.
void AllowCustomContent() {
    void* const instance = R::FindObjectByClass(L"mainGameInstance_C");
    void* const cls = instance ? R::ClassOf(instance) : nullptr;
    const int32_t rules = cls ? R::FindPropertyOffset(cls, L"gameRules") : -1;
    void* const type = cls ? R::PropertyInnerStruct(cls, L"gameRules") : nullptr;
    const int32_t rule = type ? R::FindPropertyOffsetByPrefix(type, L"customContent_") : -1;
    if (rules >= 0 && rule >= 0) static_cast<uint8_t*>(instance)[rules + rule] = 1;
    UE_LOGI("[KPRINT] the Custom Content rule set on in this process (ok=%d)", rules >= 0 && rule >= 0 ? 1 : 0);
}

uint64_t g_armedAt = 0, g_reportAt = 0;
int g_step = 0, g_printed = 0;

}  // namespace

void Tick(coop::net::Session* session) {
    if (Role().empty() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_armedAt) {
        g_armedAt = Now();
        UE_LOGI("[KPRINT] probe armed role=%s actor=%s", host ? "host" : "client", Role().c_str());
        AllowCustomContent();
    }
    const uint64_t t = Now() - g_armedAt;
    const bool actor = (Role() == "host") == host && (host || coop::players::Registry::Get().LocalPeerId() == 1);
    std::vector<void*> printers = All(L"3dPrinterAnim_C");
    void* const printer = printers.empty() ? nullptr : printers.front();
    if (host && g_step == 0 && t >= 100000) {
        g_step = 1;
        if (!printer) {
            void* const me = coop::players::Registry::Get().Local();
            void* const cls = OI::ClassByName(L"3dPrinterAnim_C");
            FVector at{};
            bool ran = false;
            if (cls && me && E::TryGetActorLocation(me, at)) {
                const FVector fwd = E::GetActorForwardVector(me);
                const FVector to{at.X + fwd.X * 200.f, at.Y + fwd.Y * 200.f, at.Z - 60.f};
                void* const a = E::BeginDeferredSpawn(cls, to, {});
                ran = a && E::FinishDeferredSpawn(a, to, {});
            }
            UE_LOGI("[KPRINT] host: no printer here, spawned one (class %d, ran=%d)", cls ? 1 : 0, ran ? 1 : 0);
        }
    }
    if (actor && g_printed == 0 && t >= 110000) {
        g_printed = 1;
        void* const fn = printer ? R::FindDispatchFunctionCached(R::ClassOf(printer), L"selectMesh") : nullptr;
        ue_wrap::ParamFrame f(fn);
        uint8_t name[16] = {};
        const bool sel = fn && f.valid() && ue_wrap::fstring_utils::MintFString(Model(), name) &&
                         f.SetRaw(L"meshName", name, sizeof(name)) && ue_wrap::Call(printer, f);
        // As the printer's widget does; printer_sync's gate judges it as a player's press.
        const bool printed = CallNoArgs(printer, L"print");
        UE_LOGI("[KPRINT] actor: selected '%ls' (ran=%d) and printed (ran=%d)", Model().c_str(), sel ? 1 : 0,
                printed ? 1 : 0);
    }
    if (host && g_step == 1 && t >= 125000) {
        g_step = 2;
        UE_LOGI("[KPRINT] host: Finish (active=%d, ran=%d)", Field<uint8_t>(printer, L"active", 0),
                CallNoArgs(printer, L"Finish") ? 1 : 0);
    } else if (host && g_step == 2 && t >= 150000) {
        g_step = 3;
        for (void* o : All(L"printedObject_C")) {
            FVector at{};
            if (!E::TryGetActorLocation(o, at)) continue;
            E::SetActorLocation(o, FVector{at.X + 30.f, at.Y, at.Z + 80.f});
            UE_LOGI("[KPRINT] host: lifted a printed object from (%.1f,%.1f,%.1f)", at.X, at.Y, at.Z);
        }
    } else if (host && g_step == 3 && t >= 165000) {
        g_step = 4;
        for (void* o : All(L"printedObject_C")) {
            const int32_t off = R::FindPropertyOffset(R::ClassOf(o), L"lightActive");
            if (off >= 0) static_cast<uint8_t*>(o)[off] ^= 1;
            UE_LOGI("[KPRINT] host: toggled a printed object's light (updLight ran=%d)", CallNoArgs(o, L"updLight") ? 1 : 0);
        }
    } else if (host && g_step == 4 && t >= 185000) {
        g_step = 5;
        UE_LOGI("[KPRINT] DONE");
    }
    if (t < 95000 || t > 400000 || Now() < g_reportAt) return;
    g_reportAt = Now() + 3000;
    Report();
}

}  // namespace coop::dev::kprint_probe
