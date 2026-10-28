// coop/interactables/coffee_state.cpp -- see coop/interactables/coffee_state.h.

#include "coop/interactables/coffee_state.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/props/prop_record_gates.h"
#include "coop/props/prop_save_data.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/coffee.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::coffee_state {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace CF = ue_wrap::coffee;

// The two bodies that change the maker's flags, declared on its class: the brew, and the hit handler that is the
// fill. Another class's function of either name is never asked.
UE_FACT_FN(kRowBrew, "coffee_state", L"prop_coffeeMaker_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowHit, "coffee_state", L"prop_coffeeMaker_C",
           L"BndEvt__StaticMesh_K2Node_ComponentBoundEvent_0_ComponentHitSignature__DelegateSignature", Declared);

constexpr int kTagBrew = 0x43534250;  // 'CSBP'
constexpr int kTagHit  = 0x43534854;  // 'CSHT'

constexpr uint64_t kRefusalSayMs = 10000;

std::atomic<coop::net::Session*> g_session{nullptr};
// Each watch registers once; the set settles once both are live, or once no name is left to resolve.
bool g_brewWatched = false, g_hitWatched = false, g_settled = false, g_gatesRegistered = false;

// Each maker's cancelled hits, for the bounded line: the count so far, when the next line may be said, and the count
// at the last line said. Sized by the makers a session has seen, so the steady state allocates nothing.
struct CancelSay {
    void*    maker;
    uint64_t n;
    uint64_t nextMs;
    uint64_t lastN;
};
std::vector<CancelSay> g_cancelSay;

uint64_t g_cancelled = 0, g_madeMaker = 0, g_madeBag = 0, g_recordsRefused = 0, g_publishFailed = 0;
// The other actor of the newest cancelled hit, for the drill's read only.
ue_wrap::CachedObjRef g_lastOther;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

coop::net::Session* HostSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->role() == coop::net::Role::Host) ? s : nullptr;
}

// ---- the host's notes ------------------------------------------------------------------------
// What the maker (and a bag that touched it) read before a body ran, kept for the POST of the same call. Each
// body has its own note, and neither nests: the hit handler is one dispatch of the physics scene, and the brew is
// one press. A POST with no note of its own call publishes nothing.
struct Note {
    void*                 object = nullptr;
    CF::MakerFlags        flags;
    ue_wrap::CachedObjRef bag;  // the hit handler's other actor, when it is a coffee bag
    int32_t               amounts = 0;
};
Note g_brewNote, g_hitNote;

// Publishes `actor`'s record. A publish the transport refused is registered for a re-send by prop_save_data; one
// that could not be made at all (no peer ready, the capture failed) is counted and said.
void PublishRecord(coop::net::Session* s, void* actor, const char* what, uint64_t& made) {
    const std::wstring key = ue_wrap::prop::GetKeyString(actor);
    if (key.empty() || key == L"None" || !coop::prop_save_data::Publish(s, actor, key)) {
        ++g_publishFailed;
        UE_LOGW("[COFFEE-STATE] the %s's record on key='%ls' was not published (no peer ready, or the capture "
                "failed)", what, key.c_str());
        return;
    }
    ++made;
}

// The client's cancelled hit on `maker`, said for the first three of that maker and then once in kRefusalSayMs
// with the count since the last line: the hit runs at contact rate, so a count-based bound still said twice a second.
void SayCancelled(void* maker) {
    CancelSay* c = nullptr;
    for (CancelSay& x : g_cancelSay)
        if (x.maker == maker) { c = &x; break; }
    if (!c) {
        g_cancelSay.push_back({maker, 0, 0, 0});
        c = &g_cancelSay.back();
    }
    ++c->n;
    if (c->n > 3 && NowMs() < c->nextMs) return;
    UE_LOGI("[COFFEE-STATE] a client's hit on maker %p is cancelled -- the fill is the host's (%llu since the last "
            "line, %llu in all)", maker, static_cast<unsigned long long>(c->n - c->lastN),
            static_cast<unsigned long long>(c->n));
    c->lastN = c->n;
    c->nextMs = NowMs() + kRefusalSayMs;
}

// ---- the brew --------------------------------------------------------------------------------
sg::Verdict OnBrewPre(const sg::Call& call) {
    coop::net::Session* s = HostSession();
    if (!s || !call.object) return sg::Verdict::Run;
    static R::ParamOffset s_action{L"action"};
    const int32_t off = s_action.Of(call.function);
    g_brewNote.object = nullptr;
    if (off < 0 || *(call.locals + off) != CF::kBrewAction) return sg::Verdict::Run;
    if (!CF::ReadMakerFlags(call.object, g_brewNote.flags)) return sg::Verdict::Run;
    g_brewNote.object = call.object;
    return sg::Verdict::Run;
}

void OnBrewPost(const sg::Call& call) {
    coop::net::Session* s = HostSession();
    if (!s || g_brewNote.object != call.object) return;
    g_brewNote.object = nullptr;
    // The coffee drill's red arm: a brew publishes nothing. Resolved once.
    static const bool s_redNoPublish =
        coop::config::ResolveEnum(coop::config_registry::rows::coffee_red) == "no_publish";
    if (s_redNoPublish) return;
    CF::MakerFlags now;
    // A brew sets and clears the three flags inside the one call, so the record that crosses is the final one.
    if (CF::ReadMakerFlags(call.object, now) && !(now == g_brewNote.flags))
        PublishRecord(s, call.object, "maker", g_madeMaker);
}

// ---- the fill --------------------------------------------------------------------------------
// The drill's tally of the hit handler's runs by the class of the actor that touched the maker. The class name is
// resolved the first time a class is seen; only the drill's runs fill it.
struct ClassTally {
    void*        cls;
    std::wstring name;
    uint64_t     n;
};
std::vector<ClassTally> g_tally;
uint64_t g_tallyHits = 0;

void TallyOther(void* other) {
    ++g_tallyHits;
    void* cls = other ? R::ClassOf(other) : nullptr;
    for (ClassTally& t : g_tally)
        if (t.cls == cls) { ++t.n; return; }
    g_tally.push_back({cls, cls ? R::ClassNameOf(other) : std::wstring(L"none"), 1});
}

sg::Verdict OnHitPre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !call.object) return sg::Verdict::Run;
    static R::ParamOffset s_other{L"OtherActor"};
    // The coffee drill's reads of what touches the maker, on both roles; with the drill off the client's path is
    // a role test and a counter. Resolved once.
    static const bool s_drill = coop::config::ResolveFlag(coop::config_registry::rows::coffee_drill);
    if (s_drill && (s->role() == coop::net::Role::Client || s->role() == coop::net::Role::Host)) {
        const int32_t offDrill = s_other.Of(call.function);
        void* seen = offDrill >= 0 ? *reinterpret_cast<void* const*>(call.locals + offDrill) : nullptr;
        TallyOther(seen);
        if (s->role() == coop::net::Role::Client && seen != g_lastOther.Raw()) g_lastOther.Set(seen);
    }
    if (s->role() == coop::net::Role::Client) {
        // The coffee drill's red arm: this copy's own run of the hit handler is not cancelled. Resolved once.
        static const bool s_redNoCancel =
            coop::config::ResolveEnum(coop::config_registry::rows::coffee_red) == "no_cancel";
        if (s_redNoCancel) return sg::Verdict::Run;
        // The fill is the host's: this copy's run would destroy its mirror of the cup and set flags no record
        // carries. The cup's destroy and the maker's record come from the host. Contact rate: counted, and said
        // bounded by time.
        ++g_cancelled;
        SayCancelled(call.object);
        return sg::Verdict::Cancel;
    }
    if (s->role() != coop::net::Role::Host) return sg::Verdict::Run;
    g_hitNote.object = nullptr;
    if (!CF::ReadMakerFlags(call.object, g_hitNote.flags)) return sg::Verdict::Run;
    g_hitNote.bag.Reset();
    const int32_t off = s_other.Of(call.function);
    void* other = off >= 0 ? *reinterpret_cast<void* const*>(call.locals + off) : nullptr;
    if (CF::IsBag(other) && CF::ReadBagAmounts(other, g_hitNote.amounts)) g_hitNote.bag.Set(other);
    g_hitNote.object = call.object;
    return sg::Verdict::Run;
}

void OnHitPost(const sg::Call& call) {
    coop::net::Session* s = HostSession();
    if (!s || g_hitNote.object != call.object) return;
    g_hitNote.object = nullptr;
    CF::MakerFlags now;
    if (CF::ReadMakerFlags(call.object, now) && !(now == g_hitNote.flags))
        PublishRecord(s, call.object, "maker", g_madeMaker);
    void* bag = g_hitNote.bag.Get();
    g_hitNote.bag.Reset();
    int32_t amounts = 0;
    if (bag && CF::ReadBagAmounts(bag, amounts) && amounts != g_hitNote.amounts)
        PublishRecord(s, bag, "bag", g_madeBag);
}

// ---- the record gate -------------------------------------------------------------------------
// The host takes a client's record for a maker or a bag only when it is the record behind that client's own drop
// (coop/props/prop_drop_intent awaits it); a forged flag or a refilled bag is any other record. A refusal is
// final for that record, so it is counted once.
bool AcceptsClientRecord(void* actor, uint8_t senderSlot) {
    const std::wstring key = ue_wrap::prop::GetKeyString(actor);
    if (!key.empty() && coop::prop_save_data::AwaitsRecordFrom(key, senderSlot)) return true;
    ++g_recordsRefused;
    static uint64_t s_nextSayMs = 0;
    static uint64_t s_since = 0;
    const uint64_t now = NowMs();
    if (now < s_nextSayMs) { ++s_since; return false; }
    UE_LOGW("[COFFEE-STATE] slot %u's record for key='%ls' is refused -- it is no drop's own (%llu more since "
            "the last line)", static_cast<unsigned>(senderSlot), key.c_str(),
            static_cast<unsigned long long>(s_since));
    s_nextSayMs = now + kRefusalSayMs;
    s_since = 0;
    return false;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_brewWatched) g_brewWatched = sg::WatchName(kRowBrew, kTagBrew, &OnBrewPre, &OnBrewPost);
    if (!g_hitWatched) g_hitWatched = sg::WatchName(kRowHit, kTagHit, &OnHitPre, &OnHitPost);
    if (!g_gatesRegistered) {
        g_gatesRegistered = true;
        coop::prop_record_gates::RegisterIntentGate(L"prop_coffeeMaker_C", &AcceptsClientRecord);
        coop::prop_record_gates::RegisterIntentGate(L"prop_coffeeBag_C", &AcceptsClientRecord);
    }
}

void Tick() {
    sg::ResolvePendingNames();
    if (g_settled || !g_brewWatched || !g_hitWatched) return;
    if (sg::NameWatchLive(kRowBrew, kTagBrew) && sg::NameWatchLive(kRowHit, kTagHit)) {
        g_settled = true;
        UE_LOGI("[COFFEE-STATE] the maker's brew and fill gates are live");
    } else if (sg::PendingNameCount() == 0) {
        g_settled = true;
        UE_LOGE("[COFFEE-STATE] a maker gate is dead (resolved into a full table, or its class declares no such "
                "body) -- the maker's state crosses only where a press was made");
    }
}

void OnDisconnect() {
    if (g_cancelled || g_madeMaker || g_madeBag || g_recordsRefused || g_publishFailed)
        UE_LOGI("[COFFEE-STATE] session end -- client hits cancelled=%llu maker records=%llu bag records=%llu "
                "client records refused=%llu publishes failed=%llu",
                static_cast<unsigned long long>(g_cancelled), static_cast<unsigned long long>(g_madeMaker),
                static_cast<unsigned long long>(g_madeBag), static_cast<unsigned long long>(g_recordsRefused),
                static_cast<unsigned long long>(g_publishFailed));
    g_cancelled = g_madeMaker = g_madeBag = g_recordsRefused = g_publishFailed = 0;
    g_brewNote = Note{};
    g_hitNote = Note{};
    g_lastOther.Reset();
    g_cancelSay.clear();
    g_tally.clear();
    g_tallyHits = 0;
}

DrillView ReadForDrill() {
    DrillView v;
    v.cancelled = g_cancelled;
    v.hits = g_tallyHits;
    v.madeMaker = g_madeMaker;
    v.recordsRefused = g_recordsRefused;
    for (const ClassTally& t : g_tally) v.byClass.push_back({t.name, t.n});
    void* other = g_lastOther.Get();
    if (other && ue_wrap::prop::IsDescendantOfProp(other)) v.lastOtherKey = ue_wrap::prop::GetKeyString(other);
    return v;
}

}  // namespace coop::coffee_state
