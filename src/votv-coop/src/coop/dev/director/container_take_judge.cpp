// coop/dev/director/container_take_judge.cpp -- the two-peer take race's counts file and verdict (see the header).

#include "container_take_judge.h"

#include <algorithm>
#include <fstream>

#include <windows.h>

namespace coop::director {

uint64_t NowUnixMs() {
    FILETIME ft; ::GetSystemTimeAsFileTime(&ft);
    uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;  // 100ns since 1601
    return (t - 116444736000000000ULL) / 10000ULL;   // -> Unix ms
}

std::string BesideGo(const std::string& goFile, const std::string& leaf) {
    if (goFile.empty() || leaf.empty()) return {};
    const size_t cut = goFile.find_last_of("\\/");
    return (cut == std::string::npos ? std::string() : goFile.substr(0, cut + 1)) + leaf;
}

bool WriteSentinel(const std::string& file, const std::string& text) {
    const std::string tmp = file + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        f.flush();
        if (!f) return false;
    }
    return ::MoveFileExA(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

std::string CountsFile(const std::string& goFile, const std::string& role) {
    return role.empty() ? std::string() : BesideGo(goFile, "multivoid_race_counts_" + role + ".txt");
}
bool WriteCounts(const std::string& file, const PeerCounts& c) {
    const int32_t v[] = {c.contBefore, c.contAfter, c.pockBefore, c.pockAfter, c.act};
    std::string text;
    for (const int32_t n : v) text += std::to_string(n) + " ";
    return !file.empty() && WriteSentinel(file, text);
}
bool WaitForCounts(const std::string& file, uint64_t timeoutMs, PeerCounts& out) {
    const uint64_t start = NowUnixMs();
    while (!file.empty() && NowUnixMs() - start < timeoutMs) {
        std::ifstream f(file, std::ios::binary);
        PeerCounts c;
        if (f && f >> c.contBefore >> c.contAfter >> c.pockBefore >> c.pockAfter >> c.act) { out = c; return true; }
        ::Sleep(200);
    }
    return false;
}

Judged Judge(const PeerCounts& host, const PeerCounts& client) {
    if ((std::min)({host.contBefore, host.contAfter, host.pockBefore, host.pockAfter, client.pockBefore, client.pockAfter}) < 0)
        return {-1, "aborted", "a-count-did-not-read"};
    const int32_t loss = host.contBefore - host.contAfter;
    const int32_t gained = (host.pockAfter - host.pockBefore) + (client.pockAfter - client.pockBefore);
    if (loss < 0 || loss > 1 || host.pockAfter < host.pockBefore || client.pockAfter < client.pockBefore)
        return {-1, "aborted", "a-count-moved-the-wrong-way"};
    const int32_t total = (1 - loss) + gained;
    if (client.act == kActAborted) return {total, "aborted", "the-client-act-did-not-replay"};
    if (client.act == kActNone || client.act == kActPending) return {total, "aborted", "the-client-act-never-ran-or-is-open"};
    if (loss == 0 && gained == 0) return {total, "nothing", ""};
    if (loss == 1 && gained == 0) return {total, "vanished", ""};
    return {total, total == 1 ? "correct" : "dup", ""};
}

}  // namespace coop::director
