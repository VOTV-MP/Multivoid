// harness/autotest/autotest_sv_request.cpp -- TEST-ONLY: the server information sv.request writes onto a disc, across two peers.
//
// One peer, VOTVCOOP_SV_REQUEST_WHO (host or client), types the line at its own terminal, ejects the disc and inserts it in the
// laptop. Both peers write what they hold at each step, and the other compares: the server box's slot, the disc, the laptop's
// slot, and whether the rows name this peer's own dish code. Gated by VOTVCOOP_RUN_SV_REQUEST; the folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/floppy_disc.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/desk/dish.h"
#include "ue_wrap/desk/sat_console.h"
#include "ue_wrap/devices/floppy_slot.h"
#include "ue_wrap/devices/laptop.h"
#include "ue_wrap/devices/serverbox.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace D = ue_wrap::dish;
namespace E = ue_wrap::engine;
namespace FD = ue_wrap::floppy_disc;
namespace FS = ue_wrap::floppy_slot;
namespace R = ue_wrap::reflection;
namespace SB = ue_wrap::serverbox;
namespace SC = ue_wrap::sat_console;

std::string g_dir;
int g_fails = 0;
bool g_isClient = false;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void Mark(const std::string& name, const std::string& body = "x") {
    std::string tmp = g_dir + "\\" + name + ".tmp";
    { std::ofstream(tmp, std::ios::binary | std::ios::trunc) << body; }
    ::MoveFileExA(tmp.c_str(), (g_dir + "\\" + name).c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool Await(const std::string& name, std::string* body = nullptr, int seconds = 120) {
    for (int i = 0; i < seconds * 10; ++i) {
        std::ifstream f(g_dir + "\\" + name, std::ios::binary);
        std::string s;
        if (f && std::getline(f, s) && !s.empty()) {
            if (body) *body = s;
            return true;
        }
        ::Sleep(100);
    }
    UE_LOGW("sv-request: VERDICT ABORT -- no '%hs' from the other peer", name.c_str());
    return false;
}

void Check(bool ok, const char* what) {
    UE_LOGI("sv-request: %hs %hs", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

// A disc's or a slot's rows as one line: their count and a digest of every character.
std::string Digest(const std::vector<std::wstring>& rows, int32_t writes) {
    uint64_t h = 1469598103934665603ull;
    for (const std::wstring& r : rows) {
        for (wchar_t c : r) {
            h ^= static_cast<uint64_t>(c);
            h *= 1099511628211ull;
        }
        h ^= 0xFFFF;
        h *= 1099511628211ull;
    }
    char b[96];
    std::snprintf(b, sizeof(b), "rows=%zu writes=%d digest=%016llx", rows.size(), writes, static_cast<unsigned long long>(h));
    return b;
}

void LogRows(const char* where, const std::vector<std::wstring>& rows) {
    for (size_t i = 0; i < rows.size() && i < 3; ++i) {
        std::wstring shown = rows[i].substr(0, 110);
        for (wchar_t& c : shown)
            if (c == L'\r' || c == L'\n') c = L'|';
        UE_LOGI("sv-request: %hs row %zu len=%zu '%ls'", where, i, rows[i].size(), shown.c_str());
    }
}

void* ServerOf(void* dish) {
    const int32_t off = dish ? R::FindPropertyOffset(R::ClassOf(dish), L"server") : -1;
    return off < 0 ? nullptr : *reinterpret_cast<void**>(static_cast<uint8_t*>(dish) + off);
}

// The first dish with a server, by its index: the same on both peers.
int PickDish() {
    const int n = D::Count();
    for (int i = 0; i < n; ++i)
        if (ServerOf(D::DishByIndex(i))) return i;
    return -1;
}

bool ReadSlot(FS::DeviceKind kind, void* device, std::vector<std::wstring>& rows, int32_t& writes, int32_t& type) {
    FS::Scalars s;
    FS::Content c;
    if (!FS::ReadScalars(kind, device, s) || !FS::ReadContent(kind, device, c)) return false;
    rows = c.data;
    writes = s.readWrites;
    type = s.floppyType;
    return true;
}

// This peer's reading of a step: what it holds, for the other peer to compare with its own.
std::string StepBox(void* box) {
    std::vector<std::wstring> rows;
    int32_t writes = 0, type = -1;
    std::string d = "unread";
    OnGameThread([&] {
        if (ReadSlot(FS::DeviceKind::ServerBox, box, rows, writes, type)) d = "type=" + std::to_string(type) + " " + Digest(rows, writes);
    });
    return d;
}

void* FindDisc(const std::wstring& key) {
    void* found = nullptr;
    for (void* o : R::FindObjectsByClass(L"prop_floppyDisc_R_C"))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__") && ue_wrap::prop::GetKeyString(o) == key) found = o;
    return found;
}

// The discs that stand now, by key, of any class of the family.
std::vector<std::wstring> DiscKeys() {
    std::vector<std::wstring> keys;
    for (void* o : R::FindObjectsByClass(L"prop_floppyDisc_R_C"))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) keys.push_back(ue_wrap::prop::GetKeyString(o));
    return keys;
}

// Compares this peer's reading with the other's: each writes its own and reads the other's.
void CompareStep(const std::string& step, const std::string& mine, const char* what) {
    const std::string me = g_isClient ? "c_" : "h_", other = g_isClient ? "h_" : "c_";
    Mark(me + step, mine);
    std::string theirs;
    if (!Await(other + step, &theirs, 60)) return;
    UE_LOGI("sv-request: %hs: here %hs, there %hs", step.c_str(), mine.c_str(), theirs.c_str());
    Check(mine == theirs, what);
}

void Run(bool iAct) {
    // The host makes the fixture: a blank disc in the first dish's server, as a player's insert does.
    int dishIndex = -1;
    void* box = nullptr;
    // The pawn stands before the dishes do.
    for (int i = 0; i < 240 && !box; ++i) {
        OnGameThread([&] {
            FS::EnsureResolved(FS::DeviceKind::ServerBox);
            FS::EnsureResolved(FS::DeviceKind::Laptop);
            FD::EnsureResolved();
            SB::EnsureResolved();
            D::EnsureResolved();
            SC::EnsureResolved();
            ue_wrap::laptop::EnsureResolved();
            dishIndex = PickDish();
            box = dishIndex >= 0 ? ServerOf(D::DishByIndex(dishIndex)) : nullptr;
        });
        if (!box) ::Sleep(500);
    }
    Check(box != nullptr, "a dish has a server box");
    if (!box) return;
    UE_LOGI("sv-request: dish %d", dishIndex);

    if (!g_isClient) {
        Await("client_ready", nullptr, 480);
        OnGameThread([&] {
            void* cls = R::FindClass(L"prop_floppyDisc_R_C");
            void* pawn = coop::players::Registry::Get().Local();
            ue_wrap::FVector at{};
            if (!cls || !pawn || !E::TryGetActorLocation(pawn, at)) return;
            void* disc = E::SpawnActor(cls, {at.X + 100.f, at.Y, at.Z + 50.f});
            if (!disc) return;
            FD::DiscContent content;
            FD::ReadDiscContent(disc, content);
            UE_LOGI("sv-request: a new disc reads writes=%d rows=%zu", content.readWrites, content.data.size());
            if (content.readWrites <= 0) content.readWrites = 5;
            // VOTVCOOP_SV_REQUEST_SEED: the rows a player's earlier requests left, one per other dish, each as long as a real one.
            const int seed = std::atoi(coop::config::ReadEnv("VOTVCOOP_SV_REQUEST_SEED").c_str());
            for (int i = 0; i < seed && dishIndex + 1 + i < D::Count(); ++i) {
                std::wstring code;
                D::ReadHashcode(dishIndex + 1 + i, code);
                const std::wstring eol(L"\r\n");
                content.data.push_back(L"$t" + eol + L"Server data: /n name ; /d data" + eol + L"/n " + D::TechName(dishIndex + 1 + i) + eol + L"/d 00" + eol + code);
            }
            UE_LOGI("sv-request: the disc holds %zu rows before the request", content.data.size());
            FD::WriteDiscContent(disc, content);
            const bool called = SB::CallProcessFloppy(box, disc);
            UE_LOGI("sv-request: the insert was called (%d)", called ? 1 : 0);
        });
    } else {
        Mark("client_ready");
    }
    ::Sleep(3000);
    CompareStep("inserted", StepBox(box), "the blank disc in the slot reads the same on both peers");
    if (iAct) {
        OnGameThread([&] {
            void* term = SC::LocalTerminal();
            void* dish = D::DishByIndex(dishIndex);
            if (!term || !dish) return;
            SC::CallInit(term, D::TechName(dishIndex), false, dish);
            SC::RunCommand(term, L"clr");
            SC::RunCommand(term, L"sv.request");
            UE_LOGI("sv-request: typed sv.request at dish %d", dishIndex);
        });
    }
    // Export runs ten packets, each a coin toss on a low upgrade: about fifteen seconds and sometimes more.
    std::vector<std::wstring> rows;
    int32_t writes = 0, type = -1;
    const size_t wantRows = static_cast<size_t>(std::atoi(coop::config::ReadEnv("VOTVCOOP_SV_REQUEST_SEED").c_str())) + 1;
    for (int i = 0; i < 900 && rows.size() < wantRows; ++i) {
        OnGameThread([&] { ReadSlot(FS::DeviceKind::ServerBox, box, rows, writes, type); });
        if (rows.size() < wantRows) ::Sleep(100);
    }
    UE_LOGI("sv-request: this peer's slot holds %zu rows, want %zu", rows.size(), wantRows);
    Check(rows.size() >= wantRows, "the export wrote a row into this peer's slot");
    LogRows("slot", rows);
    bool namesCode = false;
    OnGameThread([&] {
        std::wstring code;
        D::ReadHashcode(dishIndex, code);
        for (const std::wstring& r : rows)
            if (!code.empty() && r.find(code) != std::wstring::npos) namesCode = true;
        UE_LOGI("sv-request: this peer's dish code has %zu characters", code.size());
    });
    Check(namesCode, "the row holds this peer's own code for that dish");
    ::Sleep(3000);
    CompareStep("exported", StepBox(box), "the exported rows read the same on both peers");

    std::vector<std::wstring> before;
    OnGameThread([&] { before = DiscKeys(); });
    if (iAct) {
        OnGameThread([&] { SB::CallEjectFloppy(box); });
        UE_LOGI("sv-request: ejected");
    }
    std::wstring discKey;
    void* disc = nullptr;
    for (int i = 0; i < 150 && !disc; ++i) {
        OnGameThread([&] {
            for (const std::wstring& k : DiscKeys()) {
                bool had = false;
                for (const std::wstring& b : before) had = had || b == k;
                if (!had && !k.empty()) {
                    discKey = k;
                    disc = FindDisc(k);
                }
            }
        });
        if (!disc) ::Sleep(100);
    }
    Check(disc != nullptr, "the ejected disc stands on this peer");
    if (!disc) return;
    ::Sleep(3000);
    std::string discText = "unread";
    OnGameThread([&] {
        FD::DiscContent c;
        if (FD::ReadDiscContent(disc, c)) {
            discText = Digest(c.data, c.readWrites);
            LogRows("disc", c.data);
        }
    });
    CompareStep("ejected", discText, "the ejected disc's rows read the same on both peers");

    if (iAct) {
        OnGameThread([&] { ue_wrap::laptop::CallInsertDisc(disc); });
        UE_LOGI("sv-request: inserted in the laptop");
    }
    ::Sleep(5000);
    std::string laptopText = "unread";
    OnGameThread([&] {
        void* laptop = nullptr;
        for (void* o : R::FindObjectsByClass(L"laptop_C"))
            if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) laptop = o;
        std::vector<std::wstring> lr;
        int32_t lw = 0, lt = -1;
        if (laptop && ReadSlot(FS::DeviceKind::Laptop, laptop, lr, lw, lt)) {
            laptopText = "type=" + std::to_string(lt) + " " + Digest(lr, lw);
            LogRows("laptop", lr);
        }
    });
    Check(laptopText.find("rows=0") == std::string::npos && laptopText != "unread", "the laptop holds the rows on this peer");
    CompareStep("laptop", laptopText, "the laptop's rows read the same on both peers");
    UE_LOGI("sv-request: DONE fails=%d", g_fails);
}

}  // namespace

DWORD WINAPI SvRequestThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("sv-request: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    g_isClient = IsClientRole();
    const bool clientActs = coop::config::ReadEnv("VOTVCOOP_SV_REQUEST_WHO") == "client";
    if (g_isClient) {
        bool ready = false;
        for (int i = 0; i < 480 && !ready; ++i) {
            OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
            if (!ready) ::Sleep(500);
        }
        ::Sleep(8000);
    } else {
        void* pawn = nullptr;
        for (int i = 0; i < 480 && !pawn; ++i) {
            OnGameThread([&] { pawn = coop::players::Registry::Get().Local(); });
            if (!pawn) ::Sleep(500);
        }
    }
    Run(g_isClient == clientActs);
    return 0;
}

}  // namespace harness::autotest
