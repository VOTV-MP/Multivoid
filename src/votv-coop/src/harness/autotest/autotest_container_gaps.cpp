// harness/autotest/autotest_container_gaps.cpp -- TEST-ONLY: four container cases, each read on two peers.
//
//   photo  -- each peer fills a drive box (prop_box_C) with a drive whose signal carries a photo, by the box's own
//             putDriveIn, and publishes it as the box's upd would; each peer logs its copy's photos and their hashes.
//   nested -- the host files a disc into a basket and the basket into a backpack (addObject); the client takes the
//             basket out through the backpack's screen. Each peer logs each copy's records and their slots.
//   bin    -- the host drops a disc into a garbage container, then tips it over; each peer logs its itemsInside, its
//             canHold and canCollect, and the disc.
//   break  -- a basket of two discs takes a lethal impactDamage on the host, another on the client that grabs it; each
//             peer logs each basket and the discs in its world.
// After each step each peer logs `container-gaps: STATE <step>`; a file per step orders the peers. Reads only reflection
// and the game's functions, so it runs on a build without the fixes. Gated by VOTVCOOP_RUN_CONTAINER_GAPS (VOTVCOOP_CAN_DIR).

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/creatures/npc_sync.h"       // GetSession
#include "coop/net/blob_chunks.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_save_data.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/desk/drive_chain.h"
#include "ue_wrap/desk/saved_signals.h"
#include "ue_wrap/desk/signal_dynamic.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

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
constexpr const wchar_t* kDiscPath   = L"/Game/objects/prop_floppyDisc_R.prop_floppyDisc_R_C";
constexpr const wchar_t* kDiscClass  = L"prop_floppyDisc_R_C";
constexpr size_t kPhotoBytes = 30000;   // a photo's size when the save has none to measure

std::string g_dir;

// The case actors, by the key the host names (game thread only).
struct Actors {
    void* box = nullptr;
    void* basket = nullptr;   // nested: the outer, a backpack
    void* bin = nullptr;
    void* brk1 = nullptr;     // break: the host's
    void* brk2 = nullptr;     // break: the client's
    void* inner = nullptr;    // nested: the host's inner basket before it is filed
    std::string brk2Key;
} g;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void Write(const std::string& name, const std::string& text) {
    std::ofstream(g_dir + "\\" + name, std::ios::binary | std::ios::trunc) << text;
}

bool Await(const std::string& name, std::string* text = nullptr, int seconds = 240) {
    for (int i = 0; i < seconds * 10; ++i) {
        std::ifstream f(g_dir + "\\" + name, std::ios::binary);
        std::string s;
        if (f && std::getline(f, s) && !s.empty()) {
            if (text) *text = s;
            return true;
        }
        ::Sleep(100);
    }
    UE_LOGW("container-gaps: VERDICT ABORT -- no '%hs' from the other peer", name.c_str());
    return false;
}

void* Me() { return coop::players::Registry::Get().Local(); }

bool Live(void* a) { return a && R::IsLive(a); }

std::string Key(void* a) {
    const std::wstring k = Live(a) ? ue_wrap::prop::GetInteractableKeyString(a) : std::wstring();
    return std::string(k.begin(), k.end());
}

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

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
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

void* SpawnPath(const wchar_t* path, const ue_wrap::FVector& at) {
    void* cls = ue_wrap::asset_load::LoadObjectByPath(path);
    return cls ? E::SpawnActor(cls, at) : nullptr;
}

std::string WaitKey(void* a) {
    std::string k;
    for (int i = 0; i < 100 && (k.empty() || k == "None"); ++i) {
        OnGameThread([&] { k = Key(a); });
        if (k.empty() || k == "None") ::Sleep(100);
    }
    return k;
}

// The inventory's own addObject.
bool PutIn(void* container, void* item) {
    void* inv = ci::InventoryOf(container);
    void* fn = inv ? R::FindDispatchFunctionCached(R::ClassOf(inv), L"addObject") : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || !f.Set<void*>(L"actor", item) || !f.Set<int32_t>(L"insertIndex1", -1) || !ue_wrap::Call(inv, f))
        return false;
    return f.Get<bool>(L"return");
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
// its own upd would publish it.
void FillPhotoSlot(int slot, uint8_t seed) {
    OnGameThread([&] {
        void* put = Live(g.box) ? R::FindFunction(R::ClassOf(g.box), L"putDriveIn") : nullptr;
        ue_wrap::FVector at{};
        if (!put || !DC::EnsureResolved() || !E::TryGetActorLocation(g.box, at)) {
            UE_LOGW("container-gaps: photo slot %d -- no box or putDriveIn", slot);
            return;
        }
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
        UE_LOGI("container-gaps: %hs filled photo slot %d (ok=%d) photo %zu B hash %016llx, record %zu B, sent=%d",
                IsClientRole() ? "CLIENT" : "HOST", slot, ok ? 1 : 0, png.size(),
                static_cast<unsigned long long>(Fnv(png)), bytes, sent ? 1 : 0);
    });
}

std::string PhotoState() {
    SR::SaveRecord rec;
    if (!Live(g.box) || !SR::CaptureRecord(g.box, rec)) return "box=none";
    // The record as this build reads it, and the photos the engine row itself holds.
    std::string s = "signals=" + std::to_string(rec.signals.size());
    const int32_t off = R::FindPropertyOffset(R::ClassOf(g.box), L"drives");
    if (off < 0) return s + " photos=? (the box has no `drives`)";
    int engine = 0;
    std::string hashes;
    if (off >= 0) {
        const uint8_t* base = static_cast<const uint8_t*>(g.box) + off;
        const uint8_t* data = *reinterpret_cast<uint8_t* const*>(base);
        const int32_t n = *reinterpret_cast<const int32_t*>(base + 8);
        for (int32_t i = 0; data && i < n && i < 16; ++i) {
            SD::Row row;
            if (!SD::ReadStruct(data + static_cast<size_t>(i) * SD::kStride, row, /*withImage=*/true)) continue;
            if (row.image.empty()) continue;
            ++engine;
            char h[48];
            std::snprintf(h, sizeof(h), " %ls:%zuB:%016llx", row.name.c_str(), row.image.size(),
                          static_cast<unsigned long long>(Fnv(row.image)));
            hashes += h;
        }
    }
    return s + " photos=" + std::to_string(engine) + hashes;
}

// ---- nested ----

std::string Records(void* container) {
    std::vector<SR::SaveRecord> recs;
    void* inv = Live(container) ? ci::InventoryOf(container) : nullptr;
    if (!inv || !ci::ReadContents(inv, recs, 512, nullptr)) return "none";
    std::string s = std::to_string(recs.size()) + "[";
    for (size_t i = 0; i < recs.size(); ++i) {
        const auto& r = recs[i];
        const size_t slash = r.className.find_last_of(L"./");
        const std::wstring leaf = slash == std::wstring::npos ? r.className : r.className.substr(slash + 1);
        s += (i ? "," : "") + std::string(leaf.begin(), leaf.end());
        int32_t stack = -1, count = -1;
        if (ci::IsContainerRecord(r) && ci::ContainerStackIndex(r, stack)) {
            if (stack >= 0) ci::ReadStackCount(stack, count);
            s += "{stack=" + std::to_string(stack) + " holds=" + std::to_string(count) + "}";
        }
    }
    return s + "]";
}

std::string NestedState() {
    std::string s = "pack=" + Records(g.basket);
    int boxes = 0;
    std::string held;
    for (void* o : R::FindObjectsByClass(L"prop_container_pbasket_C")) {
        if (!Live(o) || o == g.brk1 || o == g.brk2 || R::NameStartsWith(R::NameOf(o), L"Default__")) continue;
        ++boxes;
        held += " basket=" + Records(o);
    }
    // A take lands in the taker's own inventory: the basket's record there, and what its stack holds.
    std::string own;
    if (IsClientRole()) {
        std::vector<SR::SaveRecord> recs;
        void* inv = CV::OwnInventory() ? ci::InventoryOf(CV::OwnInventory()) : nullptr;
        if (inv && ci::ReadContents(inv, recs, 512, nullptr))
            for (const auto& r : recs) {
                int32_t stack = -1, count = -1;
                if (!ci::IsContainerRecord(r) || !ci::ContainerStackIndex(r, stack)) continue;
                if (stack >= 0) ci::ReadStackCount(stack, count);
                own += " own{stack=" + std::to_string(stack) + " holds=" + std::to_string(count) + "}";
            }
    }
    return s + " loose_baskets=" + std::to_string(boxes) + held + own;
}

// ---- bin ----

int32_t g_offItems = -1, g_offPropData = -1, g_offCanHold = -1, g_offCanCollect = -1;

std::string BinState() {
    if (!Live(g.bin)) return "bin=none";
    void* cls = R::ClassOf(g.bin);
    if (g_offItems < 0) g_offItems = R::FindPropertyOffset(cls, L"itemsInside");
    if (g_offPropData < 0) {
        g_offPropData = R::FindPropertyOffset(cls, L"propData");
        if (void* st = R::PropertyInnerStruct(cls, L"propData")) {
            g_offCanHold = R::FindPropertyOffsetByPrefix(st, L"canHold_");
            g_offCanCollect = R::FindPropertyOffsetByPrefix(st, L"canCollect_");
        }
    }
    int32_t items = -1;
    if (g_offItems >= 0) items = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(g.bin) + g_offItems + 8);
    const uint8_t* pd = g_offPropData >= 0 ? static_cast<const uint8_t*>(g.bin) + g_offPropData : nullptr;
    const int hold = pd && g_offCanHold >= 0 ? (pd[g_offCanHold] ? 1 : 0) : -1;
    const int collect = pd && g_offCanCollect >= 0 ? (pd[g_offCanCollect] ? 1 : 0) : -1;
    ue_wrap::FVector binAt{};
    E::TryGetActorLocation(g.bin, binAt);
    int nearby = 0, attached = 0;
    for (void* d : R::FindObjectsByClass(kDiscClass)) {
        ue_wrap::FVector at{};
        if (!Live(d) || !E::TryGetActorLocation(d, at) || Dist(at, binAt) > 200.f) continue;
        ++nearby;
        void* parent = nullptr;
        if (E::GetAttachParentActor(d, parent) && parent == g.bin) ++attached;
    }
    ue_wrap::FRotator rot{};
    E::TryGetActorRotation(g.bin, rot);
    char buf[200];
    std::snprintf(buf, sizeof(buf), "itemsInside=%d canHold=%d canCollect=%d discsNear=%d discsAttached=%d binRoll=%.0f",
                  items, hold, collect, nearby, attached, rot.Roll);
    return buf;
}

// ---- break ----

std::string BreakState(void* basket, const char* name) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s: basket=%d records=%s", name, Live(basket) ? 1 : 0,
                  Live(basket) ? Records(basket).c_str() : "-");
    return buf;
}

int DiscsInWorld() {
    int n = 0;
    for (void* d : R::FindObjectsByClass(kDiscClass))
        if (Live(d) && !R::NameStartsWith(R::NameOf(d), L"Default__")) ++n;
    return n;
}

void LogState(const std::string& step) {
    OnGameThread([&] {
        if (!Live(g.brk2) && !g.brk2Key.empty()) g.brk2 = ByKey(L"prop_container_pbasket_C", g.brk2Key);
        UE_LOGI("container-gaps: STATE %hs photo{%hs} nested{%hs} bin{%hs} break{%hs | %hs | discsInWorld=%d}",
                step.c_str(), PhotoState().c_str(), NestedState().c_str(), BinState().c_str(),
                BreakState(g.brk1, "first").c_str(), BreakState(g.brk2, "held").c_str(), DiscsInWorld());
    });
}

// ---- the actions ----

void ClientPhoto() { FillPhotoSlot(0, 1); }
void HostPhoto() { FillPhotoSlot(1, 2); }

// The take a player makes: the backpack's inventory screen opened, and its first slot's own button pressed.
void ClientExtractNested() {
    // A view closes past the arm's 200 uu: the player stands beside the backpack first.
    OnGameThread([] {
        ue_wrap::FVector at{};
        if (E::TryGetActorLocation(g.basket, at)) E::TeleportTo(Me(), {at.X + 90.f, at.Y, at.Z + 60.f}, ue_wrap::FRotator{});
    });
    ::Sleep(1500);
    bool opened = false;
    OnGameThread([&] { opened = CV::Open(g.basket); });
    ::Sleep(1500);
    bool pressed = false;
    OnGameThread([&] {
        void* screen = CV::InventoryScreen();
        void* slot = screen ? CV::SlotById(screen, 0) : nullptr;
        pressed = slot && CV::PressSlot(slot);
    });
    ::Sleep(1500);
    OnGameThread([] { CV::Close(); });
    UE_LOGI("container-gaps: CLIENT took the backpack's first record through its screen (opened=%d pressed=%d)",
            opened ? 1 : 0, pressed ? 1 : 0);
}

void HostTipBin() {
    OnGameThread([] {
        ue_wrap::FRotator rot{};
        E::TryGetActorRotation(g.bin, rot);
        const bool ok = E::SetActorRotation(g.bin, ue_wrap::FRotator{rot.Pitch, rot.Yaw, 180.f});
        UE_LOGI("container-gaps: HOST tipped the bin over (ok=%d)", ok ? 1 : 0);
    });
}

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

void HostBreak() {
    OnGameThread([] { UE_LOGI("container-gaps: HOST broke its basket (ok=%d)", Lethal(g.brk1) ? 1 : 0); });
}

// The local player's physics grab on the basket where it lies, as a player's grab holds a prop: the grab handle takes
// its mesh and the game counts it grabbed, so this client authors it.
void ClientGrab() {
    OnGameThread([] {
        void* me = Me();
        void* mesh = Live(g.brk2) ? ue_wrap::prop::GetStaticMesh(g.brk2) : nullptr;
        void* handle = me ? E::ReadMainPlayerGrabHandle(me) : nullptr;
        void* phc = R::FindClass(P::name::PhysicsHandleComponentClass);
        void* grab = phc ? R::FindFunction(phc, P::name::GrabComponentAtLocationFn) : nullptr;
        ue_wrap::FVector at{};
        bool ok = false;
        if (mesh && handle && grab && E::TryGetActorLocation(g.brk2, at)) {
            ue_wrap::ParamFrame f(grab);
            f.Set<void*>(L"Component", mesh);
            f.Set<ue_wrap::FVector>(L"GrabLocation", at);
            ok = f.valid() && ue_wrap::Call(handle, f) && E::WriteMainPlayerGrabbingPair(me, g.brk2, mesh);
        }
        UE_LOGI("container-gaps: CLIENT grabbed the second basket (ok=%d grabbing=%d)", ok ? 1 : 0,
                me && E::IsMainPlayerGrabbing(me, g.brk2) ? 1 : 0);
    });
}

void ClientBreak() {
    OnGameThread([] { UE_LOGI("container-gaps: CLIENT broke the basket it grabs (ok=%d)", Lethal(g.brk2) ? 1 : 0); });
}

void Step(bool hostActs, const char* name, void (*act)(), int waitMs) {
    const std::string done = std::string(name) + (hostActs ? "_host" : "_client");
    const bool mine = hostActs != IsClientRole();
    if (mine) {
        act();
        ::Sleep(waitMs);
        LogState(name);
        Write(done, "x");
        Await(std::string(name) + "_seen");
    } else {
        if (!Await(done)) return;
        ::Sleep(2500);
        LogState(name);
        Write(std::string(name) + "_seen", "x");
    }
}

void RunSteps() {
    LogState("0-start");
    Step(false, "1-client-photo", &ClientPhoto, 4000);
    Step(true, "2-host-photo", &HostPhoto, 4000);
    Step(false, "3-client-extracts-nested", &ClientExtractNested, 5000);
    Step(true, "4-host-tips-bin", &HostTipBin, 4000);
    Step(true, "5-host-breaks", &HostBreak, 4000);
    Step(false, "6-client-holds", &ClientGrab, 3000);
    Step(false, "7-client-breaks-held", &ClientBreak, 4000);
    UE_LOGI("container-gaps: DONE");
}

void RunHost() {
    ::Sleep(20000);
    if (!Await("client_ready")) return;
    // Real photo sizes, from the host's signal library.
    OnGameThread([] {
        std::string sizes;
        int with = 0;
        for (int32_t i = 0; i < 400; ++i) {
            SD::Row row;
            if (!ue_wrap::saved_signals::ReadRow(i, row, /*withImage=*/true)) break;
            if (row.image.empty()) continue;
            ++with;
            if (with <= 8) sizes += " " + std::to_string(row.image.size());
        }
        UE_LOGI("container-gaps: HOST library photos=%d sizes(B):%hs", with, sizes.c_str());
    });
    // The fixtures, each at its own spot.
    std::string keys;
    OnGameThread([&] {
        g.box = SpawnPath(kBoxPath, Spot(0, 40.f));
        g.basket = SpawnPath(kPackPath, Spot(1, 40.f));
        void* binCls = R::FindClass(L"prop_garbageContainer_C");
        g.bin = binCls ? E::SpawnActor(binCls, Spot(2, 20.f)) : nullptr;
        g.brk1 = SpawnPath(kBasketPath, Spot(3, 40.f));
        g.brk2 = SpawnPath(kBasketPath, Spot(6, 40.f));
        g.inner = SpawnPath(kBasketPath, Spot(1, 120.f));
    });
    ::Sleep(3000);
    // The nested pair, tried until both inventories have bound (a container's Init runs a tick after its spawn).
    bool a = false, b = false;
    for (int i = 0; i < 20 && !b; ++i) {
        OnGameThread([&] {
            ue_wrap::FVector at{};
            if (!a && Live(g.inner) && E::TryGetActorLocation(g.inner, at)) {
                void* disc = SpawnPath(kDiscPath, {at.X, at.Y, at.Z + 40.f});
                a = disc && PutIn(g.inner, disc);
                if (disc) E::DestroyActor(disc);
            }
            if (a && !b) {
                b = PutIn(g.basket, g.inner);
                if (b) E::DestroyActor(g.inner);
            }
        });
        if (!b) ::Sleep(500);
    }
    // The baskets that break, each filled by the inventory's own addObject.
    OnGameThread([&] {
        int filled = 0;
        for (void* brk : {g.brk1, g.brk2}) {
            ue_wrap::FVector p{};
            if (!E::TryGetActorLocation(brk, p)) continue;
            for (int i = 0; i < 2; ++i) {
                void* d = SpawnPath(kDiscPath, {p.X, p.Y, p.Z + 60.f + i * 20.f});
                if (d && PutIn(brk, d)) { E::DestroyActor(d); ++filled; }
            }
        }
        // A disc dropped into the bin from above.
        ue_wrap::FVector binAt{};
        if (E::TryGetActorLocation(g.bin, binAt)) SpawnPath(kDiscPath, {binAt.X, binAt.Y, binAt.Z + 70.f});
        UE_LOGI("container-gaps: HOST fixtures: disc in basket=%d, basket in backpack=%d, discs in break baskets=%d",
                a ? 1 : 0, b ? 1 : 0, filled);
    });
    keys = WaitKey(g.box) + "|" + WaitKey(g.basket) + "|" + WaitKey(g.bin) + "|" + WaitKey(g.brk1) + "|" + WaitKey(g.brk2);
    g.brk2Key = WaitKey(g.brk2);
    UE_LOGI("container-gaps: HOST keys %hs", keys.c_str());
    ::Sleep(6000);
    Write("keys", keys);
    if (!Await("client_found")) return;
    RunSteps();
}

void RunClient() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    ::Sleep(10000);
    Write("client_ready", "x");
    std::string keys;
    if (!Await("keys", &keys)) return;
    std::vector<std::string> k;
    for (size_t a = 0, b; a <= keys.size(); a = b + 1) {
        b = keys.find('|', a);
        if (b == std::string::npos) b = keys.size();
        k.push_back(keys.substr(a, b - a));
    }
    if (k.size() != 5) {
        UE_LOGW("container-gaps: VERDICT ABORT -- the keys line '%hs' does not split in five", keys.c_str());
        return;
    }
    bool all = false;
    for (int i = 0; i < 300 && !all; ++i) {
        OnGameThread([&] {
            if (!g.box) g.box = ByKey(L"prop_box_C", k[0]);
            if (!g.basket) g.basket = ByKey(L"prop_backpack_C", k[1]);
            if (!g.bin) g.bin = ByKey(L"prop_garbageContainer_C", k[2]);
            if (!g.brk1) g.brk1 = ByKey(L"prop_container_pbasket_C", k[3]);
            if (!g.brk2) g.brk2 = ByKey(L"prop_container_pbasket_C", k[4]);
            g.brk2Key = k[4];
            all = g.box && g.basket && g.bin && g.brk1 && g.brk2;
        });
        if (!all) ::Sleep(100);
    }
    UE_LOGI("container-gaps: CLIENT found box=%d basket=%d bin=%d brk1=%d brk2=%d", g.box ? 1 : 0, g.basket ? 1 : 0,
            g.bin ? 1 : 0, g.brk1 ? 1 : 0, g.brk2 ? 1 : 0);
    ::Sleep(3000);
    Write("client_found", "x");
    RunSteps();
}

}  // namespace

DWORD WINAPI ContainerGapsThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("container-gaps: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    if (IsClientRole()) RunClient();
    else                RunHost();
    return 0;
}

}  // namespace harness::autotest
