// coop/dev/throw_drill.cpp -- see coop/dev/throw_drill.h.

#include "coop/dev/throw_drill.h"

#include "drive_drill_verbs.h"  // Hold, Throw: the player's own pickup and throw (co-located private header)

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/end_reason.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/props/container_birth.h"
#include "coop/props/container_slice_wire.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::throw_drill {
namespace {

namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace ci = ue_wrap::container_inventory;
namespace cw = coop::props::container_slice_wire;
namespace cb = coop::props::container_birth;

constexpr const wchar_t* kPackPath  = L"/Game/objects/prop_backpack.prop_backpack_C";
constexpr const wchar_t* kPackClass = L"prop_backpack_C";
constexpr const wchar_t* kItemClass = L"prop_drive_C";
constexpr int      kItems         = 2;
constexpr float    kAheadCm       = 100.f;
constexpr float    kDropCm        = 30.f;
constexpr float    kNearCm        = 400.f;    // CLIENT: the fixture's mirror is this near its player
constexpr uint64_t kFillBoundMs   = 5000;     // HOST: the spawn to the backpack's own inventory binding its slot
constexpr uint64_t kCopyBoundMs   = 90000;    // HOST: the fixture to its copy (the client's find, hold and throw)
constexpr uint64_t kSliceBoundMs  = 35000;    // HOST: the copy to its contents, past the transfer's 30 s
constexpr uint64_t kGoneBoundMs   = 15000;    // HOST: the kick to the transfer ending with its author
constexpr uint64_t kFindBoundMs   = 60000;    // CLIENT: world-ready to the fixture holding its records
constexpr uint64_t kSettleBoundMs = 10000;
constexpr uint64_t kCheckEveryMs  = 250;
constexpr int      kRestChecks    = 4;

enum class Step : uint8_t { Arm, Fill, Copy, Slice, Gone, Find, Settle, Throw, Done };
Step     g_step = Step::Arm;
uint64_t g_stepMs = 0;
uint64_t g_nextCheckMs = 0;
int      g_session = 1;
ue_wrap::CachedObjRef g_fixture;   // this peer's backpack before the throw
ue_wrap::CachedObjRef g_copy;      // HOST: its copy the throw birthed
uint64_t g_digest = 0;             // HOST: the fixture's contents, hashed without an eid
uint32_t g_expired0 = 0, g_gone0 = 0;
ue_wrap::FVector g_lastAt{};
int      g_restReads = 0;

const std::string& Mode() {
    static const std::string s = coop::config::ResolveString(::coop::config_registry::rows::throw_drill);
    return s;
}
bool Enabled() { return Mode() == "run" || Mode() == "red" || Mode() == "gone"; }
void Next(Step s) { g_step = s; g_stepMs = ::GetTickCount64(); }
bool Expired(uint64_t bound) { return ::GetTickCount64() - g_stepMs > bound; }
bool Due() {
    const uint64_t now = ::GetTickCount64();
    if (now < g_nextCheckMs) return false;
    g_nextCheckMs = now + kCheckEveryMs;
    return true;
}
void Fail(const char* what) {
    UE_LOGW("[THROW-DRILL] FAIL in session %d: %s", g_session, what);
    g_step = Step::Done;
}
void Abandon(const char* why) {
    UE_LOGW("[THROW-DRILL] ABANDONED in session %d: %s", g_session, why);
    g_step = Step::Done;
}

// A container's records and their digest, hashed with no eid so a copy compares to its original. False unread.
bool Contents(void* container, size_t* count, uint64_t* digest) {
    void* inv = container ? ci::InventoryOf(container) : nullptr;
    std::vector<ue_wrap::save_record::SaveRecord> recs;
    if (!inv || !ci::ReadContents(inv, recs, cw::kMaxRecords)) return false;
    *count = recs.size();
    *digest = cw::ContentHash(0, recs);
    return true;
}

// The inventory's own put-in, as the player's hand puts an item in. True when it took the item.
bool AddObject(void* container, void* item) {
    void* inv = ci::InventoryOf(container);
    void* fn = inv ? R::FindDispatchFunctionCached(R::ClassOf(inv), L"addObject") : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || !f.Set<void*>(L"actor", item) || !f.Set<int32_t>(L"insertIndex1", -1) ||
        !ue_wrap::Call(inv, f))
        return false;
    return f.Get<bool>(L"return");
}

// HOST: the backpacks standing when the fixture was filled, held by slot and serial, so a copy born at a
// destroyed one's address still reads as new.
std::vector<ue_wrap::CachedObjRef> g_before;

bool IsDefault(void* obj) { return R::NameStartsWith(R::NameOf(obj), L"Default__"); }

void SnapshotPacks(void* cls) {
    g_before.clear();
    ue_wrap::object_index::ForEachInstance(cls, [](void*, void* obj, int32_t) {
        if (IsDefault(obj)) return;
        g_before.emplace_back();
        g_before.back().Set(obj);
    }, nullptr);
}

// HOST: a backpack born since the snapshot that is a world container: the copy the throw births.
void* NewCopy(void* cls) {
    void* found = nullptr;
    ue_wrap::object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
        auto* out = static_cast<void**>(c);
        if (*out || IsDefault(obj) || !ci::IsContainer(obj)) return;
        for (const ue_wrap::CachedObjRef& b : g_before)
            if (b.Is(obj)) return;
        void* inv = ci::InventoryOf(obj);
        if (inv && ci::IsWorldInventory(inv)) *out = obj;
    }, &found);
    return found;
}

// CLIENT: the backpack within kNearCm of `from` holding the host's records, the nearest of them.
struct Near { ue_wrap::FVector from; void* best; float bestCm; };
void* NearestFixture(void* cls, const ue_wrap::FVector& from) {
    Near ctx{from, nullptr, kNearCm};
    ue_wrap::object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
        auto* n = static_cast<Near*>(c);
        ue_wrap::FVector at{};
        size_t count = 0;
        uint64_t digest = 0;
        if (!R::IsLive(obj) || !E::TryGetActorLocation(obj, at)) return;
        const float cm = std::hypot(at.X - n->from.X, at.Y - n->from.Y);
        if (cm < n->bestCm && Contents(obj, &count, &digest) && count == static_cast<size_t>(kItems)) {
            n->best = obj;
            n->bestCm = cm;
        }
    }, &ctx);
    return ctx.best;
}

void HostTick(coop::net::Session* s) {
    if (g_step == Step::Done || !Due()) return;
    switch (g_step) {
    case Step::Arm: {
        // gone waits for the observer too: the kick must leave a peer in the session (the rig starts it after slot 1).
        if (!s->IsSlotWorldReady(1) || (Mode() == "gone" && !s->IsSlotWorldReady(2))) return;
        void* puppet = coop::puppet_drive::Puppet(1).GetActor();
        ue_wrap::FVector at{};
        if (!puppet || !R::IsLive(puppet) || !E::TryGetActorLocation(puppet, at)) return;
        void* packCls = ue_wrap::asset_load::LoadObjectByPath(kPackPath);
        if (!packCls) { Abandon("the backpack's class does not load"); return; }
        const ue_wrap::FVector fwd = E::GetActorForwardVector(puppet);
        const ue_wrap::FVector put{at.X + fwd.X * kAheadCm, at.Y + fwd.Y * kAheadCm, at.Z + kDropCm};
        void* pack = E::SpawnActor(packCls, put);
        if (!pack || !ci::IsContainer(pack)) { Abandon("the backpack did not spawn as a container"); return; }
        g_fixture.Set(pack);
        Next(Step::Fill);
        return;
    }
    case Step::Fill: {   // its inventory's own init binds the save slot's contents first; addObject indexes it
        void* pack = g_fixture.Get();
        void* inv = pack ? ci::InventoryOf(pack) : nullptr;
        if (!pack) { Abandon("the backpack died"); return; }
        if (!inv || !ci::ContentsSlot(inv)) {
            if (Expired(kFillBoundMs)) Abandon("the backpack's inventory never bound its contents slot");
            return;
        }
        void* itemCls = R::FindClass(kItemClass);
        ue_wrap::FVector put{};
        if (!itemCls || !E::TryGetActorLocation(pack, put)) { Abandon("the drive's class or the backpack's place is unread"); return; }
        int added = 0;
        for (int i = 0; i < kItems; ++i) {
            void* item = E::SpawnActor(itemCls, {put.X, put.Y, put.Z + 60.f + 30.f * i});
            if (item && AddObject(pack, item)) ++added;
        }
        size_t n = 0;
        if (added != kItems || !Contents(pack, &n, &g_digest) || n != static_cast<size_t>(kItems)) {
            Abandon("the backpack did not take the two drives");
            return;
        }
        g_expired0 = cb::EndedExpired();
        g_gone0 = cb::EndedGone();
        SnapshotPacks(ue_wrap::object_index::ClassByName(kPackClass));
        UE_LOGI("[THROW-DRILL] host (%s): a backpack holding %zu drives before the client's puppet, digest %016llx",
                Mode().c_str(), n, static_cast<unsigned long long>(g_digest));
        Next(Step::Copy);
        return;
    }
    case Step::Copy: {   // the throw births a new backpack here, from the client's intent
        void* copy = NewCopy(ue_wrap::object_index::ClassByName(kPackClass));
        if (!copy) {
            if (Expired(kCopyBoundMs)) Fail("no copy of the thrown backpack was born on the host within 90 s");
            return;
        }
        g_copy.Set(copy);
        UE_LOGI("[THROW-DRILL] host: the thrown backpack's copy is born here (awaiting slot 1's slice: %s)",
                cb::Awaited(copy, 1) ? "yes" : "no");
        if (Mode() == "gone") {
            if (!cb::Awaited(copy, 1)) { Abandon("the copy was not awaiting slot 1's slice to end with its author"); return; }
            s->Kick(1, coop::net::EndReason::KickedByHost, "the throw drill ends the thrower's session");
            Next(Step::Gone);
            return;
        }
        Next(Step::Slice);
        return;
    }
    case Step::Slice: {
        size_t n = 0;
        uint64_t digest = 0;
        void* copy = g_copy.Get();
        const bool read = copy && Contents(copy, &n, &digest);
        if (read && digest == g_digest) {
            if (Mode() == "red") {
                Fail("the copy holds the thrower's contents though the client skipped its slice");
                return;
            }
            UE_LOGI("[THROW-DRILL] host DONE in session %d: the copy holds the thrower's %zu records, digest equal", g_session, n);
            g_step = Step::Done;
            return;
        }
        if (!Expired(kSliceBoundMs)) return;
        char why[200];
        std::snprintf(why, sizeof(why), "the copy holds %zu record(s) against the thrower's %d (digest %s); its transfer "
                      "expired: %s", read ? n : 0, kItems, read ? "differs" : "unread",
                      cb::EndedExpired() > g_expired0 ? "yes" : "no");
        Fail(why);
        return;
    }
    case Step::Gone:
        if (cb::EndedGone() > g_gone0) {
            UE_LOGI("[THROW-DRILL] host DONE in session %d: the kicked thrower's transfer ended with it", g_session);
            g_step = Step::Done;
            return;
        }
        if (Expired(kGoneBoundMs)) Fail("the kicked thrower's transfer did not end with it within 15 s");
        return;
    default:
        return;
    }
}

void ClientTick() {
    if (g_step == Step::Done) return;
    // The first client throws; a second, the rig's observer, keeps the host's session alive when gone kicks the
    // first, so the transfer ends through the peer-gone path and not with the whole session.
    if (coop::players::Registry::Get().LocalPeerId() > 1) return;
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;
    switch (g_step) {
    case Step::Arm:
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        Next(Step::Find);
        return;
    case Step::Find: {
        if (!Due()) return;
        ue_wrap::FVector me{};
        void* cls = ue_wrap::object_index::ClassByName(kPackClass);
        void* pack = cls && E::TryGetActorLocation(player, me) ? NearestFixture(cls, me) : nullptr;
        if (pack) {
            g_fixture.Set(pack);
            g_restReads = 0;
            UE_LOGI("[THROW-DRILL] client: the backpack's mirror holds the host's %d records", kItems);
            Next(Step::Settle);
            return;
        }
        if (Expired(kFindBoundMs)) Abandon("no backpack holding the host's two records stood near this client within 60 s");
        return;
    }
    case Step::Settle: {
        if (!Due()) return;
        void* pack = g_fixture.Get();
        ue_wrap::FVector at{};
        if (!pack || !E::TryGetActorLocation(pack, at)) { Abandon("the backpack's mirror died"); return; }
        g_restReads = std::fabs(at.X - g_lastAt.X) + std::fabs(at.Y - g_lastAt.Y) + std::fabs(at.Z - g_lastAt.Z) < 1.f
                          ? g_restReads + 1 : 0;
        g_lastAt = at;
        // At rest and out of the host's fall stream: a mirror the stream still parks is not the hand's to take.
        if (g_restReads >= kRestChecks && !coop::prop_drive_stream::IsParked(pack)) Next(Step::Throw);
        else if (Expired(kSettleBoundMs)) Abandon("the backpack's mirror never came to rest");
        return;
    }
    case Step::Throw: {
        void* pack = g_fixture.Get();
        if (!pack) { Abandon("the backpack's mirror died"); return; }
        if (!coop::dev::drive_drill_verbs::Hold(player, pack)) { Abandon("the backpack could not be taken into the hand"); return; }
        if (!coop::dev::drive_drill_verbs::Throw(player)) { Abandon("the hand's backpack could not be thrown"); return; }
        UE_LOGI("[THROW-DRILL] client (%s): the backpack taken into the hand and thrown, a new actor's birth", Mode().c_str());
        g_step = Step::Done;
        return;
    }
    default:
        return;
    }
}

}  // namespace

void Tick(coop::net::Session* s) {
    if (!Enabled() || !s) return;
    if (s->role() == coop::net::Role::Host) HostTick(s);
    else if (s->connected()) ClientTick();
}

void OnDisconnect() {
    if (!Enabled()) return;
    ++g_session;
    g_step = Step::Arm;
    g_stepMs = g_nextCheckMs = 0;
    g_fixture.Reset();
    g_copy.Reset();
    g_before.clear();
    g_digest = 0;
    g_expired0 = g_gone0 = 0;
    g_lastAt = {};
    g_restReads = 0;
}

}  // namespace coop::dev::throw_drill
