// harness/autotest/autotest_disc_race.cpp -- TEST-ONLY: two players put a disc in one server inside the same poll.
//
// The host's insert lands first and the client's claim names another disc. Neither disc may be lost: each carries a row of its own,
// and both peers must find both rows, in the slot or on a disc standing in the world. Gated by VOTVCOOP_RUN_DISC_RACE; the folder
// is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/floppy_disc.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/floppy_slot.h"
#include "ue_wrap/devices/serverbox.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace E = ue_wrap::engine;
namespace FD = ue_wrap::floppy_disc;
namespace FS = ue_wrap::floppy_slot;
namespace R = ue_wrap::reflection;
namespace SB = ue_wrap::serverbox;

const wchar_t* const kHostClass = L"prop_floppyDisc_R_C";
const wchar_t* const kClientClass = L"prop_floppyDisc_G_C";
const wchar_t* const kHostRow = L"race row of the host's disc";
const wchar_t* const kClientRow = L"race row of the client's disc";

std::string g_dir;
int g_fails = 0;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void Mark(const char* name, const std::string& body = "x") {
    std::string tmp = g_dir + "\\" + name + ".tmp";
    { std::ofstream(tmp, std::ios::binary | std::ios::trunc) << body; }
    ::MoveFileExA(tmp.c_str(), (g_dir + "\\" + name).c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool Await(const char* name, std::string* body = nullptr, int seconds = 300, int stepMs = 100) {
    for (int i = 0; i < seconds * 1000 / stepMs; ++i) {
        std::ifstream f(g_dir + "\\" + name, std::ios::binary);
        std::string s;
        if (f && std::getline(f, s) && !s.empty()) {
            if (body) *body = s;
            return true;
        }
        ::Sleep(stepMs);
    }
    UE_LOGW("disc-race: VERDICT ABORT -- no '%hs' from the other peer", name);
    return false;
}

void Check(bool ok, const char* what) {
    UE_LOGI("disc-race: %hs %hs", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

std::vector<void*> Live(const wchar_t* cls) {
    std::vector<void*> out;
    for (void* o : R::FindObjectsByClass(cls))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) out.push_back(o);
    return out;
}

void* Newest(const wchar_t* cls, const std::vector<void*>& before) {
    for (void* o : Live(cls)) {
        bool had = false;
        for (void* b : before) had = had || b == o;
        if (!had) return o;
    }
    return nullptr;
}

void* ByKey(const wchar_t* cls, const std::string& key) {
    const std::wstring want(key.begin(), key.end());
    for (void* o : Live(cls))
        if (ue_wrap::prop::GetKeyString(o) == want) return o;
    return nullptr;
}

void WriteRow(void* disc, const wchar_t* row) {
    FD::DiscContent c;
    FD::ReadDiscContent(disc, c);
    c.data.assign(1, row);
    FD::WriteDiscContent(disc, c);
}

// Whether `row` is in the box's slot, and on how many discs standing in the world.
void FindRow(void* box, const wchar_t* row, bool& inSlot, int& standing) {
    inSlot = false;
    standing = 0;
    FS::Content slot;
    FS::Scalars st;
    if (FS::ReadScalars(FS::DeviceKind::ServerBox, box, st) && st.floppyType >= 0 && FS::ReadContent(FS::DeviceKind::ServerBox, box, slot))
        for (const std::wstring& r : slot.data) inSlot = inSlot || r == row;
    for (const wchar_t* cls : {kHostClass, kClientClass}) {
        for (void* o : Live(cls)) {
            FD::DiscContent c;
            if (!FD::ReadDiscContent(o, c)) continue;
            for (const std::wstring& r : c.data)
                if (r == row) ++standing;
        }
    }
}

}  // namespace

DWORD WINAPI DiscRaceThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("disc-race: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    const bool client = IsClientRole();
    void* box = nullptr;
    for (int i = 0; i < 480 && !box; ++i) {
        OnGameThread([&] {
            FS::EnsureResolved(FS::DeviceKind::ServerBox);
            FD::EnsureResolved();
            SB::EnsureResolved();
            std::vector<void*> boxes;
            SB::ReadServers(boxes);
            bool ready = !client || coop::net_pump::HasAnnouncedWorldReady();
            if (ready && !boxes.empty()) box = boxes[0];
        });
        if (!box) ::Sleep(500);
    }
    if (!box) return 0;

    void* mine = nullptr;
    if (!client) {
        if (!Await("client_ready", nullptr, 480)) return 0;
        std::string hostKey, clientKey;
        OnGameThread([&] {
            void* pawn = coop::players::Registry::Get().Local();
            ue_wrap::FVector at{};
            if (!pawn || !E::TryGetActorLocation(pawn, at)) return;
            mine = E::SpawnActor(R::FindClass(kHostClass), {at.X + 100.f, at.Y, at.Z + 50.f});
            void* theirs = E::SpawnActor(R::FindClass(kClientClass), {at.X + 100.f, at.Y + 60.f, at.Z + 50.f});
            if (mine) WriteRow(mine, kHostRow);
            if (theirs) clientKey = [&] { const std::wstring k = ue_wrap::prop::GetKeyString(theirs); return std::string(k.begin(), k.end()); }();
        });
        Check(mine != nullptr && !clientKey.empty(), "the host spawned both discs");
        if (!mine) return 0;
        Mark("keys", clientKey);
        if (!Await("armed")) return 0;
        // The same poll: the host's insert, and the word that makes the client insert as well.
        // VOTVCOOP_DISC_RACE_DELAY: the client goes first by this many ms, so its claim can find the slot filled.
        const int delayMs = std::atoi(coop::config::ReadEnv("VOTVCOOP_DISC_RACE_DELAY").c_str());
        if (delayMs > 0) {
            Mark("go");
            ::Sleep(static_cast<DWORD>(delayMs));
        }
        OnGameThread([&] { SB::CallProcessFloppy(box, mine); });
        if (delayMs <= 0) Mark("go");
        UE_LOGI("disc-race: the host inserted");
    } else {
        ::Sleep(8000);
        Mark("client_ready");
        std::string key;
        if (!Await("keys", &key)) return 0;
        for (int i = 0; i < 200 && !mine; ++i) {
            OnGameThread([&] { mine = ByKey(kClientClass, key); });
            if (!mine) ::Sleep(100);
        }
        Check(mine != nullptr, "the client's disc crossed");
        if (!mine) return 0;
        OnGameThread([&] { WriteRow(mine, kClientRow); });
        Mark("armed");
        if (!Await("go", nullptr, 60, 10)) return 0;
        OnGameThread([&] { SB::CallProcessFloppy(box, mine); });
        UE_LOGI("disc-race: the client inserted");
    }

    ::Sleep(9000);
    bool hostSlot = false, clientSlot = false;
    int hostStanding = 0, clientStanding = 0;
    OnGameThread([&] {
        FindRow(box, kHostRow, hostSlot, hostStanding);
        FindRow(box, kClientRow, clientSlot, clientStanding);
    });
    UE_LOGI("disc-race: the host's disc: in the slot %d, standing %d; the client's disc: in the slot %d, standing %d",
            hostSlot ? 1 : 0, hostStanding, clientSlot ? 1 : 0, clientStanding);
    Check(hostSlot || hostStanding == 1, "the host's disc survives, once");
    Check(clientSlot || clientStanding == 1, "the client's disc survives, once");
    Check(!(hostSlot && hostStanding > 0) && !(clientSlot && clientStanding > 0), "neither disc is both in the slot and in the world");
    Check(hostSlot != clientSlot, "exactly one disc holds the slot");
    Mark(client ? "client_done" : "host_done");
    Await(client ? "host_done" : "client_done", nullptr, 30);
    UE_LOGI("disc-race: DONE fails=%d", g_fails);
    return 0;
}

}  // namespace harness::autotest
