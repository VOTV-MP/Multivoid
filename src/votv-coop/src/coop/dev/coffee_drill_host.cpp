// coop/dev/coffee_drill_host.cpp -- see coop/dev/coffee_drill.h: the host's half. It picks a powered wall socket,
// spawns the maker and plugs it in, opens each arm on a chat edge, acts in its own arms (it grants the points, drops
// the cup and the bag, spawns the spare cups), and closes an arm when its copies show the arm's state and the client
// has said the same.

#include "coffee_drill_detail.h"  // co-located private header (src tree, not include/)
#include "cord_drill_detail.h"    // co-located private header: Narrow, Wide and the stand reads

#include "coop/net/session.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/coffee.h"
#include "ue_wrap/devices/cord.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/world/economy.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::coffee_drill::detail {
namespace {

namespace CD = ue_wrap::cord;
namespace CF = ue_wrap::coffee;
namespace E = ue_wrap::engine;
namespace CDD = coop::dev::cord_drill::detail;

enum class Ph : uint8_t { Wait, Plug, Stage, Arm, Rejoin, Done };

struct Host {
    Ph          ph = Ph::Wait;
    int         arm = 0;
    int         sub = 0;
    uint64_t    phaseMs = 0, armMs = 0;
    bool        closable = false, seen = false;
    bool        clientArriving = false;   // the client walks or waits in this arm: its bound starts at its "at" line
    std::string consumed;                 // the nonce of the client's ready line the host acted on
    int32_t     points0 = 0;              // arm 1: the balance after the grant
    std::vector<std::wstring> cupsBefore; // arms 1 and 3: the cups that lived before the press
    std::wstring cupKey[kArms + 1];       // the key said in arm n, for the host's own reads
    ue_wrap::CachedObjRef cup;            // arms 2, 4, 5: the empty cup the host spawned
    ue_wrap::CachedObjRef bag;
    int32_t     bagAmounts0 = 0;
    std::wstring bagKey;
    ue_wrap::FVector standAt{};           // the nominal stand point: the spare cups start over it
};
Host g_h;

void Fail(const char* arm, const char* why) {
    FailLine("host", arm, why);
    g_h.ph = Ph::Done;
}
void FailArm(const char* why) {
    char name[16];
    std::snprintf(name, sizeof(name), "arm%d", g_h.arm);
    Fail(name, why);
}

bool FreshReady() {
    std::string nonce;
    if (!Rest("coffee client ready ", &nonce) || nonce == g_h.consumed) return false;
    g_h.consumed = nonce;
    return true;
}

// ---- the cast: a maker plugged into a powered wall socket ----

bool SetUp(void* player) {
    void* vend = NearestVending(player);
    const std::wstring vendKey = vend ? ue_wrap::prop::GetKeyString(vend) : std::wstring();
    if (!KeyRides(vendKey)) { Fail("stage", "the world holds no keyed vending machine"); return false; }
    ue_wrap::FVector vendAt{}, vendStand{};
    if (!VendingStand(player, vend, vendAt, vendStand)) { Fail("stage", "the vending machine has no nav point near it"); return false; }
    Pick pick;
    const bool found = ChooseSocket(player, vendStand, pick);
    UE_LOGI("[COFFEE] host setup: free sockets=%d powered=%d placed=%d side-blocked=%d off the vending floor=%d "
            "without a route=%d socket=%s", pick.free, pick.powered, pick.placed, pick.blocked, pick.offFloor,
            pick.noRoute, found ? "found" : "none");
    if (!found) {
        // The world gave the drill no ground to run on: not a verdict on the lane. The client is told so it ends too.
        UE_LOGW("[COFFEE] INVALID (host) -- no maker spot on the vending floor");
        Say("coffee host invalid");
        g_h.ph = Ph::Done;
        return false;
    }
    void* cls = ue_wrap::reflection::FindClass(L"prop_coffeeMaker_C");
    void* maker = cls ? E::SpawnActor(cls, pick.makerAt) : nullptr;
    const std::wstring makerKey = maker ? ue_wrap::prop::GetKeyString(maker) : std::wstring();
    if (!KeyRides(makerKey)) { Fail("stage", "the maker did not spawn with a key"); return false; }
    const std::wstring sockKey = ue_wrap::prop::GetActorSaveKeyString(pick.sock);
    if (!KeyRides(sockKey)) { Fail("stage", "the chosen socket has no key"); return false; }
    Cast& c = TheCast();
    c.vending.Set(vend);
    c.maker.Set(maker);
    c.sock.Set(pick.sock);
    c.vendKey = vendKey;
    c.makerKey = makerKey;
    c.sockKey = sockKey;
    g_h.standAt = pick.stand;
    UE_LOGI("[COFFEE] host chose socket key=%s, spawned the maker key=%s, the vending machine key=%s",
            CDD::Narrow(sockKey).c_str(), CDD::Narrow(makerKey).c_str(), CDD::Narrow(vendKey).c_str());
    UE_LOGI("[COFFEE] host layout: vending at (%.0f,%.0f,%.0f) stand (%.0f,%.0f,%.0f); socket at (%.0f,%.0f,%.0f); "
            "client stand (%.0f,%.0f,%.0f), host stand (%.0f,%.0f,%.0f); route %.0f cm, largest height delta %.0f cm",
            vendAt.X, vendAt.Y, vendAt.Z, vendStand.X, vendStand.Y, vendStand.Z, pick.at.X, pick.at.Y, pick.at.Z,
            pick.clientNav.X, pick.clientNav.Y, pick.clientNav.Z, pick.hostNav.X, pick.hostNav.Y, pick.hostNav.Z,
            pick.routeCm, pick.maxDzCm);
    return true;
}

// ---- the phases ----

void HostWait(coop::net::Session* s, void* player) {
    if (!s->IsSlotWorldReady(1) || !FreshReady()) return;
    if (!SetUp(player)) return;
    g_h.ph = Ph::Plug;
    g_h.phaseMs = NowMs();
}

// The maker's cord exists once it has begun play; the host plugs it by a scripted connect, as the cord drill's host
// does, and the maker reads power once its cord reports the plug.
void HostPlug() {
    void* maker = Maker();
    void* sock = Sock();
    if (!maker || !sock) return Fail("stage", "the maker or the socket is gone");
    if (NowMs() - g_h.phaseMs > kArmFailMs) return Fail("stage", "the maker was not plugged and powered in time");
    void* cord = CD::IsCordedProp(maker) ? CD::CordOf(maker) : nullptr;
    if (!cord) return;
    if (g_h.sub == 0) {
        if (!CD::Connect(cord, sock)) return Fail("stage", "the scripted connect of the maker did not run");
        g_h.sub = 1;
        return;
    }
    if (CD::SocketOf(cord) != sock) return Fail("stage", "the scripted connect did not plug the maker into the socket");
    Flags f;
    if (!ReadFlags(maker, f) || f.power != 1) return;
    const Cast& c = TheCast();
    UE_LOGI("[COFFEE] host maker is plugged and powered");
    LogMakerAt("after the plug");
    Say("coffee keys V=" + CDD::Narrow(c.vendKey) + " M=" + CDD::Narrow(c.makerKey) + " S=" + CDD::Narrow(c.sockKey));
    g_h.ph = Ph::Stage;
    g_h.phaseMs = NowMs();
}

void OpenArm(int n);

void HostStage() {
    if (Heard("coffee client staged")) return OpenArm(1);
    if (NowMs() - g_h.phaseMs > kStageFailMs) Fail("stage", "the client never resolved the devices");
}

// A spare empty cup over the stand point; it falls to the floor there, clear of the maker.
// The brew of arm 3 leaves its coffee cup on the maker's plate, where an empty cup carried there lands on it and
// never touches the maker. Every coffee cup on the maker goes before the carry. Collected first: the instance walk
// must not destroy what it walks.
void ClearCoffeeCupsOnMaker() {
    void* maker = Maker();
    ue_wrap::FVector m{};
    void* cls = ue_wrap::object_index::ClassByName(L"prop_food_coffeeCup_C");
    if (!maker || !cls || !E::TryGetActorLocation(maker, m)) return;
    std::vector<void*> cups;
    ue_wrap::object_index::ForEachInstance(cls, [](void* ctx, void* o, int32_t) {
        static_cast<std::vector<void*>*>(ctx)->push_back(o);
    }, &cups);
    int cleared = 0;
    for (void* c : cups) {
        ue_wrap::FVector at{};
        if (ue_wrap::reflection::NameStartsWith(ue_wrap::reflection::NameOf(c), L"Default__") ||
            !E::TryGetActorLocation(c, at))
            continue;
        const float dx = at.X - m.X, dy = at.Y - m.Y, dz = at.Z - m.Z;
        if (dx * dx + dy * dy + dz * dz > 120.f * 120.f) continue;
        E::DestroyActor(c);
        ++cleared;
    }
    UE_LOGI("[COFFEE] host cleared %d coffee cup(s) off the maker before the carry", cleared);
}

bool SpawnSpareCup(int n) {
    const ue_wrap::FVector at{g_h.standAt.X, g_h.standAt.Y, g_h.standAt.Z + kCupLiftCm};
    void* cup = SpawnProp(L"prop_C", at, L"cup");
    const std::wstring key = cup ? ue_wrap::prop::GetKeyString(cup) : std::wstring();
    if (!KeyRides(key)) { FailArm("the spare cup did not spawn with a key"); return false; }
    g_h.cup.Set(cup);
    g_h.cupKey[n] = key;
    Say(CupPrefix(n) + CDD::Narrow(key));
    return true;
}

void OpenArm(int n) {
    g_h.ph = Ph::Arm;
    g_h.arm = n;
    g_h.sub = 0;
    g_h.armMs = NowMs();
    g_h.closable = g_h.seen = false;
    g_h.clientArriving = n == 1 || n == 3 || n == 4 || n == 5;
    char when[24];
    std::snprintf(when, sizeof(when), "at arm %d's open", n);
    LogMakerAt(when);
    Say(ArmLine(n));
    if (n == 1) {
        ue_wrap::economy::AddPoints(kGrantPoints);
        g_h.points0 = Points();
        g_h.cupsBefore = CupKeys();
        UE_LOGI("[COFFEE] host granted %d points: balance=%d, %zu cups live", static_cast<int>(kGrantPoints),
                static_cast<int>(g_h.points0), g_h.cupsBefore.size());
    } else if (n == 3) {
        // The bag has done its work; a bag left touching the maker would fill it again after the brew.
        if (void* bag = g_h.bag.Get()) E::DestroyActor(bag);
        g_h.bag.Reset();
        g_h.cupsBefore = CupKeys();
    } else if (n == 4) {
        SpawnSpareCup(4);
    } else if (n == 5) {
        // The arm-4 cup lies where the arm-5 cup will; it goes first so the client's grab aims at one.
        if (void* old = g_h.cup.Get()) E::DestroyActor(old);
        g_h.cup.Reset();
        ClearCoffeeCupsOnMaker();
        SpawnSpareCup(5);
    }
}

// ---- what the host's copies must show when arm n has closed ----

bool Sees(int n, std::string& saw) {
    Flags f;
    const bool read = Maker() && ReadFlags(Maker(), f);
    char buf[200];
    switch (n) {
    case 1: {
        const int32_t p = Points();
        std::snprintf(buf, sizeof(buf), "points %d -> %d (want %d), new cup %s", static_cast<int>(g_h.points0),
                      static_cast<int>(p), static_cast<int>(g_h.points0 - kVendingCost),
                      g_h.cupKey[1].empty() ? "none" : "live");
        saw = buf;
        return p == g_h.points0 - kVendingCost && !g_h.cupKey[1].empty() && LiveByKey(g_h.cupKey[1]);
    }
    case 2: {
        int32_t amounts = -1;
        void* bag = g_h.bag.Get();
        if (bag) CF::ReadBagAmounts(bag, amounts);
        std::snprintf(buf, sizeof(buf), "%s, cup %s, bag amounts %d (want %d)", read ? FlagsText(f).c_str() : "flags unread",
                      g_h.cup.Get() ? "STILL LIVES" : "gone", amounts, static_cast<int>(g_h.bagAmounts0 - 1));
        saw = buf;
        return read && FlagsAre(f, 1, 1, 0) && f.power == 1 && !g_h.cup.Get() && bag && amounts == g_h.bagAmounts0 - 1;
    }
    case 3:
        std::snprintf(buf, sizeof(buf), "%s, new cup %s", read ? FlagsText(f).c_str() : "flags unread",
                      g_h.cupKey[3].empty() ? "none" : "live");
        saw = buf;
        return read && FlagsAre(f, 0, 0, 0) && !g_h.cupKey[3].empty() && LiveByKey(g_h.cupKey[3]);
    case 4:
        std::snprintf(buf, sizeof(buf), "%s, the arm's cup %s", read ? FlagsText(f).c_str() : "flags unread",
                      g_h.cup.Get() ? "lives" : "GONE");
        saw = buf;
        return read && FlagsAre(f, 0, 0, 0) && g_h.cup.Get();
    default:
        std::snprintf(buf, sizeof(buf), "%s, the arm's cup %s", read ? FlagsText(f).c_str() : "flags unread",
                      g_h.cup.Get() ? "STILL LIVES" : "gone");
        saw = buf;
        return read && FlagsAre(f, 1, 0, 0) && !g_h.cup.Get();
    }
}

// Spawns a cup of `cls` over the maker, keyed; null, said, when it did not spawn.
void* DropOverMaker(const wchar_t* cls, const wchar_t* nameRow) {
    ue_wrap::FVector at{};
    if (!E::TryGetActorLocation(Maker(), at)) { FailArm("the maker's place did not read"); return nullptr; }
    at.Z += kAboveCm;
    void* p = SpawnProp(cls, at, nameRow);
    if (!p || !KeyRides(ue_wrap::prop::GetKeyString(p))) { FailArm("the dropped prop did not spawn with a key"); return nullptr; }
    return p;
}

// The arm's own actions; each sets `closable` once its last act has run.
void ArmActs() {
    Host& h = g_h;
    switch (h.arm) {
    case 1:   // the press is the client's; the host watches for the cup its copy of the vending machine made
        if (h.cupKey[1].empty()) {
            h.cupKey[1] = NewCupKey(h.cupsBefore);
            if (!h.cupKey[1].empty()) Say(CupPrefix(1) + CDD::Narrow(h.cupKey[1]));
        }
        if (Points() < h.points0 - kVendingCost) return FailArm("the balance fell by more than one coffee: the press ran twice");
        h.closable = !h.cupKey[1].empty();
        return;
    case 2:
        if (h.sub == 0) {   // the cup first, so the maker's two fills are told apart
            void* cup = DropOverMaker(L"prop_C", L"cup");
            if (!cup) return;
            h.cup.Set(cup);
            h.cupKey[2] = ue_wrap::prop::GetKeyString(cup);
            h.sub = 1;
        } else if (h.sub == 1) {
            Flags f;
            if (h.cup.Get() || !Maker() || !ReadFlags(Maker(), f) || f.cup != 1) return;
            void* bag = DropOverMaker(L"prop_coffeeBag_C", nullptr);
            if (!bag) return;
            h.bag.Set(bag);
            h.bagKey = ue_wrap::prop::GetKeyString(bag);
            if (!CF::ReadBagAmounts(bag, h.bagAmounts0)) return FailArm("the bag's amounts did not read");
            h.sub = 2;
        } else if (h.sub == 2) {
            Flags f;
            int32_t amounts = -1;
            if (!Maker() || !ReadFlags(Maker(), f) || f.coffee != 1 || !h.bag.Get() ||
                !CF::ReadBagAmounts(h.bag.Get(), amounts) || amounts != h.bagAmounts0 - 1) return;
            Say(std::string(kFillPrefix) + "cup=" + CDD::Narrow(h.cupKey[2]) + " bag=" + CDD::Narrow(h.bagKey) +
                " amt=" + std::to_string(amounts));
            h.closable = true;
            h.sub = 3;
        }
        return;
    case 3:   // the press is the client's; the host watches for the cup its maker made
        if (h.cupKey[3].empty()) {
            h.cupKey[3] = NewCupKey(h.cupsBefore);
            if (!h.cupKey[3].empty()) Say(CupPrefix(3) + CDD::Narrow(h.cupKey[3]));
        }
        h.closable = !h.cupKey[3].empty();
        return;
    case 4:   // a client's own fill destroys the host's cup by the crossed destroy: that is the failure
        if (!h.cup.Get()) return FailArm("the host's cup was destroyed: a client's own run of the fill crossed its destroy");
        h.closable = Heard("coffee client hit 4");
        return;
    default:
        h.closable = true;
        return;
    }
}

void HostArm() {
    Host& h = g_h;
    // The client's walk or wait is its own phase, bounded by the director's deadline; the arm's bound starts when it
    // arrived.
    if (h.clientArriving && Heard(AtLine(h.arm))) {
        h.clientArriving = false;
        h.armMs = NowMs();
    }
    if ((h.arm == 4 || h.arm == 5) && Heard("coffee client inconclusive " + std::to_string(h.arm))) {
        UE_LOGW("[COFFEE] arm%d INCONCLUSIVE: the client reports it could not run the arm (its own line has why)",
                h.arm);
        h.ph = Ph::Done;
        return;
    }
    if (h.clientArriving) {
        if (NowMs() - h.armMs > kStageFailMs) return FailArm("walk: the client never reported arriving");
    } else if (NowMs() - h.armMs > kArmFailMs) {
        std::string saw;
        Sees(h.arm, saw);
        std::string why = "not closed within " + std::to_string(kArmFailMs) + " ms of its bound's start: " + saw;
        return FailArm(why.c_str());
    }
    ArmActs();
    if (h.ph != Ph::Arm) return;
    std::string saw;
    if (h.closable && !h.seen && Sees(h.arm, saw)) {
        UE_LOGI("[COFFEE] host sees arm=%d: %s", h.arm, saw.c_str());
        Say(HostSeenLine(h.arm));
        h.seen = true;
    }
    if (!h.seen || !Heard(ClientSeenLine(h.arm))) return;
    if (h.arm < kArms) OpenArm(h.arm + 1);
    else h.ph = Ph::Rejoin;
}

// The client left after its five arms: the joiner is owed the maker's record in its seed, and the host's maker
// already reads hasCup=1 hasCoffee=0 coffeeReady=0. It names the maker when the rejoined client says it is ready.
void HostRejoin(coop::net::Session* s) {
    if (!s->IsSlotWorldReady(1) || !FreshReady()) return;
    Say("coffee expect m=" + CDD::Narrow(TheCast().makerKey) + " want 1,0,0");
    g_h.ph = Ph::Done;
}

}  // namespace

void HostTick(coop::net::Session* s) {
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;
    switch (g_h.ph) {
    case Ph::Wait:   return HostWait(s, player);
    case Ph::Plug:   return HostPlug();
    case Ph::Stage:  return HostStage();
    case Ph::Arm:    return HostArm();
    case Ph::Rejoin: return HostRejoin(s);
    case Ph::Done:   return;
    }
}

bool HostOnDisconnect() {
    // The five arms are done (the host only waits for the client to leave) and the client left: the host carries on
    // to the rejoin, with the maker as it stands.
    const bool done = g_h.ph == Ph::Rejoin || (g_h.ph == Ph::Arm && g_h.arm == kArms && g_h.seen);
    if (done) {
        g_h.ph = Ph::Rejoin;
        return true;
    }
    g_h = Host{};
    return false;
}

}  // namespace coop::dev::coffee_drill::detail
