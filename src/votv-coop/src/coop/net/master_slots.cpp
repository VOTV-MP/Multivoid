// coop/net/master_slots.cpp -- see coop/net/master_slots.h.

#include "coop/net/master_slots.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"   // kOfficialMasterSlots, kSignalingPort
#include "ue_wrap/core/log.h"

#include <cstdlib>
#include <mutex>
#include <string_view>

namespace coop::net::master_slots {
namespace {

// Every earlier build knows one master, this address, so old and new builds meet on the default
// slot while those cohorts play: the first official slot may be renamed, never moved.
constexpr std::string_view FirstAddress(std::string_view list) {
    const std::string_view first = list.substr(0, list.find(','));
    const size_t eq = first.find('=');
    return eq == std::string_view::npos ? first : first.substr(eq + 1);
}
static_assert(FirstAddress(kOfficialMasterSlots) == "master.multivoid.dev:10443",
              "the default slot must stay on the address every earlier build pins");

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

bool EqualsNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// A master address's host: the scheme and any path dropped, then everything before the port.
std::string HostOf(const std::string& url) {
    std::string s = url;
    const size_t scheme = s.find("://");
    if (scheme != std::string::npos) s = s.substr(scheme + 3);
    const size_t slash = s.find('/');
    if (slash != std::string::npos) s.resize(slash);
    const size_t colon = s.rfind(':');
    if (colon != std::string::npos) s.resize(colon);
    return s;
}

// Empty when the HTTP client will dial it (http_client.cpp's grammar: an optional http:// or
// https://, a host, a port a master always names, and nothing else); otherwise what is wrong.
std::string AddressProblem(const std::string& url) {
    std::string s = url;
    const size_t scheme = s.find("://");
    if (scheme != std::string::npos) {
        std::string proto = s.substr(0, scheme);
        for (char& c : proto) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (proto != "http" && proto != "https") return "an unknown scheme";
        s = s.substr(scheme + 3);
    }
    for (char c : s)
        if (static_cast<unsigned char>(c) <= ' ' || c == 0x7F) return "a space in the address";
    if (s.find('/') != std::string::npos) return "a path after the port";
    const size_t colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0) return "no host:port";
    const std::string port = s.substr(colon + 1);
    if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos)
        return "a bad port";
    const unsigned long n = std::strtoul(port.c_str(), nullptr, 10);
    if (n == 0 || n > 65535) return "a bad port";
    return {};
}

// A label is one tab's text and is written back to the ini as the choice, so it is printable
// ASCII and carries none of the characters this list's grammar or the ini's comment stripping
// would cut on.
std::string LabelProblem(const std::string& label) {
    if (label.empty()) return "an empty label";
    if (label.size() > kMaxLabelChars)
        return "a label over " + std::to_string(kMaxLabelChars) + " characters";
    for (char c : label) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u > 0x7E) return "a label outside printable ASCII";
        if (c == ';' || c == '#' || c == '=' || c == ',') return "a label with ; # = or ,";
    }
    return {};
}

struct State {
    std::vector<Slot> slots;
    int selected = 0;
};
std::once_flag g_once;
std::mutex g_mu;
State g_state;

bool IsOfficial(const std::string& url) {
    static const std::vector<Slot> kOfficial = Parse(kOfficialMasterSlots, nullptr);
    for (const Slot& s : kOfficial)
        if (EqualsNoCase(s.url, url)) return true;
    return false;
}

// Un-gated and pure, at Init: a wrong verdict here does not crash, it silently drops a player's
// master or shows one that cannot be dialled, which reads as a network fault.
void RunSelftest() {
    struct Case { const char* text; size_t slots; size_t rejected; const char* firstLabel; const char* what; };
    static const Case kCases[] = {
        {kOfficialMasterSlots, 2, 0, "USA", "the official list"},
        {"http://127.0.0.1:5555", 1, 0, "127.0.0.1", "a bare address is a list of one"},
        {"A=h:1, a=h:2", 1, 1, "A", "a label repeated in another case"},
        {"A=h", 0, 1, nullptr, "an address without a port"},
        {"A=h:1/v1", 0, 1, nullptr, "a path after the port"},
        {"=h:1", 0, 1, nullptr, "an empty label"},
        {"A label much too long=h:1", 0, 1, nullptr, "a label past the bound"},
        {"A=ftp://h:1", 0, 1, nullptr, "an unknown scheme"},
        {"A=h:1,,B=http://h:2,", 2, 0, "A", "empty entries are skipped"},
        {"A=h:1,B=h:2,C=h:3,D=h:4,E=h:5,F=h:6,G=h:7", kMaxSlots, 1, "A", "the slot bound"},
        {"A=h:99999", 0, 1, nullptr, "a port out of range"},
    };
    int bad = 0;
    for (const Case& c : kCases) {
        std::vector<std::string> rej;
        const std::vector<Slot> got = Parse(c.text, &rej);
        const bool ok = got.size() == c.slots && rej.size() == c.rejected &&
                        (!c.firstLabel || (!got.empty() && got[0].label == c.firstLabel));
        if (!ok) {
            UE_LOGE("master_slots selftest FAIL: %s -- '%s' gave %zu slot(s), %zu rejected",
                    c.what, c.text, got.size(), rej.size());
            ++bad;
        }
    }
    if (bad == 0)
        UE_LOGI("master_slots selftest: ALL PASS (%zu cases)", sizeof(kCases) / sizeof(kCases[0]));
}

void InitOnce() {
    RunSelftest();
    std::vector<std::string> rejected;
    const std::string text =
        ::coop::config::ResolveString(::coop::config_registry::rows::net_masters);
    std::vector<Slot> slots = Parse(text, &rejected);
    for (const std::string& r : rejected)
        UE_LOGW("master_slots: a net.masters entry was skipped -- %s", r.c_str());
    if (slots.empty()) {
        UE_LOGW("master_slots: net.masters names no usable master -- using the official list");
        slots = Parse(kOfficialMasterSlots, nullptr);
    }
    // An unknown label is not an error: the list may have changed under a remembered choice (a
    // lab list from the environment, a slot the player removed), and the default is the answer.
    const std::string want =
        Trim(::coop::config::ResolveString(::coop::config_registry::rows::net_master));
    int selected = 0;
    bool found = want.empty();
    for (size_t i = 0; i < slots.size() && !found; ++i)
        if (EqualsNoCase(slots[i].label, want)) { selected = static_cast<int>(i); found = true; }
    if (!found)
        UE_LOGI("master_slots: net.master='%s' is not in the list -- showing %s", want.c_str(),
                slots[0].label.c_str());
    // Labels only for the official masters; an address the player configured is theirs to see.
    std::string names;
    for (const Slot& s : slots) {
        if (!names.empty()) names += ", ";
        names += s.label;
        if (!IsOfficial(s.url)) names += " (" + s.url + ")";
    }
    UE_LOGI("master_slots: %zu master(s): %s -- showing %s", slots.size(), names.c_str(),
            slots[static_cast<size_t>(selected)].label.c_str());
    std::lock_guard<std::mutex> lk(g_mu);
    g_state.slots = std::move(slots);
    g_state.selected = selected;
}

void EnsureInit() { std::call_once(g_once, InitOnce); }

}  // namespace

std::vector<Slot> Parse(const std::string& text, std::vector<std::string>* rejected) {
    std::vector<Slot> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t comma = text.find(',', pos);
        if (comma == std::string::npos) comma = text.size();
        const std::string entry = Trim(text.substr(pos, comma - pos));
        pos = comma + 1;
        if (entry.empty()) continue;
        Slot s;
        const size_t eq = entry.find('=');
        if (eq == std::string::npos) {
            s.url = entry;
            s.label = HostOf(entry).substr(0, kMaxLabelChars);
        } else {
            s.label = Trim(entry.substr(0, eq));
            s.url = Trim(entry.substr(eq + 1));
        }
        std::string why = LabelProblem(s.label);
        if (why.empty()) why = AddressProblem(s.url);
        for (size_t i = 0; why.empty() && i < out.size(); ++i)
            if (EqualsNoCase(out[i].label, s.label)) why = "a label already in the list";
        if (why.empty() && out.size() >= kMaxSlots)
            why = "more than " + std::to_string(kMaxSlots) + " masters";
        if (!why.empty()) {
            if (rejected) rejected->push_back("'" + entry + "': " + why);
            continue;
        }
        out.push_back(std::move(s));
    }
    return out;
}

void Init() { EnsureInit(); }

std::vector<Slot> List() {
    EnsureInit();
    std::lock_guard<std::mutex> lk(g_mu);
    return g_state.slots;
}

int SelectedIndex() {
    EnsureInit();
    std::lock_guard<std::mutex> lk(g_mu);
    return g_state.selected;
}

Slot Selected() {
    EnsureInit();
    std::lock_guard<std::mutex> lk(g_mu);
    return g_state.slots[static_cast<size_t>(g_state.selected)];
}

bool Select(int index) {
    EnsureInit();
    std::string label;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (index < 0 || index >= static_cast<int>(g_state.slots.size())) return false;
        if (index == g_state.selected) return true;   // the list already shown: nothing to write
        g_state.selected = index;
        label = g_state.slots[static_cast<size_t>(index)].label;
    }
    // Outside the lock: the ini write is file I/O under the config layer's own mutex.
    if (::coop::config::WriteIniValue(::coop::config_registry::rows::net_master,
                                       label.c_str()))
        UE_LOGI("master_slots: showing %s (remembered in net.master)", label.c_str());
    else
        UE_LOGW("master_slots: showing %s, but multivoid.ini could not remember it -- the next "
                "launch opens on the list it had", label.c_str());
    return true;
}

std::string DisplayName(const std::string& endpoint) {
    EnsureInit();
    const std::string host = HostOf(endpoint);
    std::lock_guard<std::mutex> lk(g_mu);
    for (const Slot& s : g_state.slots)
        if (EqualsNoCase(endpoint, s.url) || (!host.empty() && EqualsNoCase(host, HostOf(s.url))))
            return s.label;
    return endpoint;
}

std::string DefaultSignalingUrl() {
    return HostOf(Selected().url) + ":" + std::to_string(kSignalingPort);
}

}  // namespace coop::net::master_slots
