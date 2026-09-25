// coop/dev/grime_drill.cpp -- see coop/dev/grime_drill.h.

#include "coop/dev/grime_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/interactables/grime_sync.h"
#include "coop/net/session.h"
#include "coop/player/roster.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/grime.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::grime_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace G  = ue_wrap::grime;
namespace OI = ue_wrap::object_index;
using Clock = std::chrono::steady_clock;

constexpr float kMinProcess  = 20.f;    // a partial of 10 leaves the decal standing
constexpr float kPartialSub  = 10.f;
constexpr float kDestroySub  = 1000.f;  // any process there is goes below zero
constexpr float kEps         = 0.0005f;
constexpr auto  kPhaseBound  = std::chrono::seconds(15);

enum class Phase { Unpicked, Partial, AwaitOtherFall, Destroy, AwaitOtherZero, Done };

struct Decal {
    void*        actor = nullptr;
    int32_t      idx = -1;
    std::wstring key;
    float        start = 0.f;
};
Decal g_mine, g_other;
Phase g_phase = Phase::Unpicked;
Clock::time_point g_since{};

const char* Side() { return coop::roster::LocalIsHost() ? "host" : "client"; }

bool RoleIsReady(coop::net::Session& s) {
    if (coop::roster::LocalIsHost()) {
        for (int slot = 1; slot < coop::net::kMaxPeers; ++slot)
            if (s.IsSlotWorldReady(slot)) return true;
        return false;
    }
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

// A bool field of the decal, its offset resolved once per class.
struct BoolField {
    const wchar_t* name;
    void* cls = nullptr;
    int32_t off = -1;
    uint8_t mask = 0;
};
bool ReadBool(BoolField& f, void* decal) {
    void* const cls = R::ClassOf(decal);
    if (cls != f.cls) {
        f.cls = cls;
        f.off = -1;
        f.mask = 0;
        R::FindBoolProperty(cls, f.name, f.off, f.mask);
    }
    return f.off >= 0 && f.mask != 0 && (static_cast<const uint8_t*>(decal)[f.off] & f.mask) != 0;
}

// A decal the drill can use: a clean lowers it, and the rain, which cleans outdoor decals near either
// peer's camera, leaves it alone, so every fall a peer sees is the other peer's clean.
bool Usable(void* decal) {
    static BoolField s_cleanable{L"isCleanable"};
    static BoolField s_resistRain{L"resistRain"};
    return ReadBool(s_cleanable, decal) && ReadBool(s_resistRain, decal);
}

void Done(const char* verdict) {
    g_phase = Phase::Done;
    UE_LOGI("[GRIME-DRILL] %s DONE %s", Side(), verdict);
}

// The two lowest position keys among the usable decals (grime_C and its subclasses) above kMinProcess:
// the same pair on both peers, the client's joined copy having taken the host's values.
bool Pick() {
    void* const cls = OI::ClassByName(L"grime_C");
    if (!cls) return false;
    struct Cand {
        void*        actor;
        int32_t      idx;
        std::wstring key;
        float        process;
    };
    std::vector<Cand> cands;
    // The decals are subclasses of grime_C (dust, blood, oil and the rest), each its own class in the index.
    struct Classes {
        void* base;
        std::vector<void*> list;
    } classes{cls, {}};
    OI::ForEachClass([](void* ctx, void* c, void*) {
        auto& x = *static_cast<Classes*>(ctx);
        if (R::IsDescendantOfAny(c, &x.base, 1)) x.list.push_back(c);
    }, &classes);
    for (void* c : classes.list) OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t index) {
        auto& out = *static_cast<std::vector<Cand>*>(ctx);
        if (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable)) return;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__") || !Usable(obj)) return;
        float p = 0.f;
        if (!G::ReadProcess(obj, p) || p <= kMinProcess) return;
        std::wstring key = coop::grime_sync::DebugPosKeyForActor(obj);
        if (!key.empty()) out.push_back(Cand{obj, index, std::move(key), p});
    }, &cands);
    if (cands.size() < 2) return false;
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.key < b.key; });
    const bool host = coop::roster::LocalIsHost();
    const Cand& mine = cands[host ? 1 : 0];
    const Cand& other = cands[host ? 0 : 1];
    g_mine = Decal{mine.actor, mine.idx, mine.key, mine.process};
    g_other = Decal{other.actor, other.idx, other.key, other.process};
    UE_LOGI("[GRIME-DRILL] %s cleans key='%ls' (process %.3f) and watches key='%ls' (process %.3f), of %zu",
            Side(), g_mine.key.c_str(), g_mine.start, g_other.key.c_str(), g_other.start, cands.size());
    return true;
}

// The rain's call form, no sponge and no sound, through ProcessEvent.
bool Clean(float sub) {
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(g_mine.actor), L"clean");
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<void*>(L"sponge", nullptr);
    f.Set<float>(L"Sub", sub);
    f.Set<bool>(L"noSound", true);
    return ue_wrap::Call(g_mine.actor, f);
}

bool ReadOther(float& v) {
    return R::IsLiveByIndex(g_other.actor, g_other.idx) && G::ReadProcess(g_other.actor, v);
}

}  // namespace

void Tick(coop::net::Session* session) {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::grime_drill);
    if (!s_on || g_phase == Phase::Done) return;
    if (!session || !session->connected() || !RoleIsReady(*session) || !G::EnsureResolved()) return;
    const auto now = Clock::now();
    switch (g_phase) {
    case Phase::Unpicked:
        if (OI::Backlog() != 0) return;  // the index holds the world first
        if (!Pick()) {
            Done("fewer than two cleanable decals that resist the rain -- INCONCLUSIVE");
            return;
        }
        g_phase = Phase::Partial;
        return;
    case Phase::Partial: {
        float before = 0.f, after = 0.f;
        if (!R::IsLiveByIndex(g_mine.actor, g_mine.idx) || !G::ReadProcess(g_mine.actor, before)) {
            Done("its own decal is gone before the partial -- FAIL");
            return;
        }
        if (!Clean(kPartialSub)) {
            Done("clean did not dispatch -- FAIL");
            return;
        }
        G::ReadProcess(g_mine.actor, after);
        UE_LOGI("[GRIME-DRILL] %s PARTIAL on its own decal: %.3f -> %.3f", Side(), before, after);
        if (after >= before - kEps) {
            Done("the partial clean did not lower its own decal -- FAIL");
            return;
        }
        g_phase = Phase::AwaitOtherFall;
        g_since = now;
        return;
    }
    case Phase::AwaitOtherFall: {
        float v = 0.f;
        if (!ReadOther(v)) {
            Done("the other peer's decal is gone on this copy -- FAIL");
            return;
        }
        if (v < g_other.start - kEps) {
            UE_LOGI("[GRIME-DRILL] %s sees the other peer's wipe: %.3f -> %.3f", Side(), g_other.start, v);
            g_phase = Phase::Destroy;
            return;
        }
        if (now - g_since > kPhaseBound) Done("the other peer's partial wipe never reached this copy -- FAIL");
        return;
    }
    case Phase::Destroy: {
        if (!R::IsLiveByIndex(g_mine.actor, g_mine.idx)) {
            Done("its own decal is gone before the destroy -- FAIL");
            return;
        }
        if (!Clean(kDestroySub)) {
            Done("clean did not dispatch -- FAIL");
            return;
        }
        const bool gone = !R::IsLiveByIndex(g_mine.actor, g_mine.idx);
        UE_LOGI("[GRIME-DRILL] %s DESTROY on its own decal: %s", Side(), gone ? "it ended its play" : "it still stands");
        if (!gone) {
            Done("a clean below zero did not destroy its own decal -- FAIL");
            return;
        }
        g_phase = Phase::AwaitOtherZero;
        g_since = now;
        return;
    }
    case Phase::AwaitOtherZero: {
        float v = 0.f;
        if (!ReadOther(v)) {
            Done("the other peer's decal was destroyed on this copy, where an apply only repaints -- FAIL");
            return;
        }
        if (v <= kEps) {
            Done("the other peer's decal reads zero and still stands on this copy -- PASS");
            return;
        }
        if (now - g_since > kPhaseBound) Done("the other peer's wipe to destruction never reached this copy -- FAIL");
        return;
    }
    case Phase::Done:
        return;
    }
}

void OnDisconnect() {
    g_mine = Decal{};
    g_other = Decal{};
    g_phase = Phase::Unpicked;
}

}  // namespace coop::dev::grime_drill
