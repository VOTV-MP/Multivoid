// harness/compat_boot.cpp -- see harness/compat_boot.h.

#include "harness/compat_boot.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/game_build/game_build.h"
#include "coop/net/protocol.h"
#include "coop/version.h"
#include "harness/compat_report.h"
#include "harness/fingerprint_census.h"
#include "harness/sdk_check.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/scan_ledger.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace harness::compat_boot {
namespace {

namespace CR = harness::compat_report;
namespace GB = coop::game_build;
namespace SL = ue_wrap::scan_ledger;

// What a missed scan costs, by row: the report's knowledge, kept out of the engine layer (principle 7).
struct ScanCost {
    const char*  token;     // the row's name for the red arm: [dev] scan_ledger_red=<token>
    CR::Severity severity;  // of a miss
    const char*  missTakes;
    const char*  notTried;  // why a row may be untried this far into a session
};
constexpr ScanCost kCosts[] = {
    {"GUObjectArray", CR::Severity::Critical, "everything: the mod stands down", "not tried yet"},
    {"FNameToString", CR::Severity::Critical, "everything: the mod stands down", "not tried yet"},
    {"ProcessEvent", CR::Severity::Critical, "everything: the mod stands down", "not tried yet"},
    {"FMemoryRealloc", CR::Severity::Critical,
     "no engine-heap allocation or free: the engine arrays and strings the mod writes are refused, the buffers it "
     "frees leak, and growing the engine's object-notification lists stands the mod down",
     "not tried yet"},
    {"ScriptGate", CR::Severity::Critical,
     "no multiplayer session starts (MV-J32), since a prop's key is never seen written, and Blueprint-internal "
     "calls are neither watched nor refused: every script_gate watch is off",
     "not tried yet"},
    {"SaveGameToSlot", CR::Severity::Important,
     "the world-save gate: a client in a session can save the host's world over its own",
     "tried when a multiplayer session starts"},
    {"OverlayDraw", CR::Severity::Important, "the overlay: no Multivoid menu, chat, HUD or dialogs",
     "tried when the overlay starts"},
    {"OverlayResize", CR::Severity::Important,
     "a window resize or a fullscreen switch can end the game in an engine Fatal (the overlay still holds the "
     "back buffer)",
     "tried when the overlay starts"},
    {"ActorEndPlay", CR::Severity::Important,
     "actors ending play go unseen: synced entities the world destroyed stay registered", "not tried yet"},
    {"VulkanPresent", CR::Severity::Important,
     "under the Vulkan renderer the overlay has no draw seam: no Multivoid menu, chat, HUD or dialogs",
     "tried when the overlay starts"},
    {"VulkanPixelFormats", CR::Severity::Important,
     "under the Vulkan renderer the overlay cannot learn the back buffer's format: no Multivoid menu, chat, HUD "
     "or dialogs",
     "tried when the overlay starts"},
};
static_assert(sizeof kCosts / sizeof kCosts[0] == static_cast<size_t>(SL::Scan::kCount), "a cost per Scan");

std::string             g_standDownWhy;     // set once, on a stand-down; the boot thread only
std::atomic<uint32_t>   g_writtenVersion{0};  // the ledger version the engine section last showed
std::atomic<bool>       g_passed{false};     // write 2 ran on a boot that passed: refreshes are owed

std::string HashHex(const uint8_t* bytes, size_t n) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        out.push_back(kHex[bytes[i] >> 4]);
        out.push_back(kHex[bytes[i] & 0xF]);
    }
    return out;
}

// What runs, for a person: "VOTV <version> -- a newer version, untested." and its kin.
std::string RunningSentence(const GB::Detected& d) {
    if (!d.row) return "an untested version: " + GB::UntestedDetail() + ".";
    const std::string name = std::string("VOTV ") + d.row->display;
    switch (d.kind) {
    case GB::Kind::Target:   return name + " (the target).";
    case GB::Kind::Newer:    return name + " -- a newer version, untested.";
    case GB::Kind::Older:    return name + " -- an older version, untested.";
    case GB::Kind::Untested: break;
    }
    return name + ".";
}

// The engine section: the boot's health check, passed or why the mod stood down, then every scan row with
// its state and, for a miss, what it takes away. The red arm reads one row as missed (the report only).
void EngineSection() {
    const std::string red = coop::config::ResolveString(coop::config_registry::rows::scan_ledger_red);
    g_writtenVersion.store(SL::Version(), std::memory_order_release);
    CR::Begin(kEngineSection);
    CR::Line(kEngineSection, "=== Engine ===");
    if (g_standDownWhy.empty()) {
        CR::Line(kEngineSection, "The boot's health check passed: the engine's objects, names and calls read as the "
                                 "profile says.");
    } else {
        CR::Line(kEngineSection, "Multivoid did not start: " + g_standDownWhy);
        CR::Line(kEngineSection,
                 "The game runs on without it. multivoid.log has the details (look for 'STANDING DOWN').");
        CR::Failure(kEngineSection, CR::Severity::Critical);
    }
    CR::Line(kEngineSection, "What the mod finds by searching the game's code:");
    bool redSeen = red.empty();
    for (size_t i = 0; i < static_cast<size_t>(SL::Scan::kCount); ++i) {
        const auto scan = static_cast<SL::Scan>(i);
        const ScanCost& cost = kCosts[i];
        SL::State state = SL::StateOf(scan);
        if (!red.empty() && red == cost.token) {
            state = SL::State::Missed;
            redSeen = true;
            static std::atomic<bool> s_said{false};
            if (!s_said.exchange(true)) UE_LOGW("scan-ledger: RED ARM %s reported missed", cost.token);
        }
        switch (state) {
        case SL::State::Found:
            CR::Line(kEngineSection, std::string("  found      ") + SL::Name(scan));
            break;
        case SL::State::Missed:
            CR::Line(kEngineSection, std::string("  MISSED     ") + SL::Name(scan) + " -- " + cost.missTakes);
            CR::Failure(kEngineSection, cost.severity);
            break;
        case SL::State::NotTried:
            CR::Line(kEngineSection, std::string("  not tried  ") + SL::Name(scan) + " -- " + cost.notTried);
            break;
        }
    }
    if (!redSeen) {
        static std::atomic<bool> s_said{false};
        if (!s_said.exchange(true)) UE_LOGE("scan-ledger: RED ARM FAILED %s (no scan row has that name)", red.c_str());
    }
}

}  // namespace

void WriteBuild() {
    const GB::Detected& d = GB::Running();
    char made[128];
    std::snprintf(made, sizeof made, "Multivoid %s b%u is made for VOTV %s.", coop::version::kGameTarget,
                  static_cast<unsigned>(coop::net::kProtocolVersion), coop::version::kGameTarget);
    const std::string pak = d.id.pakRead ? HashHex(d.id.pakHash, sizeof d.id.pakHash)
                                         : std::string("not read (") + (d.id.pakProblem ? d.id.pakProblem : "?") + ")";
    char stamp[16];
    std::snprintf(stamp, sizeof stamp, "%08x", d.id.exeStamp);
    CR::Begin(kBuildSection);
    CR::Line(kBuildSection, "=== Game build ===");
    CR::Line(kBuildSection, made);
    CR::Line(kBuildSection, "The game running: " + RunningSentence(d));
    CR::Line(kBuildSection, std::string("Identity: exe ") + stamp + ", pak index " + pak +
                                (d.forced ? "  [FORCED by [dev] game_build_force]" : ""));
    if (d.kind == GB::Kind::Target) {
        CR::SetVerdictNote({});
    } else {
        const std::string running =
            d.row ? std::string("VOTV ") + d.row->display + ", untested" : std::string("an untested version");
        CR::SetVerdictNote("the game running is " + running);
    }
    CR::Pending(kEngineSection, "engine: the boot's health check is running");
    CR::Write();
}

void WritePassed() {
    EngineSection();
    CR::Pending(harness::sdk_check::kReportSection, "sdk-check: runs when the first gameplay world has loaded");
    CR::Pending(harness::fingerprint_census::kReportSection,
                "fingerprint-census: runs when the first gameplay world has loaded");
    CR::Write();
    g_passed.store(true, std::memory_order_release);
}

void WriteStandDown(const std::string& why) {
    g_standDownWhy = why;
    EngineSection();
    CR::Write();
}

void RefreshIfScansMoved() {
    if (!g_passed.load(std::memory_order_acquire)) return;
    if (ue_wrap::scan_ledger::Version() == g_writtenVersion.load(std::memory_order_acquire)) return;
    EngineSection();
    CR::Write(/*announce=*/false);
    UE_LOGI("compat-report: the engine section was rewritten -- a scan was decided after the boot");
}

}  // namespace harness::compat_boot
