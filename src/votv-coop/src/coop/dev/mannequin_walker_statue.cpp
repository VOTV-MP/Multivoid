// coop/dev/mannequin_walker_statue.cpp -- see coop/dev/mannequin_walker_statue.h.

#include "coop/dev/mannequin_walker_statue.h"

#include "harness/autotest/peer_signal.h"

#include "coop/dev/mannequin_walker_geometry.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace coop::dev::mannequin_walker_drill::statue {
namespace {

namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace PS = harness::autotest::peer_signal;
namespace R  = ue_wrap::reflection;
using ue_wrap::FVector;

// The ticker's walker-or-statue gate reads the day number (timeZ.Z) at or above this.
constexpr int32_t kStatueDay = 14;
// From the host's call of the ticker to the stand gone, the broken prop standing with a key, and the walker born: the ticker's own
// Delay(0.5) and the spawn points that do not refuse (a point the host's camera sees refuses).
constexpr int kRouteBoundS = 20;
// The client's wait for the copies of what the host swapped, and each peer's wait on the other's mark.
constexpr int kCopyBoundS = 30;
constexpr int kPeerWaitS = 90;
// A prop_C within this of the stand's place is the broken mannequin the ticker left, when it is new since before the tick.
constexpr float kBrokenNearCm = 150.f;

constexpr const wchar_t* kStatueClass = L"prop_mannequin_C";
constexpr const wchar_t* kPropClass = L"prop_C";
constexpr const wchar_t* kTickerClass = L"ticker_mannequinSpawner_C";

struct Statue {
    std::string key;
    FVector     at{};
};

// Every live instance of exactly `cls` (not the default object).
std::vector<void*> AllOf(const wchar_t* cls) {
    std::vector<void*> out;
    if (void* c = OI::ClassByName(cls))
        OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
            if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &out);
    return out;
}

std::string KeyOfActor(void* actor) { return Narrow(ue_wrap::prop::GetInteractableKeyString(actor)); }

std::vector<Statue> Statues() {
    std::vector<Statue> out;
    for (void* s : AllOf(kStatueClass)) {
        Statue st;
        st.key = KeyOfActor(s);
        E::TryGetActorLocation(s, st.at);
        out.push_back(st);
    }
    return out;
}

bool StatueStands(const std::string& key) {
    for (void* s : AllOf(kStatueClass))
        if (KeyOfActor(s) == key) return true;
    return false;
}

void* PropByKey(const std::string& key) {
    for (void* p : AllOf(kPropClass))
        if (KeyOfActor(p) == key) return p;
    return nullptr;
}

// A prop_C that was not there before the tick, within kBrokenNearCm of `at`: the broken mannequin the ticker leaves.
void* NewBrokenNear(const std::vector<void*>& before, const FVector& at) {
    for (void* p : AllOf(kPropClass)) {
        if (std::find(before.begin(), before.end(), p) != before.end()) continue;
        FVector pa{};
        if (E::TryGetActorLocation(p, pa) && Dist3(pa, at) < kBrokenNearCm) return p;
    }
    return nullptr;
}

template <class T>
T* Field(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off < 0 ? nullptr : reinterpret_cast<T*>(static_cast<uint8_t*>(obj) + off);
}

// What the forced roll overwrote, to write back.
struct Forced {
    int32_t* day = nullptr;
    float*   weight = nullptr;
    int32_t  dayWas = 0;
    float    weightWas = 0.f;
};

// The ticker's own tick with its gate met and its roll certain: the day number it reads and the statue route's weight, written
// where its body reads them, then its ReceiveTick. False when a field or the function did not resolve (nothing was run).
bool RunStatueTick(void* ticker, Forced& f) {
    void** gm = Field<void*>(ticker, L"gamemode");
    if (!gm || !*gm) return false;
    f.weight = Field<float>(*gm, L"mannDest");
    void** cycle = Field<void*>(*gm, L"daynightCycle");
    int32_t* timeZ = cycle && *cycle ? Field<int32_t>(*cycle, L"timeZ") : nullptr;   // X hour, Y minute, Z day number
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(ticker), L"ReceiveTick");
    if (!f.weight || !timeZ || !fn) return false;
    ue_wrap::ParamFrame frame(fn);
    if (!frame.valid()) return false;
    f.day = &timeZ[2];
    f.dayWas = *f.day;
    f.weightWas = *f.weight;
    if (*f.day < kStatueDay) *f.day = kStatueDay;
    *f.weight = 1.f;
    frame.Set<float>(L"DeltaSeconds", 0.016f);
    const bool ran = ue_wrap::Call(ticker, frame);
    *f.day = f.dayWas;
    *f.weight = f.weightWas;
    return ran;
}

std::vector<std::string> Words(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}

// ---- the host ----

bool HostRoute(std::string& goneKey, std::string& brokenKey, std::string& walkerKey, FVector& goneAt) {
    std::vector<Statue> before;
    std::vector<void*>  propsBefore, walkersBefore;
    void*                 ticker = nullptr;
    ue_wrap::CachedObjRef tickerRef;  // held across the client's wait: read through its slot, never through the pointer
    OnGameThread([&] {
        before = Statues();
        propsBefore = AllOf(kPropClass);
        walkersBefore = Walkers();
        const std::vector<void*> t = AllOf(kTickerClass);
        ticker = t.empty() ? nullptr : t.front();
        tickerRef.Set(ticker);
    });
    if (!ticker) return Invalid("step 8: this world has no mannequin spawner ticker to run");
    if (before.empty()) return Invalid("step 8: this world holds no mannequin statue: the ticker's statue route has nothing to swap");
    std::string keys;
    for (const Statue& s : before) {
        if (s.key.empty()) return Invalid("step 8: a mannequin statue has no key: the prop lane has nothing to cross it by");
        keys += s.key + " ";
    }
    UE_LOGI("mannequin-drill: step 8: %zu statue(s), %zu walker(s), ticker %p", before.size(), walkersBefore.size(), ticker);
    PS::Mark("statue_go", keys);
    std::string snap;
    if (!AwaitClient("statue_snap", kPeerWaitS, &snap)) return false;
    UE_LOGI("mannequin-drill: step 8: the client held %s of the host's statues before the swap", snap.c_str());

    Forced forced;
    bool   ran = false;
    OnGameThread([&] {
        void* const live = tickerRef.Get();
        ran = live && RunStatueTick(live, forced);
    });
    if (!ran) return Invalid("step 8: the ticker is gone, or its day number, roll weight or tick did not read: the statue route did not run");

    void*   walker = nullptr;
    void*   broken = nullptr;
    Statue  gone;
    const bool done = WaitGT("the ticker's statue route ends: a stand gone, the broken prop and a walker", kRouteBoundS, [&] {
        if (gone.key.empty())
            for (const Statue& s : before)
                if (!StatueStands(s.key)) gone = s;
        if (gone.key.empty()) return false;
        if (!walker) {
            for (void* w : Walkers())
                if (std::find(walkersBefore.begin(), walkersBefore.end(), w) == walkersBefore.end()) walker = w;
        }
        if (!broken) broken = NewBrokenNear(propsBefore, gone.at);
        return walker && broken && !KeyOf(walker).empty() && !KeyOfActor(broken).empty();
    }, false);
    if (!done) {
        char why[240];
        std::snprintf(why, sizeof(why), "step 8: the ticker's statue route gave %s%s%s within %d s of its tick (the game's own route, run on "
                      "the host: not a lane's failure)", gone.key.empty() ? "no stand gone " : "a stand gone ",
                      broken ? "and the broken prop " : "and no broken prop ", walker ? "and a walker" : "and no walker", kRouteBoundS);
        return Invalid(why);
    }
    OnGameThread([&] {
        goneKey = gone.key;
        goneAt = gone.at;
        brokenKey = KeyOfActor(broken);
        walkerKey = KeyOf(walker);
    });
    UE_LOGI("mannequin-drill: step 8: the ticker swapped statue '%s' at (%.0f,%.0f,%.0f) for broken prop '%s' and spawned walker '%s'",
            goneKey.c_str(), goneAt.X, goneAt.Y, goneAt.Z, brokenKey.c_str(), walkerKey.c_str());
    char body[256];
    std::snprintf(body, sizeof(body), "%s %s %s", goneKey.c_str(), brokenKey.c_str(), walkerKey.c_str());
    PS::Mark("statue_swap", body);
    return true;
}

}  // namespace

bool Host() {
    std::string goneKey, brokenKey, walkerKey;
    FVector     goneAt{};
    if (!HostRoute(goneKey, brokenKey, walkerKey, goneAt)) return false;
    std::string word;
    if (!AwaitClient("statue_done", kCopyBoundS + kPeerWaitS, &word)) return false;
    char what[300];
    std::snprintf(what, sizeof(what), "the ticker's statue route crosses: the client lost the stand '%s' and gained the broken prop '%s' "
                  "and the walker '%s' (%s)", goneKey.c_str(), brokenKey.c_str(), walkerKey.c_str(), word.c_str());
    Check(true, "8", what);
    return true;
}

// ---- the client ----

bool Client() {
    std::string keysText;
    if (!PS::AwaitText("statue_go", keysText, kPeerWaitS)) return false;
    // The host's statues this client holds a copy of, before the host's tick.
    const std::vector<std::string> hostKeys = Words(keysText);
    std::vector<std::string>       held;
    OnGameThread([&] {
        for (const std::string& k : hostKeys)
            if (StatueStands(k)) held.push_back(k);
    });
    char word[64];
    std::snprintf(word, sizeof(word), "%zu of %zu", held.size(), hostKeys.size());
    PS::Mark("statue_snap", word);

    std::string swap;
    if (!PS::AwaitText("statue_swap", swap, kPeerWaitS)) return false;
    const std::vector<std::string> parts = Words(swap);
    if (parts.size() != 3) return Invalid("step 8: the host's swap mark is not three keys");
    const std::string &goneKey = parts[0], &brokenKey = parts[1], &walkerKey = parts[2];

    bool standGone = false, broken = false, walker = false;
    WaitGT("the swap reaches this client: the stand gone, the broken prop and the walker standing", kCopyBoundS, [&] {
        standGone = standGone || !StatueStands(goneKey);
        broken = broken || PropByKey(brokenKey) != nullptr;
        walker = walker || ByKey(walkerKey) != nullptr;
        return standGone && broken && walker;
    }, false);

    char what[240];
    const bool existed = std::find(held.begin(), held.end(), goneKey) != held.end();
    std::snprintf(what, sizeof(what), "this client held a copy of the stand '%s' the host's ticker swapped, before the swap (%zu of %zu "
                  "of the host's statues)", goneKey.c_str(), held.size(), hostKeys.size());
    if (!Check(existed, "8", what)) return FailedToHost("statue_done", "8", what);
    std::snprintf(what, sizeof(what), "the stand's copy '%s' is gone here after the swap (within %d s)", goneKey.c_str(), kCopyBoundS);
    if (!Check(standGone, "8", what)) return FailedToHost("statue_done", "8", what);
    std::snprintf(what, sizeof(what), "the broken mannequin prop '%s' has a copy here", brokenKey.c_str());
    if (!Check(broken, "8", what)) return FailedToHost("statue_done", "8", what);
    std::snprintf(what, sizeof(what), "the walker '%s' the swap spawned has a copy here", walkerKey.c_str());
    if (!Check(walker, "8", what)) return FailedToHost("statue_done", "8", what);
    PS::Mark("statue_done", "ok");
    return true;
}

}  // namespace coop::dev::mannequin_walker_drill::statue
