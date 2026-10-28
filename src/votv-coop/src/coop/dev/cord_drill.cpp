// coop/dev/cord_drill.cpp -- see coop/dev/cord_drill.h. The drill's switch, its chat edges and its failure line; the
// roles are cord_drill_host.cpp and cord_drill_client.cpp.

#include "coop/dev/cord_drill.h"

#include "cord_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/drill_chat.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"

#include <atomic>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::dev::cord_drill {
namespace {

namespace DC = coop::dev::drill_chat;
namespace D = detail;

std::atomic<coop::net::Session*> g_session{nullptr};

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::cord_drill);
    return s;
}

// The rows of the lobby chat are six live ones, each retiring as the next come: every line either role waits for is
// latched on sight, from the first tick, so a peer busy walking does not lose an edge.
std::unordered_set<std::string> g_heard;
std::unordered_map<std::string, std::string> g_rests;

const std::vector<std::string>& Watched() {
    static const std::vector<std::string> lines = [] {
        std::vector<std::string> v;
        for (int n = 1; n <= D::kArms; ++n) {
            v.push_back(D::ArmLine(n));
            v.push_back(D::HostSeenLine(n));
            v.push_back(D::ClientSeenLine(n));
            v.push_back(D::WalkedLine(n));
        }
        for (const char* l : {"cord client staged", "cord client dragged 4", "cord host released 4",
                              "cord client held 5", "cord host plugged 5"})
            v.push_back(l);
        return v;
    }();
    return lines;
}

const char* const kRestPrefixes[] = {"cord keys ", "cord socks ", "cord expect ", "cord client ready "};

}  // namespace

namespace detail {

bool Heard(const std::string& line) { return g_heard.count(line) != 0; }

bool Rest(const std::string& prefix, std::string* rest) {
    const auto it = g_rests.find(prefix);
    if (it == g_rests.end()) return false;
    *rest = it->second;
    return true;
}

void Say(const std::string& line) { DC::Say(line); }

std::string ArmLine(int n) { return "cord host arm " + std::to_string(n); }
std::string HostSeenLine(int n) { return "cord host seen " + std::to_string(n); }
std::string ClientSeenLine(int n) { return "cord client seen " + std::to_string(n); }
std::string WalkedLine(int n) { return "cord client walked " + std::to_string(n); }

std::string Narrow(const std::wstring& w) {
    std::string out;
    out.reserve(w.size());
    for (wchar_t c : w) out.push_back(c < 128 ? static_cast<char>(c) : '?');
    return out;
}

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::string Token(const std::string& rest, const char* tag) {
    const size_t at = rest.find(tag);
    if (at == std::string::npos) return {};
    const size_t from = at + std::char_traits<char>::length(tag);
    const size_t to = rest.find(' ', from);
    return rest.substr(from, to == std::string::npos ? std::string::npos : to - from);
}

void FailLine(const char* peer, const char* arm, const char* why) {
    UE_LOGW("[CORD] FAIL: %s: %s | %s link %s", arm, why, peer, LinkText().c_str());
}

uint64_t NowMs() { return coop::net::NowMs(); }

}  // namespace detail

void Install(coop::net::Session* s) {
    if (!Enabled()) return;
    g_session.store(s, std::memory_order_release);
}

void Tick() {
    if (!Enabled()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (DC::PollDue()) {
        for (const std::string& line : Watched())
            if (!g_heard.count(line) && DC::ChatHasLive(line)) g_heard.insert(line);
        for (const char* prefix : kRestPrefixes) {
            std::string rest;
            if (DC::LiveRestOf(prefix, &rest)) g_rests[prefix] = rest;
        }
    }
    if (s->role() == coop::net::Role::Host) D::HostTick(s);
    else if (s->connected()) D::ClientTick(s);
}

void OnDisconnect() {
    if (!Enabled()) return;
    g_heard.clear();
    g_rests.clear();
    D::ClientReset();
    if (!D::HostOnDisconnect()) D::ResetCast();
}

}  // namespace coop::dev::cord_drill
