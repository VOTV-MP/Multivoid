// coop/dev/bucket_drill.cpp -- [dev] a mop bucket's water and concrete, acted on by one player and logged on every
// peer (VOTVCOOP_BUCKET_DRILL=1 on every peer).
//
// The bucket is the save's prop_bucket_C under VOTVCOOP_BUCKET_KEY. HOST, VOTVCOOP_BUCKET_SETUP ms after it armed:
// spawns a cement bag and a trowel (prop_wallfixer_C) 1 m beside it. The ACTOR (VOTVCOOP_BUCKET_ACTOR, `client` =
// slot 1, or `host`), from VOTVCOOP_BUCKET_DELAY ms after it armed, a step every 8 s, as the game's own verbs:
//   fill     -- the actor stood 1.5 m from the bucket 5 s before; the water as the bucket's getFromWater leaves it from a
//               water volume (height 35)
//   dump     -- the bucket's left click aimed at no plant: getFromWater throws the water out
//   fill     -- again
//   concrete -- the cement bag's left click on the bucket: lib.replaceProp makes it a concrete bucket
//   take     -- the trowel's left click on the concrete bucket: one unit out
//   take2    -- 16 s later, the trowel emptied first: the bucket the host left 1 unit turns back into a mop bucket
// HOST, once a concrete bucket has stood 3 s after a take: its units made 1. EVERY PEER, every second: each mop bucket
// and concrete bucket within 6 m of the bucket's start, each cement bag and trowel there (`[BKT]`).

#include "coop/dev/bucket_drill.h"

#include "coop/cheats/item_catalog.h"
#include "coop/config/config.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/teleport_client.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_nav.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace coop::dev::bucket_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
using ue_wrap::FVector;

constexpr uint64_t kStepMs = 8000;

bool On() {
    static const bool on = coop::config::ReadEnv("VOTVCOOP_BUCKET_DRILL") == "1";
    return on;
}

uint64_t EnvMs(const char* name, uint64_t dflt) {
    const std::string v = coop::config::ReadEnv(name);
    return v.empty() ? dflt : std::strtoull(v.c_str(), nullptr, 10);
}

uint64_t Now() { return ::GetTickCount64(); }
uint64_t WallMs() {
    FILETIME ft{};
    ::GetSystemTimePreciseAsFileTime(&ft);
    return ((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000ull;
}

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
T Get(void* obj, const wchar_t* name, T dflt) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? *reinterpret_cast<const T*>(static_cast<const char*>(obj) + off) : dflt;
}

template <class T>
bool Put(void* obj, const wchar_t* name, T v) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return false;
    *reinterpret_cast<T*>(static_cast<char*>(obj) + off) = v;
    return true;
}

bool CallNoArgs(void* obj, const wchar_t* fn) {
    void* const f = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fn) : nullptr;
    ue_wrap::ParamFrame frame(f);
    return f && frame.valid() && ue_wrap::Call(obj, frame);
}

// The item's left click as the player's: the hit it reads written on `target` first (a held item's use breaks the trace).
bool HandUse(void* player, void* item, void* target) {
    FVector at{};
    if (!player || !item || !target || !E::TryGetActorLocation(target, at) ||
        !E::WriteMainPlayerHitResult(player, target, ue_wrap::prop::GetStaticMesh(target), at))
        return false;
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(item), L"playerHandUse_LMB");
    ue_wrap::ParamFrame f(fn);
    return fn && f.valid() && f.Set<void*>(L"player", player) && ue_wrap::Call(item, f);
}

float Dist2(const FVector& a, const FVector& b) {
    const float x = a.X - b.X, y = a.Y - b.Y, z = a.Z - b.Z;
    return x * x + y * y + z * z;
}

FVector g_anchor{};
bool g_anchored = false;

// The nearest live instance of `cls` within 6 m of the bucket's start; null when none.
void* Near(const wchar_t* cls) {
    void* best = nullptr;
    float bd = 600.f * 600.f;
    for (void* a : All(cls)) {
        FVector p{};
        if (!E::TryGetActorLocation(a, p)) continue;
        const float d = Dist2(p, g_anchor);
        if (d < bd) { bd = d; best = a; }
    }
    return best;
}

std::string Key(void* a) {
    const std::wstring k = ue_wrap::prop::GetInteractableKeyString(a);
    return std::string(k.begin(), k.end());
}

unsigned Eid(void* a) { return static_cast<unsigned>(coop::element::Registry::Get().EidForActor(a)); }

// The catalog's first row whose name holds `part` (case-insensitive); empty when none. Every row naming a bucket, cement
// or a fixer is said once.
std::wstring CatalogRow(const char* part) {
    const auto names = coop::cheats::item_catalog::Names();
    if (!names) return L"";
    static bool s_said = false;
    std::wstring found;
    for (const std::string& n : *names) {
        std::string low = n;
        for (char& c : low) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        if (!s_said && (low.find("bucket") != std::string::npos || low.find("cement") != std::string::npos ||
                        low.find("fixer") != std::string::npos || low.find("concrete") != std::string::npos))
            UE_LOGI("[BKT] catalog row '%s'", n.c_str());
        if (found.empty() && low.find(part) != std::string::npos) found.assign(n.begin(), n.end());
    }
    s_said = true;
    return found;
}

bool SpawnRow(const wchar_t* row, const FVector& at) {
    void* const gm = ue_wrap::world_singleton::Gamemode();
    ue_wrap::ParamFrame f(gm ? R::FindDispatchFunctionCached(R::ClassOf(gm), L"spawnPropThroughGamemode") : nullptr);
    if (!f.valid()) return false;
    const R::FName name = ue_wrap::fname_utils::StringToFName(row);
    ue_wrap::FTransform xf{};
    xf.TX = at.X;
    xf.TY = at.Y;
    xf.TZ = at.Z;
    return f.SetRaw(L"prop", &name, sizeof(name)) && f.SetRaw(L"InputPin", &xf, sizeof(xf)) &&
           f.Set<int32_t>(L"amount", 1) && ue_wrap::Call(gm, f);
}

void Report() {
    const unsigned long long w = WallMs();
    for (void* a : All(L"prop_bucket_C")) {
        FVector p{};
        if (!E::TryGetActorLocation(a, p) || Dist2(p, g_anchor) > 600.f * 600.f) continue;
        UE_LOGI("[BKT] wall=%llu bucket key=%s eid=%u height=%.2f soap=%d empty=%d alc=%d alcP=%.3f canConcrete=%d", w,
                Key(a).c_str(), Eid(a), Get<float>(a, L"Height", -1.f), Get<int32_t>(a, L"soap", -1),
                Get<uint8_t>(a, L"Empty", 9), Get<uint8_t>(a, L"alcohol", 9), Get<float>(a, L"alcoholP", -1.f),
                Get<uint8_t>(a, L"canConcrete", 9));
    }
    for (void* a : All(L"prop_concreteBucket_C")) {
        FVector p{};
        if (!E::TryGetActorLocation(a, p) || Dist2(p, g_anchor) > 600.f * 600.f) continue;
        UE_LOGI("[BKT] wall=%llu concrete key=%s eid=%u units=%d stage=%d dry=%d dryTimer=%.1f", w, Key(a).c_str(), Eid(a),
                Get<int32_t>(a, L"units", -1), Get<int32_t>(a, L"stage", -1), Get<uint8_t>(a, L"dry", 9),
                Get<float>(a, L"dryTimer", -1.f));
    }
    for (void* a : All(L"prop_cementBag_C")) {
        FVector p{};
        if (!E::TryGetActorLocation(a, p) || Dist2(p, g_anchor) > 600.f * 600.f) continue;
        UE_LOGI("[BKT] wall=%llu cementbag key=%s eid=%u units=%d", w, Key(a).c_str(), Eid(a), Get<int32_t>(a, L"units", -1));
    }
    for (void* a : All(L"prop_wallfixer_C")) {
        FVector p{};
        if (!E::TryGetActorLocation(a, p) || Dist2(p, g_anchor) > 600.f * 600.f) continue;
        UE_LOGI("[BKT] wall=%llu trowel key=%s eid=%u concrete=%d", w, Key(a).c_str(), Eid(a), Get<int32_t>(a, L"concrete", -1));
    }
}

uint64_t g_armedAt = 0, g_reportAt = 0, g_takenAt = 0;
int g_step = 0;
bool g_setup = false, g_hostLeftOne = false;

void Step(const char* what, bool ok) {
    UE_LOGI("[BKT] step %s ok=%d wall=%llu", what, ok ? 1 : 0, static_cast<unsigned long long>(WallMs()));
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_armedAt) {
        g_armedAt = Now();
        UE_LOGI("[BKT] drill armed role=%s", host ? "host" : "client");
        coop::cheats::item_catalog::RequestRefresh();
    }
    if (!g_anchored) {
        const std::string want = coop::config::ReadEnv("VOTVCOOP_BUCKET_KEY").empty()
                                     ? std::string("upAbhZM6weqmeddpJE6vHg")
                                     : coop::config::ReadEnv("VOTVCOOP_BUCKET_KEY");
        for (void* b : All(L"prop_bucket_C"))
            if (Key(b) == want && E::TryGetActorLocation(b, g_anchor)) {
                g_anchored = true;
                UE_LOGI("[BKT] the bucket %s is at (%.0f,%.0f,%.0f)", want.c_str(), g_anchor.X, g_anchor.Y, g_anchor.Z);
            }
        if (!g_anchored) return;
    }
    const uint64_t t = Now() - g_armedAt;
    void* const me = coop::players::Registry::Get().Local();

    if (host && !g_setup && t >= EnvMs("VOTVCOOP_BUCKET_SETUP", 120000)) {
        g_setup = true;
        const std::wstring bagRow = CatalogRow("cement"), fixerRow = CatalogRow("fixer");
        const bool bag = !bagRow.empty() && SpawnRow(bagRow.c_str(), {g_anchor.X + 100.f, g_anchor.Y, g_anchor.Z + 30.f});
        const bool fixer =
            !fixerRow.empty() && SpawnRow(fixerRow.c_str(), {g_anchor.X - 100.f, g_anchor.Y, g_anchor.Z + 30.f});
        UE_LOGI("[BKT] host: spawned a cement bag '%ls' (ran=%d) and a trowel '%ls' (ran=%d)", bagRow.c_str(), bag ? 1 : 0,
                fixerRow.c_str(), fixer ? 1 : 0);
    }
    // HOST: a concrete bucket a take has stood 3 s after is left one unit, so the second take empties it.
    if (host && !g_hostLeftOne && g_takenAt && Now() - g_takenAt >= 3000) {
        if (void* c = Near(L"prop_concreteBucket_C")) {
            g_hostLeftOne = true;
            Put<int32_t>(c, L"units", 1);
            const bool ok = CallNoArgs(c, L"updStage");
            UE_LOGI("[BKT] host: the concrete bucket %s left 1 unit (upd ran=%d)", Key(c).c_str(), ok ? 1 : 0);
        }
    }
    if (host && !g_takenAt) {
        static int s_units = -1;
        if (void* c = Near(L"prop_concreteBucket_C")) {
            const int u = Get<int32_t>(c, L"units", -1);
            if (s_units > 0 && u >= 0 && u < s_units) g_takenAt = Now();
            s_units = u;
        }
    }

    const std::string actor = coop::config::ReadEnv("VOTVCOOP_BUCKET_ACTOR");
    const bool acts = (actor == "host") ? host : (!host && coop::players::Registry::Get().LocalPeerId() == 1);
    const uint64_t d = EnvMs("VOTVCOOP_BUCKET_DELAY", 60000);
    static bool s_placed = false;
    if (acts && me && !s_placed && t + 5000 >= d) {   // 5 s before the first step: beside the bucket, within the host's reach
        s_placed = true;
        // Onto the NavMesh beside it, as the cheat teleport lands (a body put in geometry is taken out of bounds).
        FVector floor{};
        const float half = E::GetActorCharacterHalfHeight(me);
        coop::teleport_client::ApplyArgs args{};
        const bool onNav = E::ProjectToNav(me, {g_anchor.X + 150.f, g_anchor.Y, g_anchor.Z}, {150.f, 150.f, 400.f}, &floor);
        args.locX = floor.X;
        args.locY = floor.Y;
        args.locZ = floor.Z + half;
        const bool moved = onNav && half > 0.f && coop::teleport_client::ApplyLocally(args);
        UE_LOGI("[BKT] the actor stands beside the bucket (moved=%d)", moved ? 1 : 0);
    }
    if (acts && me && g_step < 6 && t >= d + kStepMs * static_cast<uint64_t>(g_step == 5 ? 6 : g_step)) {
        void* const bucket = Near(L"prop_bucket_C");
        switch (g_step) {
        case 0:
        case 2:
            Step("fill", bucket && Put<float>(bucket, L"Height", 35.f) && CallNoArgs(bucket, L"upd"));
            break;
        case 1:
            Step("dump", bucket && HandUse(me, bucket, bucket));
            break;
        case 3:
            Step("concrete", bucket && HandUse(me, Near(L"prop_cementBag_C"), bucket));
            break;
        case 4:
            Step("take", HandUse(me, Near(L"prop_wallfixer_C"), Near(L"prop_concreteBucket_C")));
            break;
        case 5: {
            void* const fixer = Near(L"prop_wallfixer_C");
            Put<int32_t>(fixer, L"concrete", 0);
            Step("take2", HandUse(me, fixer, Near(L"prop_concreteBucket_C")));
            UE_LOGI("[BKT] DONE wall=%llu", static_cast<unsigned long long>(WallMs()));
            break;
        }
        }
        ++g_step;
    }
    if (Now() < g_reportAt) return;
    g_reportAt = Now() + 1000;
    Report();
}

}  // namespace coop::dev::bucket_drill
