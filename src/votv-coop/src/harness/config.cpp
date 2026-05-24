// harness/config.cpp -- env + ini configuration readers.
//
// Extracted from harness/harness.cpp (2026-05-25 modular refactor).

#include "harness/config.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace harness::config {

std::wstring ModuleDir() {
    HMODULE self = nullptr;
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p(path);
    const size_t sep = p.find_last_of(L"\\/");
    return sep == std::wstring::npos ? L"." : p.substr(0, sep);
}

std::string ReadEnv(const char* name) {
    char buf[256] = {};
    const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf) : std::string();
}

std::string ReadScenario() {
    const std::string env = ReadEnv("VOTVCOOP_SCENARIO");
    if (!env.empty()) return env;
    const std::wstring path = ModuleDir() + L"\\scenario.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || !f) return "newgame";
    char line[64] = {};
    char* got = std::fgets(line, sizeof(line), f);
    std::fclose(f);
    if (!got) return "newgame";
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    return s.empty() ? "newgame" : s;
}

std::string ReadIniValue(const char* key, const char* def) {
    const std::wstring path = ModuleDir() + L"\\votv-coop.ini";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || !f) return def;
    const std::string prefix = std::string(key) + "=";
    std::string result = def;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s(line);
        s.erase(std::remove_if(s.begin(), s.end(),
                               [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }),
                s.end());
        if (s.rfind(prefix, 0) == 0) { result = s.substr(prefix.size()); break; }
    }
    std::fclose(f);
    return result;
}

coop::net::Config ReadNetConfig(bool& enabled) {
    coop::net::Config c;
    std::string role = ReadEnv("VOTVCOOP_NET_ROLE");
    if (role.empty()) role = ReadIniValue("net.role", "");
    enabled = (role == "host" || role == "client");
    c.role = (role == "client") ? coop::net::Role::Client : coop::net::Role::Host;

    std::string peer = ReadEnv("VOTVCOOP_NET_PEER");
    c.peerIp = peer.empty() ? ReadIniValue("net.peer", "127.0.0.1") : peer;

    std::string port = ReadEnv("VOTVCOOP_NET_PORT");
    if (port.empty()) port = ReadIniValue("net.port", "");
    if (!port.empty()) c.port = static_cast<uint16_t>(std::strtoul(port.c_str(), nullptr, 10));
    return c;
}

std::wstring ReadNickname() {
    std::string nick = ReadEnv("VOTVCOOP_NET_NICK");
    if (nick.empty()) nick = ReadIniValue("net.nick", "Player");
    return std::wstring(nick.begin(), nick.end());
}

}  // namespace harness::config
