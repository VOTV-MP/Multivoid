// harness/autotest/autotest_container_gaps.cpp -- TEST-ONLY: four container cases, each read on two peers.
//
//   photo  -- each peer fills a drive box with a photo-bearing drive and publishes it; the other peer's box must hold the
//             row and the photo, byte for byte.
//   nested -- the host files a disc into a basket and the basket into a backpack; the client walks to it, takes the
//             basket through the screen, and the basket in its own inventory must hold the disc.
//   bin    -- the host drops a disc into a garbage container, tips it out and destroys it: the client's
//             copy must read the host's list and flags, the listed disc must be glued to it, and the disc must be free at the end.
//   break  -- a basket of two discs takes a lethal impactDamage on the host, another on the client that grabs it: both
//             peers end with the same discs, and the client's hit must be asked of the host.
// The host says `rig: VERDICT container-gaps PASS`; a peer says FAIL at the event that shows a case wrong and the other
// stops; a bound only says INVALID. Reds, on a client: garbage_state_off, nest_record_off, photo_record_off,
// break_request_off. Gated by VOTVCOOP_RUN_CONTAINER_GAPS (VOTVCOOP_CAN_DIR is the peers' folder).

#include "harness/autotest.h"
#include "container_gaps_shared.h"   // co-located private header (src tree, not include/)
#include "harness/autotest/peer_signal.h"

#include "coop/creatures/npc_sync.h"       // GetSession
#include "coop/dev/director/director.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/container_break.h"
#include "coop/props/prop_drive_host.h"   // ClientSyncerOf: whether the host admitted the client's hold
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_save_data.h"
#include "coop/props/remote_prop.h"        // IsDrivenBy
#include "coop/session/host_world_settle.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"
#include "coop/session/world_load_episode.h"
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/desk/drive_chain.h"
#include "ue_wrap/desk/saved_signals.h"
#include "ue_wrap/desk/signal_dynamic.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"  // DestroyActor

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

using namespace container_gaps;   // the case actors, the waits and the ending, shared with the bin case

namespace PS = harness::autotest::peer_signal;
namespace RV = coop::rig_verdict;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace DC = ue_wrap::drive_chain;
namespace SR = ue_wrap::save_record;
namespace SD = ue_wrap::signal_dynamic;
namespace ci = ue_wrap::container_inventory;
namespace CV = ue_wrap::container_view;
namespace P  = ue_wrap::profile;

constexpr const wchar_t* kBoxPath    = L"/Game/objects/prop_box.prop_box_C";
constexpr const wchar_t* kBasketPath = L"/Game/objects/prop_container_pbasket.prop_container_pbasket_C";
constexpr const wchar_t* kPackPath   = L"/Game/objects/prop_backpack.prop_backpack_C";
constexpr const wchar_t* kDiscClass  = L"prop_floppyDisc_R_C";
// A photo's size: 15,824 B, as the rig measured one. Two of them and the box's record fit the record bound (56,092 B);
// two of 30,000 B did not (60,766 B, refused whole), the several-photos residual, which this drill does not measure.
constexpr size_t kPhotoBytes = 15824;

// ---- readiness ----

bool HostWorldUp() {
    return Wait("the host's player", 240, [] {
               void* me = nullptr;
               OnGameThread([&] { me = coop::players::Registry::Get().Local(); });
               return me != nullptr;
           }) &&
           Wait("the host's world settled", 120, [] {
               bool s = false;
               OnGameThread([&] {
                   s = coop::host_world_settle::Settled() && ue_wrap::object_index::IsSeeded() &&
                       ue_wrap::object_index::Backlog() == 0;
               });
               return s;
           });
}

// The announced world, the host's puppet, and the join's reconcile window over (rig: READY joined).
bool ClientWorldUp() {
    return Wait("the announced world", 480, [] {
               bool r = false;
               OnGameThread([&] { r = coop::net_pump::HasAnnouncedWorldReady(); });
               return r;
           }) &&
           Wait("the host's puppet", 60, [] {
               bool have = false;
               OnGameThread([&] {
                   coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(0);
                   have = p && p->valid();
               });
               return have;
           }) &&
           Wait("the reconcile window over", 120, [] {
               bool over = false;
               OnGameThread([&] { over = !coop::world_load_episode::InReconcileWindow(); });
               return over;
           });
}

// ---- helpers ----

void* Me() { return coop::players::Registry::Get().Local(); }

std::string Key(void* a) {
    const std::wstring k = Live(a) ? ue_wrap::prop::GetInteractableKeyString(a) : std::wstring();
    return std::string(k.begin(), k.end());
}

bool KeyOk(const std::string& k) { return !k.empty() && k != "None"; }

void* ByKey(const wchar_t* cls, const std::string& key) {
    const std::wstring w(key.begin(), key.end());
    for (void* o : R::FindObjectsByClass(cls))
        if (Live(o) && ue_wrap::prop::GetInteractableKeyString(o) == w) return o;
    return nullptr;
}

uint64_t Fnv(const std::vector<uint8_t>& b) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t c : b) { h ^= c; h *= 1099511628211ull; }
    return h;
}

// The host's spot for case `i`: a row ahead of the host's player, 150 cm apart.
ue_wrap::FVector Spot(int i, float up) {
    ue_wrap::FVector at{};
    void* me = Me();
    if (!me || !E::TryGetActorLocation(me, at)) return at;
    const ue_wrap::FVector f = E::GetActorForwardVector(me);
    const ue_wrap::FVector side{-f.Y, f.X, 0.f};
    const float s = (i - 2.f) * 150.f;
    return {at.X + f.X * 220.f + side.X * s, at.Y + f.Y * 220.f + side.Y * s, at.Z + up};
}

// The inventory's own addObject, once the container's inventory has run its init (its `obj` is live): before that its arrays
// are not built and the call faults inside the game (the fill loop asks again each poll, so the readiness is the event).
bool PutIn(void* container, void* item) {
    if (!ci::InventoryInitialized(container)) return false;
    void* inv = ci::InventoryOf(container);
    void* fn = inv ? R::FindDispatchFunctionCached(R::ClassOf(inv), L"addObject") : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || !f.Set<void*>(L"actor", item) || !f.Set<int32_t>(L"insertIndex1", -1) || !ue_wrap::Call(inv, f))
        return false;
    return f.Get<bool>(L"return");
}

// The director walks this peer's player to `at`; nobody is teleported. False, said INVALID, when it did not arrive.
bool WalkTo(const ue_wrap::FVector& at, const char* what) {
    std::shared_ptr<coop::director::BackgroundWalk> walk;
    OnGameThread([&] { walk = coop::director::StartBackgroundWalk(at, 150.f, kStepSec); });
    if (!walk) { RV::Say(kTag, RV::Outcome::Invalid, "the walk to the %s did not start", what); Stop(); return false; }
    if (!Wait("the walk", kStepSec + 10, [&] { return walk->state.load() != 0; })) return false;
    if (walk->state.load() != 1) { RV::Say(kTag, RV::Outcome::Invalid, "the walk to the %s did not arrive", what); Stop(); return false; }
    return true;
}

// ---- photo ----

// A photo's bytes: a PNG's signature and IHDR (256 x 256), as a receiver checks them, then filler.
std::vector<uint8_t> PhotoBytes(size_t n, uint8_t seed) {
    std::vector<uint8_t> b(n);
    static const uint8_t kHead[24] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R',
                                      0, 0, 1, 0, 0, 0, 1, 0};
    for (size_t i = 0; i < n; ++i) b[i] = i < 24 ? kHead[i] : static_cast<uint8_t>((i * 131u + seed * 17u) & 0xFF);
    return b;
}

// The photo on a drive's own signal, written raw into its `image` array (engine memory).
bool SetDrivePhoto(void* drive, const std::vector<uint8_t>& png) {
    void* cls = DC::DriveClass();
    const int32_t off = cls ? R::FindPropertyOffset(cls, L"data_0") : -1;
    void* buf = R::EngineAlloc(png.size());
    if (off < 0 || !buf) return false;
    std::memcpy(buf, png.data(), png.size());
    uint8_t* arr = static_cast<uint8_t*>(drive) + off + SD::kOff_image;
    const int32_t n = static_cast<int32_t>(png.size());
    std::memcpy(arr, &buf, sizeof(buf));
    std::memcpy(arr + 8, &n, 4);
    std::memcpy(arr + 12, &n, 4);
    return true;
}

void* SlotComp(void* box, int i) {
    const int32_t off = Live(box) ? R::FindPropertyOffset(R::ClassOf(box), L"boxesTr") : -1;
    if (off < 0) return nullptr;
    const uint8_t* base = static_cast<const uint8_t*>(box) + off;
    void* const* data = *reinterpret_cast<void* const* const*>(base);
    const int32_t n = *reinterpret_cast<const int32_t*>(base + 8);
    return data && i < n && Live(data[i]) ? data[i] : nullptr;
}

// A drive carrying a photo into the box's slot `slot` by the box's own putDriveIn, then the box's record published as
// its own upd would publish it. Returns the photo's hash, 0 when the fill did not take.
uint64_t FillPhotoSlot(int slot, uint8_t seed) {
    uint64_t hash = 0;
    OnGameThread([&] {
        void* put = Live(g.box) ? R::FindFunction(R::ClassOf(g.box), L"putDriveIn") : nullptr;
        ue_wrap::FVector at{};
        if (!put || !DC::EnsureResolved() || !E::TryGetActorLocation(g.box, at)) return;
        void* drive = E::SpawnActor(DC::DriveClass(), {at.X, at.Y, at.Z + 60.f});
        SD::Row row;
        if (!drive || !DC::ReadDriveRow(drive, row)) return;
        std::wstring signal;
        ue_wrap::saved_signals::FirstOrdinarySignalName(signal);
        row.name = L"photo" + std::to_wstring(slot);
        row.id = L"photo-" + std::to_wstring(slot);
        if (!signal.empty()) row.signal = signal;
        row.level = 2;
        row.size = 10.f;
        row.decoded = 10.f;
        row.hasData = true;
        const std::vector<uint8_t> png = PhotoBytes(kPhotoBytes, seed);
        ue_wrap::ParamFrame f(put);
        const bool ok = DC::WriteDriveRow(drive, row) && SetDrivePhoto(drive, png) && DC::CallDriveUpd(drive) &&
                        f.valid() && f.Set<void*>(L"Object", drive) && f.Set<void*>(L"lookAt", SlotComp(g.box, slot)) &&
                        f.Set<bool>(L"ignoreNotif", true) && ue_wrap::Call(g.box, f);
        const std::string k = Key(g.box);
        size_t bytes = 0;
        const bool sent = ok && coop::prop_save_data::Publish(coop::npc_sync::GetSession(), g.box,
                                                              std::wstring(k.begin(), k.end()), &bytes);
        UE_LOGI("container-gaps: filled photo slot %d (ok=%d) photo %zu B, record %zu B, sent=%d", slot, ok ? 1 : 0,
                png.size(), bytes, sent ? 1 : 0);
        if (sent) hash = Fnv(png);
    });
    return hash;
}

// The box's row named photo<slot> as the engine holds it: 0 absent, 1 present without a photo, 2 present with one
// (`size` and `hash` then name it).
int PhotoRow(int slot, size_t& size, uint64_t& hash) {
    int state = 0;
    OnGameThread([&] {
        const int32_t off = Live(g.box) ? R::FindPropertyOffset(R::ClassOf(g.box), L"drives") : -1;
        if (off < 0) return;
        const uint8_t* base = static_cast<const uint8_t*>(g.box) + off;
        const uint8_t* data = *reinterpret_cast<uint8_t* const*>(base);
        const int32_t n = *reinterpret_cast<const int32_t*>(base + 8);
        const std::wstring want = L"photo" + std::to_wstring(slot);
        for (int32_t i = 0; data && i < n && i < 16; ++i) {
            SD::Row row;
            if (!SD::ReadStruct(data + static_cast<size_t>(i) * SD::kStride, row, /*withImage=*/true) || row.name != want)
                continue;
            state = row.image.empty() ? 1 : 2;
            size = row.image.size();
            hash = Fnv(row.image);
        }
    });
    return state;
}

// This peer's box must hold the row the other peer filled, and its photo byte for byte. The row is the event: the photo
// rides the same record, so a row without it cannot change.
bool JudgePhoto(int slot, const std::string& sentHash) {
    size_t size = 0;
    uint64_t hash = 0;
    if (!Wait("the box's photo row", kStepSec, [&] { return PhotoRow(slot, size, hash) != 0; })) return false;
    const int state = PhotoRow(slot, size, hash);
    char mine[24];
    std::snprintf(mine, sizeof(mine), "%016llx", static_cast<unsigned long long>(hash));
    if (state != 2 || sentHash != mine) {
        Fail("photo slot %d: the row is here and its photo is %s (%zu B, hash %s, the sender's %s)", slot,
             state == 2 ? "not the sender's" : "missing", size, mine, sentHash.c_str());
        return false;
    }
    UE_LOGI("container-gaps: photo slot %d crossed, %zu B, hash %s", slot, size, mine);
    return true;
}

// ---- nested ----

// The records a container holds, as a count; -1 when its inventory does not read.
int RecordCount(void* container) {
    std::vector<SR::SaveRecord> recs;
    void* inv = Live(container) ? ci::InventoryOf(container) : nullptr;
    return inv && ci::ReadContents(inv, recs, 512, nullptr) ? static_cast<int>(recs.size()) : -1;
}

// What the container record in this player's own inventory holds: -2 when there is none yet, else its stack's record
// count (-1 when the record names no stack).
int OwnBasketHolds() {
    std::vector<SR::SaveRecord> recs;
    void* inv = CV::OwnInventory() ? ci::InventoryOf(CV::OwnInventory()) : nullptr;
    if (!inv || !ci::ReadContents(inv, recs, 512, nullptr)) return -2;
    for (const auto& r : recs) {
        int32_t stack = -1, count = -1;
        if (!ci::IsContainerRecord(r) || !ci::ContainerStackIndex(r, stack)) continue;
        if (stack >= 0) ci::ReadStackCount(stack, count);
        return count;
    }
    return -2;
}

// The take a player makes: the backpack's inventory screen opened, and its first slot's own button pressed.
bool ClientExtractNested() {
    ue_wrap::FVector at{};
    bool have = false;
    OnGameThread([&] { have = E::TryGetActorLocation(g.pack, at); });
    // A view closes past the arm's 200 uu: the director stands the player beside the backpack first.
    if (!have || !WalkTo(at, "backpack")) return false;
    if (!Wait("the backpack's basket here", kStepSec, [] {
            int n = 0;
            OnGameThread([&] { n = RecordCount(g.pack); });
            return n >= 1;
        }))
        return false;
    bool opened = false;
    OnGameThread([&] { opened = CV::Open(g.pack); });
    if (!opened || !Wait("the backpack's screen", 30, [] {
            void* s = nullptr;
            OnGameThread([&] { s = CV::InventoryScreen(); });
            return s != nullptr;
        })) {
        if (!opened) { RV::Say(kTag, RV::Outcome::Invalid, "the backpack's screen did not open"); Stop(); }
        return false;
    }
    bool pressed = false;
    OnGameThread([&] {
        void* screen = CV::InventoryScreen();
        void* slot = screen ? CV::SlotById(screen, 0) : nullptr;
        pressed = slot && CV::PressSlot(slot);
    });
    if (!pressed) { RV::Say(kTag, RV::Outcome::Invalid, "the backpack's first slot could not be pressed"); Stop(); return false; }
    int holds = -2;
    if (!Wait("the basket in this client's inventory", kStepSec, [&] {
            OnGameThread([&] { holds = OwnBasketHolds(); });
            return holds != -2;
        }))
        return false;
    OnGameThread([] { CV::Close(); });
    if (holds != 1) {
        Fail("nested: the basket this client took holds %d record(s) in its own inventory, the host filed 1", holds);
        return false;
    }
    UE_LOGI("container-gaps: the basket this client took holds its disc");
    return true;
}

// ---- break ----

// A lethal impactDamage on the prop's own physicsImpact component, as a hard impact makes one.
bool Lethal(void* prop) {
    void* cls = R::FindClass(L"prop_C");
    const int32_t off = cls ? R::FindPropertyOffset(cls, L"physicsImpact") : -1;
    void* comp = Live(prop) && off >= 0 ? *reinterpret_cast<void**>(static_cast<uint8_t*>(prop) + off) : nullptr;
    void* fn = Live(comp) ? R::FindFunction(R::ClassOf(comp), L"impactDamage") : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid()) return false;
    f.Set<float>(L"damage", 100000.f);
    f.Set<void*>(L"actor", Me());
    return ue_wrap::Call(comp, f);
}

int DiscsInWorld() {
    int n = 0;
    OnGameThread([&] {
        for (void* d : R::FindObjectsByClass(kDiscClass))
            if (Live(d) && !R::NameStartsWith(R::NameOf(d), L"Default__")) ++n;
    });
    return n;
}

// The local player's physics grab on the basket where it lies, as a player's grab holds a prop: the grab handle takes its
// mesh and the game counts it grabbed, so this client authors it.
bool ClientGrab() {
    bool grabbing = false;
    OnGameThread([&] {
        void* me = Me();
        void* mesh = Live(g.brk2) ? ue_wrap::prop::GetStaticMesh(g.brk2) : nullptr;
        void* handle = me ? E::ReadMainPlayerGrabHandle(me) : nullptr;
        void* phc = R::FindClass(P::name::PhysicsHandleComponentClass);
        void* grab = phc ? R::FindFunction(phc, P::name::GrabComponentAtLocationFn) : nullptr;
        ue_wrap::FVector at{};
        if (mesh && handle && grab && E::TryGetActorLocation(g.brk2, at)) {
            ue_wrap::ParamFrame f(grab);
            f.Set<void*>(L"Component", mesh);
            f.Set<ue_wrap::FVector>(L"GrabLocation", at);
            if (f.valid() && ue_wrap::Call(handle, f)) E::WriteMainPlayerGrabbingPair(me, g.brk2, mesh);
        }
        grabbing = me && E::IsMainPlayerGrabbing(me, g.brk2);
    });
    return grabbing;
}

// ---- the run ----

std::string Join(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? "|" : "") + v[i];
    return s;
}

std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> k;
    for (size_t a = 0, b; a <= s.size(); a = b + 1) {
        b = s.find('|', a);
        if (b == std::string::npos) b = s.size();
        k.push_back(s.substr(a, b - a));
    }
    return k;
}

void RunHost() {
    if (!HostWorldUp() || !Wait("the client", 480, [] { std::string s; return PS::Peek("client_ready", s); })) return;
    // The fixtures, each at its own spot.
    OnGameThread([&] {
        g.box = SpawnPath(kBoxPath, Spot(0, 40.f));
        g.pack = SpawnPath(kPackPath, Spot(1, 40.f));
        void* binCls = R::FindClass(L"prop_garbageContainer_C");
        g.bin = binCls ? E::SpawnActor(binCls, Spot(2, 20.f)) : nullptr;
        g.brk1 = SpawnPath(kBasketPath, Spot(3, 40.f));
        g.brk2 = SpawnPath(kBasketPath, Spot(6, 40.f));
        g.inner = SpawnPath(kBasketPath, Spot(1, 120.f));
    });
    // A container's Init runs a tick after its spawn: each filling is tried until the inventory has bound.
    bool a = false, b = false, f1 = false, f2 = false;
    const bool filled = Wait("the fixtures filled", 60, [&] {
        OnGameThread([&] {
            ue_wrap::FVector at{};
            if (!a && Live(g.inner) && ci::InventoryInitialized(g.inner) && E::TryGetActorLocation(g.inner, at)) {
                void* disc = SpawnPath(kDiscPath, {at.X, at.Y, at.Z + 40.f});
                a = disc && PutIn(g.inner, disc);
                if (disc) E::DestroyActor(disc);
            }
            if (a && !b) {
                b = PutIn(g.pack, g.inner);
                if (b) E::DestroyActor(g.inner);
            }
            for (int k = 0; k < 2; ++k) {
                void* brk = k ? g.brk2 : g.brk1;
                bool& done = k ? f2 : f1;
                ue_wrap::FVector p{};
                if (done || !ci::InventoryInitialized(brk) || !E::TryGetActorLocation(brk, p)) continue;
                int put = 0;
                for (int i = 0; i < 2; ++i) {
                    void* d = SpawnPath(kDiscPath, {p.X, p.Y, p.Z + 60.f + i * 20.f});
                    if (d && PutIn(brk, d)) { E::DestroyActor(d); ++put; } else if (d) E::DestroyActor(d);
                }
                done = put == 2;
            }
        });
        return b && f1 && f2;
    });
    if (!filled) return;
    std::vector<std::string> keys(5);
    if (!Wait("the fixtures' keys", 30, [&] {
            OnGameThread([&] {
                keys = {Key(g.box), Key(g.pack), Key(g.bin), Key(g.brk1), Key(g.brk2)};
            });
            for (const auto& k : keys) if (!KeyOk(k)) return false;
            return true;
        }))
        return;
    UE_LOGI("container-gaps: HOST keys %s", Join(keys).c_str());
    PS::Mark("keys", Join(keys));
    if (!WaitMark("client_found")) return;

    // photo: the client fills slot 0, the host's box must hold it; then the host fills slot 1.
    std::string text;
    if (!WaitMark("photoA", &text) || !JudgePhoto(0, text)) return;
    PS::Mark("photoA_ok");
    uint64_t h = FillPhotoSlot(1, 2);
    if (!h) { RV::Say(kTag, RV::Outcome::Invalid, "the host's photo did not fill"); Stop(); return; }
    char hex[24];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(h));
    PS::Mark("photoB", hex);
    if (!WaitMark("photoB_ok")) return;

    // nested: the client takes the basket out of the backpack.
    if (!WaitMark("nested_ok")) return;

    // bin: its own file (autotest_container_gaps_bin.cpp).
    if (!HostBinLeg()) return;

    // break: the host's own basket, then the client's.
    OnGameThread([] { Lethal(g.brk1); });
    if (!Wait("the host's basket broken", kStepSec, [] { return Dead(&g.brk1); })) return;
    PS::Mark("brk1", std::to_string(DiscsInWorld()));
    if (!WaitMark("brk1_ok")) return;
    // The host judges a break by whether the sender authors the basket: its drive or its syncer row stands on it.
    if (!Wait("the client's hold of its basket admitted", kStepSec, [] {
            bool held = false;
            OnGameThread([&] {
                const coop::element::ElementId eid = coop::prop_element_tracker::GetPropElementIdForActor(g.brk2);
                held = coop::remote_prop::IsDrivenBy(g.brk2, 1) ||
                       (eid != coop::element::kInvalidId && coop::prop_drive_host::ClientSyncerOf(eid) == 1);
            });
            return held;
        }))
        return;
    PS::Mark("brk2_held");
    if (!WaitMark("brk2_hit")) return;
    if (!Wait("the client's basket broken on the host", kStepSec, [] { return Dead(&g.brk2); })) return;
    PS::Mark("brk2", std::to_string(DiscsInWorld()));
    if (!WaitMark("brk2_ok")) return;
    RV::Say(kTag, RV::Outcome::Pass, "photo, nested, bin and break held on both peers");
    UE_LOGI("container-gaps: DONE on host");
}

void RunClient() {
    if (!ClientWorldUp()) return;
    PS::Mark("client_ready");
    std::string keysText;
    if (!WaitMark("keys", &keysText)) return;
    const std::vector<std::string> k = Split(keysText);
    if (k.size() != 5) { RV::Say(kTag, RV::Outcome::Invalid, "the keys line '%s' does not split in five", keysText.c_str()); Stop(); return; }
    if (!Wait("the fixtures here", kStepSec, [&] {
            bool all = false;
            OnGameThread([&] {
                if (!g.box) g.box = ByKey(L"prop_box_C", k[0]);
                if (!g.pack) g.pack = ByKey(L"prop_backpack_C", k[1]);
                if (!g.bin) g.bin = ByKey(L"prop_garbageContainer_C", k[2]);
                if (!g.brk1) g.brk1 = ByKey(L"prop_container_pbasket_C", k[3]);
                if (!g.brk2) g.brk2 = ByKey(L"prop_container_pbasket_C", k[4]);
                all = g.box && g.pack && g.bin && g.brk1 && g.brk2;
            });
            return all;
        }))
        return;
    PS::Mark("client_found");

    // photo
    uint64_t h = FillPhotoSlot(0, 1);
    if (!h) { RV::Say(kTag, RV::Outcome::Invalid, "the client's photo did not fill"); Stop(); return; }
    char hex[24];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(h));
    PS::Mark("photoA", hex);
    if (!WaitMark("photoA_ok")) return;
    std::string text;
    if (!WaitMark("photoB", &text) || !JudgePhoto(1, text)) return;
    PS::Mark("photoB_ok");

    // nested
    if (!ClientExtractNested()) return;
    PS::Mark("nested_ok");

    // bin
    if (!ClientBinLeg()) return;

    // break
    if (!WaitMark("brk1", &text)) return;
    const int want1 = std::atoi(text.c_str());
    if (!Wait("the host's basket gone and its discs here", kStepSec, [&] { return Dead(&g.brk1) && DiscsInWorld() == want1; })) return;
    PS::Mark("brk1_ok");
    if (!ClientGrab()) { RV::Say(kTag, RV::Outcome::Invalid, "the client's grab of its basket did not hold"); Stop(); return; }
    if (!WaitMark("brk2_held")) return;
    const uint32_t asked = coop::props::container_break::AskedCount();
    const uint32_t refused = coop::props::container_break::RefusedAnswerCount();
    bool hit = false;
    OnGameThread([&] { hit = Lethal(g.brk2); });
    if (!hit) { RV::Say(kTag, RV::Outcome::Invalid, "the lethal hit did not run"); Stop(); return; }
    if (coop::props::container_break::AskedCount() == asked) {
        Fail("break: the client's lethal hit on the basket it grabs ran on this copy and asked the host nothing");
        return;
    }
    PS::Mark("brk2_hit");
    // The host's reading, or its refusal of the hit (a decided failure, said at once).
    if (!Wait("the host's reading or its refusal", kStepSec, [&] {
            return PS::Peek("brk2", text) || coop::props::container_break::RefusedAnswerCount() != refused;
        }))
        return;
    if (coop::props::container_break::RefusedAnswerCount() != refused) {
        Fail("break: the host refused the client's lethal hit on the basket it holds");
        return;
    }
    const int want2 = std::atoi(text.c_str());
    if (!Wait("the client's basket gone and the discs equal", kStepSec, [&] { return Dead(&g.brk2) && DiscsInWorld() == want2; })) return;
    PS::Mark("brk2_ok");
    UE_LOGI("container-gaps: DONE on client");
}

}  // namespace

DWORD WINAPI ContainerGapsThread(LPVOID /*arg*/) {
    if (!PS::Begin(kTag)) return 0;
    if (IsClientRole()) RunClient();
    else                RunHost();
    return 0;
}

}  // namespace harness::autotest
