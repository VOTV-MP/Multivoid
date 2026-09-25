// coop/dev/physmods_drill.cpp -- see coop/dev/physmods_drill.h.

#include "coop/dev/physmods_drill.h"

#include "coop/config/config.h"
#include "coop/interactables/physmods_sync.h"  // CanonicalsAdopted
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/phys_mods.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"  // DestroyActor

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace coop::dev::physmods_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace PM = ue_wrap::phys_mods;
namespace CD = ue_wrap::console_desk;
namespace E  = ue_wrap::engine;

// The host's legs, once per process.
enum class Host : uint8_t { Start, Plugged, Unplugged, Done };
Host    g_host = Host::Start;
int     g_hostSlot = -1;   // leg 1's slot
uint8_t g_hostType = 0;    // leg 1's module type, of which leg 2 plugs a second
// The client's legs, once per session.
enum class Client : uint8_t { Arm, Waiting, Plugged, Acking, Done };
Client   g_client = Client::Arm;
int      g_theirSlot = -1;  // where the host's module arrived
int      g_mySlot = -1;     // leg 2's slot
uint8_t  g_myType = 0;
uint64_t g_adoptsAtLeg4 = 0;
uint8_t  g_afterLeg4[PM::kSlots] = {};  // this peer's array once its leg-4 ops ran
// Set before a lifted call and cleared after it. An engine fault inside the call is absorbed at its own
// dispatch and the call returns false, so the put-back runs in the same tick; a fault in this file's code
// between the two would skip it, and the next tick puts back the coldswap it left lifted.
bool g_liftOut = false;

int CountModules(const uint8_t arr[PM::kSlots]) {
    int n = 0;
    for (int i = 0; i < PM::kSlots; ++i) n += arr[i] ? 1 : 0;
    return n;
}

int FirstEmpty(const uint8_t arr[PM::kSlots]) {
    for (int i = 0; i < PM::kSlots; ++i)
        if (!arr[i]) return i;
    return -1;
}

int FirstFilled(const uint8_t arr[PM::kSlots]) {
    for (int i = 0; i < PM::kSlots; ++i)
        if (arr[i]) return i;
    return -1;
}

// The slot holding `byte`, other than `skip`, or -1.
int SlotOf(const uint8_t arr[PM::kSlots], uint8_t byte, int skip) {
    for (int i = 0; i < PM::kSlots; ++i)
        if (i != skip && arr[i] == byte) return i;
    return -1;
}

// "{slot:byte ...}" for the filled slots.
std::string Describe(const uint8_t arr[PM::kSlots]) {
    std::string out = "{";
    char buf[16];
    for (int i = 0; i < PM::kSlots; ++i) {
        if (!arr[i]) continue;
        std::snprintf(buf, sizeof(buf), out.size() > 1 ? " %d:%u" : "%d:%u", i, arr[i]);
        out += buf;
    }
    return out + "}";
}

bool ReadDeskBool(void* desk, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    return R::FindBoolProperty(R::ClassOf(desk), name, off, mask) &&
           (reinterpret_cast<const uint8_t*>(desk)[off] & mask) != 0;
}

void WriteDeskBool(void* desk, const wchar_t* name, bool on) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(desk), name, off, mask)) return;
    uint8_t& b = reinterpret_cast<uint8_t*>(desk)[off];
    b = on ? static_cast<uint8_t>(b | mask) : static_cast<uint8_t>(b & ~mask);
}

// plugInModule and the press's unplug both explode on a hot desk: a console active with coldswap on.
bool WouldExplode(void* desk) {
    const bool active = ReadDeskBool(desk, L"active_console") || ReadDeskBool(desk, L"active_comp") ||
                        ReadDeskBool(desk, L"active_coords") || ReadDeskBool(desk, L"active_download") ||
                        ReadDeskBool(desk, L"active_play");
    return active && ReadDeskBool(desk, L"coldswapEnabled");
}

void PutBack(void* desk) {
    if (g_liftOut && desk) WriteDeskBool(desk, L"coldswapEnabled", true);
    g_liftOut = false;
}

// The drill's own call, on a desk that would explode it, runs with coldswap lifted, so it is the call
// a cold desk takes.
template <class F>
bool Lifted(void* desk, F call) {
    g_liftOut = WouldExplode(desk);
    if (g_liftOut) WriteDeskBool(desk, L"coldswapEnabled", false);
    const bool ok = call();
    PutBack(desk);
    return ok;
}

// A module of `byte` into empty slot `slot`: one spawned at the desk and handed to plugInModule, as a
// carried one arrives there. One the desk refuses (a type off its list) is destroyed, not left lying
// at the desk. True when the slot holds `byte` after.
bool Plug(void* desk, void* pawn, int slot, uint8_t byte) {
    void* cls = PM::ClassForByte(byte);
    ue_wrap::FVector at{};
    if (!cls || !E::TryGetActorLocation(desk, at)) return false;
    void* module = E::SpawnActor(cls, {at.X, at.Y, at.Z + 120.f});
    if (!module) return false;
    uint8_t arr[PM::kSlots];
    const bool took = Lifted(desk, [&] { return PM::CallPlugInModule(desk, module, slot, pawn); }) &&
                      PM::ReadArray(arr) && arr[slot] == byte;
    if (!took && R::IsLive(module)) E::DestroyActor(module);
    return took;
}

// The lowest module type the desk takes, other than `avoid`, plugged into `slot`; 0 when none took.
uint8_t PlugAnyType(void* desk, void* pawn, int slot, uint8_t avoid) {
    for (int b = 1; b < 64; ++b) {
        const uint8_t byte = static_cast<uint8_t>(b);
        if (byte == avoid || !PM::ClassForByte(byte)) continue;
        if (Plug(desk, pawn, slot, byte)) return byte;
    }
    return 0;
}

// A player's E on `slot`: the slot empty after, the press made and the slot still full, or no press made
// (the desk wrapper's log names the step).
enum class Out : uint8_t { Emptied, StillFull, NoPress };
Out Unplug(void* desk, void* pawn, int slot) {
    if (!Lifted(desk, [&] { return PM::CallPressSlot(desk, pawn, slot); })) return Out::NoPress;
    uint8_t arr[PM::kSlots];
    return PM::ReadArray(arr) && !arr[slot] ? Out::Emptied : Out::StillFull;
}

const char* Say(Out o) {
    switch (o) {
    case Out::Emptied:   return "the slot is empty";
    case Out::StillFull: return "the press ran and the slot still holds it";
    case Out::NoPress:   return "no press was made";
    }
    return "?";
}

// A leg that cannot go on (the desk refused the drill's own plug or press, or it did not start empty)
// ends on a line a run passes as its --dead-marker, "[PHYSMODS-DRILL] ABANDONED": nothing was measured.
// A lane that measured wrong ends on one a run passes as its --fail-marker, "[PHYSMODS-DRILL] FAIL".
// Either way the run dies with its phase instead of waiting out its clock.
void Abandon(const char* side, const std::string& why) {
    UE_LOGW("[PHYSMODS-DRILL] ABANDONED on the %s: %s", side, why.c_str());
}

void Fail(const char* side, const std::string& why) {
    UE_LOGW("[PHYSMODS-DRILL] FAIL on the %s: %s", side, why.c_str());
}

void HostTick(void* desk, void* pawn, const uint8_t arr[PM::kSlots]) {
    switch (g_host) {
    case Host::Start: {
        g_host = Host::Done;
        if (CountModules(arr)) {
            Abandon("host", "the desk holds " + Describe(arr) + " at the drill's start; the drill needs an empty "
                    "desk, since its rejoin leg reads a module at the joiner's world-ready as the joiner's own load");
            return;
        }
        g_hostSlot = 0;
        g_hostType = PlugAnyType(desk, pawn, g_hostSlot, 0);
        if (!g_hostType) {
            Abandon("host", "leg 1 landed no module in slot 0");
            return;
        }
        UE_LOGI("[PHYSMODS-DRILL] host leg 1 plugged byte=%u into slot=%d through plugInModule", g_hostType, g_hostSlot);
        g_host = Host::Plugged;
        return;
    }
    case Host::Plugged: {
        const int theirs = SlotOf(arr, g_hostType, g_hostSlot);
        if (theirs < 0) return;  // the client's second module of this type has not arrived
        g_host = Host::Done;
        const Out out = Unplug(desk, pawn, g_hostSlot);
        if (out != Out::Emptied) {
            Abandon("host", std::string("leg 3, its own slot 0 through the E press: ") + Say(out));
            return;
        }
        UE_LOGI("[PHYSMODS-DRILL] host leg 3 saw the client's second byte=%u in slot=%d and unplugged its own "
                "slot=%d through the E press", g_hostType, theirs, g_hostSlot);
        g_host = Host::Unplugged;
        return;
    }
    case Host::Unplugged: {
        // Both modules of the type gone, and the client's other one in.
        if (SlotOf(arr, g_hostType, -1) >= 0 || !CountModules(arr)) return;
        g_host = Host::Done;
        if (CountModules(arr) != 1) {
            Fail("host", "the array is " + Describe(arr) + ": both modules of byte " + std::to_string(g_hostType) +
                 " are gone, but not one module of another type alone");
            return;
        }
        UE_LOGI("[PHYSMODS-DRILL] host DONE the array is %s: both modules of byte=%u are gone and the client's "
                "module of another type is in -- PASS", Describe(arr).c_str(), g_hostType);
        return;
    }
    case Host::Done:
        return;
    }
}

void ClientTick(void* desk, void* pawn, const uint8_t arr[PM::kSlots]) {
    switch (g_client) {
    case Client::Arm: {
        if (const int n = CountModules(arr)) {
            g_client = Client::Done;
            UE_LOGI("[PHYSMODS-DRILL] client REJOIN DONE its load wrote %d module(s) %s and this session plugged "
                    "nothing", n, Describe(arr).c_str());
            return;
        }
        g_client = Client::Waiting;
        return;
    }
    case Client::Waiting: {
        g_theirSlot = FirstFilled(arr);
        if (g_theirSlot < 0) return;  // the host's module arrives through its canonical
        g_myType = arr[g_theirSlot];
        g_mySlot = FirstEmpty(arr);
        g_client = Client::Done;
        if (g_mySlot < 0 || !Plug(desk, pawn, g_mySlot, g_myType)) {
            Abandon("client", "leg 2 landed no second module of byte " + std::to_string(g_myType));
            return;
        }
        UE_LOGI("[PHYSMODS-DRILL] client leg 2 saw the host's byte=%u in slot=%d and plugged a second byte=%u into "
                "slot=%d through plugInModule", g_myType, g_theirSlot, g_myType, g_mySlot);
        g_client = Client::Plugged;
        return;
    }
    case Client::Plugged: {
        if (arr[g_theirSlot]) return;  // the host's unplug has not arrived
        g_client = Client::Done;
        const Out out = Unplug(desk, pawn, g_mySlot);
        if (out != Out::Emptied) {
            Abandon("client", std::string("leg 4, its own slot through the E press: ") + Say(out));
            return;
        }
        uint8_t now[PM::kSlots];
        const int slot = PM::ReadArray(now) ? FirstEmpty(now) : -1;
        const uint8_t other = slot >= 0 ? PlugAnyType(desk, pawn, slot, g_myType) : 0;
        if (!other || !PM::ReadArray(g_afterLeg4)) {
            Abandon("client", "leg 4 landed no module of another type");
            return;
        }
        g_adoptsAtLeg4 = coop::physmods_sync::CanonicalsAdopted();
        UE_LOGI("[PHYSMODS-DRILL] client leg 4 saw the host's slot=%d empty, unplugged its own slot=%d through the "
                "E press and plugged byte=%u into slot=%d", g_theirSlot, g_mySlot, other, slot);
        g_client = Client::Acking;
        return;
    }
    case Client::Acking: {
        // The host sends its canonical after each op, applied or denied, so a count alone cannot tell the two
        // apart: the ops were applied when, two canonicals on, this peer's array is still the one its own
        // leg-4 ops left.
        const uint64_t back = coop::physmods_sync::CanonicalsAdopted() - g_adoptsAtLeg4;
        if (back < 2) return;
        g_client = Client::Done;
        if (std::memcmp(arr, g_afterLeg4, PM::kSlots) != 0) {
            Fail("client", "two canonicals after leg 4 the array is " + Describe(arr) + ", not the " +
                 Describe(g_afterLeg4) + " its own ops left (an op was denied)");
            return;
        }
        UE_LOGI("[PHYSMODS-DRILL] client ACKED the host's canonical came back %llu time(s) after leg 4 and the array "
                "is still %s", static_cast<unsigned long long>(back), Describe(arr).c_str());
        return;
    }
    case Client::Done:
        return;
    }
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::physmods_drill);
    return s;
}

void Tick(coop::net::Session* s) {
    if (!IsEnabled()) return;
    if (g_liftOut) PutBack(CD::Instance());  // a lifted call that faulted
    if (!s || !s->connected()) return;
    const bool host = s->role() == coop::net::Role::Host;
    if (host ? g_host == Host::Done : g_client == Client::Done) return;
    if (host ? !s->IsSlotWorldReady(1) : !coop::net_pump::HasAnnouncedWorldReady()) return;
    if (!PM::EnsureResolved()) return;
    void* desk = CD::Instance();
    void* pawn = coop::players::Registry::Get().Local();
    uint8_t arr[PM::kSlots];
    if (!desk || !pawn || !PM::ReadArray(arr)) return;
    if (host)
        HostTick(desk, pawn, arr);
    else
        ClientTick(desk, pawn, arr);
}

void OnDisconnect() {
    g_client = Client::Arm;
}

}  // namespace coop::dev::physmods_drill
