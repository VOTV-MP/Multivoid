// coop/thanks/thanks_list.cpp -- see coop/thanks/thanks_list.h.

#include "coop/thanks/thanks_list.h"

#include "coop/net/lobby_client.h"
#include "coop/net/master_slots.h"  // the chosen master, the one the browser talks to
#include "coop/session/shutdown.h"
#include "coop/text/repertoire.h"
#include "coop/text/utf8_codec.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"

#include "../../../resources/thanks_resource_ids.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

namespace coop::thanks_list {

namespace {

// Bounds on one copy of the file. The game's own list is about 600 names in 10 KB; these leave
// room to grow and keep a hostile master from handing the menu a wall of text.
constexpr size_t kMaxFileBytes  = 64 * 1024;
constexpr size_t kMaxSections   = 8;
constexpr size_t kMaxNames      = 2000;  // across all sections
constexpr size_t kMaxNameBytes  = 64;
constexpr size_t kMaxTitleBytes = 48;
constexpr size_t kMaxRevisionDigits = 9;  // fits an int with room; a longer one refuses the copy

const wchar_t* kCacheFileName = L"multivoid_thanks.txt";
// The cache's first line names the master it came from. It is a comment to the list's parser.
constexpr const char* kCacheMasterTag = ";master ";
// How much room that line may take. The reader sizes its buffer as the list's cap plus this,
// so a configured master address longer than the slack would write a file the next boot
// refuses to read, and the cache would silently never load; a longer address goes uncached.
constexpr size_t kCacheTagSlack = 512;
constexpr uint64_t kFetchFloorMs = 8000;  // the same floor the version check keeps

std::mutex g_mu;
List g_embedded;             // the build's own copy; what is shown when no master copy beats it
List g_shown;                // the winner
std::string g_masterRaw;     // what a master last said, as cached; empty for nothing
std::string g_masterRawFrom; // the master that said it: only its own no-list retires it
std::atomic<uint64_t> g_generation{0};
std::atomic<bool> g_fetchInFlight{false};
std::atomic<uint64_t> g_fetchStartMs{0};

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// One displayable string, or empty. Refused whole when it is not well-formed UTF-8 (a repair
// would show a name nobody has); then the nickname lane's denylist, in codepoints: controls,
// the line and paragraph separators, and every Default_Ignorable codepoint, which is where the
// bidi overrides and the zero-width characters live. A name can then neither reorder the text
// around it nor break into a second line the roll did not count.
std::string Clean(const std::string& raw, size_t maxBytes) {
    std::wstring wide;
    if (!coop::text::FromUtf8Strict(raw.data(), raw.size(), &wide)) return {};
    std::wstring kept;
    kept.reserve(wide.size());
    for (size_t i = 0; i < wide.size();) {
        uint32_t c = 0;
        const size_t units = coop::text::DecodeCodepoint(wide, i, &c);
        const wchar_t* at = wide.data() + i;
        i += units;
        if (c < 0x20 || (c >= 0x7F && c <= 0x9F)) continue;   // C0, DEL, C1
        if (c >= 0xD800 && c <= 0xDFFF) continue;             // an unpaired surrogate
        if (c == 0x2028 || c == 0x2029) continue;             // line and paragraph separators
        if (coop::text::IsDefaultIgnorable(c)) continue;
        if (kept.empty() && coop::text::IsCombiningMark(c)) continue;  // nothing to combine with
        kept.append(at, units);
    }
    return coop::text::CapUtf8Bytes(Trim(coop::text::ToUtf8(kept)), maxBytes);
}

bool ParseHexColour(const std::string& tok, uint32_t& out) {
    if (tok.size() != 7 || tok[0] != '#') return false;
    uint32_t v = 0;
    for (size_t i = 1; i < 7; ++i) {
        const char c = tok[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    out = v;
    return true;
}

// Digits only, and few enough that the value cannot overflow. False refuses the whole copy: the
// revision is what orders two copies, so a copy with an unreadable one has no place in the order.
bool ParseRevision(const std::string& val, int& out) {
    if (val.empty() || val.size() > kMaxRevisionDigits) return false;
    int v = 0;
    for (const char c : val) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

// `[Title] #RRGGBB apart`, both optional. Anything else after the bracket makes the line a name,
// so a player whose name opens with a bracketed clan tag is not read as a section.
bool ParseSectionHeader(const std::string& line, Section& out) {
    if (line.empty() || line[0] != '[') return false;
    const size_t close = line.find(']');
    if (close == std::string::npos) return false;
    Section s;
    s.title = Clean(line.substr(1, close - 1), kMaxTitleBytes);
    if (s.title.empty()) return false;
    size_t i = close + 1;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
        if (j == i) break;
        const std::string tok = line.substr(i, j - i);
        if (tok == "apart") s.apart = true;
        else if (!ParseHexColour(tok, s.rgb)) return false;
        i = j;
    }
    out = std::move(s);
    return true;
}

size_t NameCount(const List& l) {
    size_t n = 0;
    for (const Section& s : l.sections) n += s.names.size();
    return n;
}

std::wstring CachePath() {
    const std::wstring dir = ue_wrap::paths::ExeDir();
    return dir.empty() ? std::wstring() : dir + L"\\" + kCacheFileName;
}

bool ReadFileBytes(const std::wstring& path, std::string& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    std::string buf(kMaxFileBytes + kCacheTagSlack + 1, '\0');  // the list's cap plus the master line
    const size_t n = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (n == 0 || n >= buf.size()) return false;
    buf.resize(n);
    out = std::move(buf);
    return true;
}

// Written beside itself and moved into place, so a crash mid-write leaves the old cache whole.
// The staging name carries the process id: two copies of the game run from one folder would
// otherwise write into each other's staging file and publish the mixture.
bool WriteFileBytes(const std::wstring& path, const std::string& bytes) {
    const std::wstring tmp = path + L".tmp" + std::to_wstring(::GetCurrentProcessId());
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return false;
    const size_t n = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (n != bytes.size() || !::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ::DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool EmbeddedBytes(std::string& out) {
    HMODULE self = nullptr;
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&EmbeddedBytes), &self);
    if (!self) return false;
    HRSRC res = ::FindResourceW(self, MAKEINTRESOURCEW(IDR_THANKS_LIST),
                                reinterpret_cast<LPCWSTR>(RT_RCDATA));
    if (!res) return false;
    HGLOBAL glob = ::LoadResource(self, res);
    const DWORD size = ::SizeofResource(self, res);
    const void* p = glob ? ::LockResource(glob) : nullptr;
    if (!p || size == 0) return false;
    out.assign(static_cast<const char*>(p), size);
    return true;
}

// Decide what is shown from the build's copy and what the master last said, and publish it. A
// master's copy wins a tie, so an edit published without a new release still takes; an older one
// loses, so a master left holding an old file cannot take names away from a newer build.
void Settle(const char* why) {
    List master;
    std::lock_guard<std::mutex> lk(g_mu);
    const bool haveMaster =
        !g_masterRaw.empty() && Parse(g_masterRaw.data(), g_masterRaw.size(), master);
    const bool masterWins = haveMaster && master.revision >= g_embedded.revision;
    g_shown = masterWins ? std::move(master) : g_embedded;
    g_generation.fetch_add(1, std::memory_order_release);
    UE_LOGI("thanks_list: %s -- showing revision %d from the %s copy, %zu sections, %zu names", why,
            g_shown.revision, masterWins ? "master's" : "embedded", g_shown.sections.size(),
            NameCount(g_shown));
}

// The cache is a memo of what ONE master last said: the master's address on the first line, then
// the text as served. A copy another master left is not this master's word and is not read.
bool ReadCacheFor(const std::string& masterUrl, std::string& outRaw) {
    const std::wstring path = CachePath();
    std::string bytes;
    if (path.empty() || !ReadFileBytes(path, bytes)) return false;
    const std::string want = std::string(kCacheMasterTag) + masterUrl + "\n";
    if (bytes.compare(0, want.size(), want) != 0) return false;
    outRaw = bytes.substr(want.size());
    return !outRaw.empty();
}

// Deletes the cache only when it is this master's: the file may hold another master's word from
// an earlier run on that master, and that word is not this one's to retire.
void DeleteCacheOf(const std::string& masterUrl) {
    std::string mine;
    if (!ReadCacheFor(masterUrl, mine)) return;
    const std::wstring path = CachePath();
    if (!path.empty()) ::DeleteFileW(path.c_str());
}

void WriteCacheFor(const std::string& masterUrl, const std::string& raw) {
    const std::wstring path = CachePath();
    const std::string tag = std::string(kCacheMasterTag) + masterUrl + "\n";
    // A tag past the reader's slack writes a file that never loads again, which reads as "the
    // cache keeps vanishing"; refusing to write it says so once instead.
    if (tag.size() >= kCacheTagSlack) {
        UE_LOGW("thanks_list: the master address is too long to tag a cache with (%zu bytes); the "
                "downloaded list lasts this run only", tag.size());
        return;
    }
    if (path.empty() || !WriteFileBytes(path, tag + raw))
        UE_LOGW("thanks_list: the downloaded list could not be cached; it lasts this run only");
}

// THE RULE THIS LANE EXISTS FOR: only the master SAYING it has no list may retire the copy a
// client cached from it. A text it served that this parser refuses is one bad publish -- a
// mistyped revision, a file grown past the cap -- and a bad publish must never cost every player
// the list they already had. Pure, so the decision can be asserted without a master to ask.
bool RetiresCachedCopy(coop::net::lobby::ThanksFetch got, bool parsed) {
    using coop::net::lobby::ThanksFetch;
    if (got == ThanksFetch::NoList) return true;       // the master has nothing: its last word falls
    if (got == ThanksFetch::Unreachable) return false;  // no word at all: keep what we have
    return false;                                       // a text, parsed or not, never retires a copy
    (void)parsed;
}

// Un-gated, at Init: the interesting cases are a master serving a broken list and a master gone
// quiet, and no drill stages either. A wrong verdict here does not crash -- it silently empties
// every player's menu -- which is exactly the kind of decision that reads as working.
void RunSelftest() {
    using coop::net::lobby::ThanksFetch;
    struct Row { ThanksFetch got; bool parsed; bool retires; const char* what; };
    static const Row rows[] = {
        {ThanksFetch::NoList,      false, true,  "an explicit no-list retires the cached copy"},
        {ThanksFetch::Unreachable, false, false, "silence keeps it"},
        {ThanksFetch::Text,        true,  false, "a good text keeps it (and replaces it)"},
        {ThanksFetch::Text,        false, false, "a text this parser refuses keeps it"},
    };
    int bad = 0;
    for (const Row& r : rows) {
        if (RetiresCachedCopy(r.got, r.parsed) != r.retires) {
            UE_LOGE("thanks_list selftest FAIL: %s", r.what);
            ++bad;
        }
    }
    if (bad == 0)
        UE_LOGI("thanks_list selftest: ALL PASS (%zu rows) -- only an explicit no-list retires a "
                "cached copy", sizeof(rows) / sizeof(rows[0]));
}

void FetchOnce(const std::string& masterUrl) {
    if (coop::shutdown::IsShuttingDown()) return;
    std::string raw;
    const auto got = coop::net::lobby::LobbyClient::FetchThanks(masterUrl, 8000, raw);
    // The fetch blocks for up to eight seconds and this thread is detached, so teardown can begin
    // underneath it. Asked again here, before anything is written: a process that exits between a
    // staging write and its rename leaves a stray file nobody reads, and nothing downstream of a
    // shutdown is worth doing.
    if (coop::shutdown::IsShuttingDown()) return;
    if (got == coop::net::lobby::ThanksFetch::Unreachable) return;  // no word: keep what we have
    // Only the master SAYING it has no list may retire what it said before. A text it served that
    // this parser refuses is not that answer: it is one bad publish -- a mistyped revision, a file
    // grown past the cap -- and treating it as "nothing to show" would delete a good cache over a
    // typo and roll every menu back to the build's copy until the next publish.
    List probe;
    const bool parsed = got == coop::net::lobby::ThanksFetch::Text &&
                        Parse(raw.data(), raw.size(), probe);
    if (!parsed && !RetiresCachedCopy(got, parsed)) {
        UE_LOGW("thanks_list: the master's copy does not parse (%zu bytes) -- keeping what is "
                "cached; only an explicit no-list retires it", raw.size());
        return;
    }
    if (!parsed) raw.clear();  // NoList: this master has nothing, so its last word no longer stands
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (raw.empty()) {
            // Only the master whose word is held may retire it: the player may have chosen another
            // master since, and that one having no list says nothing about what the first served.
            if (g_masterRaw.empty() || g_masterRawFrom != masterUrl) return;
        } else if (raw == g_masterRaw && g_masterRawFrom == masterUrl) {
            return;  // the same word as last time: nothing to write, nothing to rebuild
        }
        changed = raw != g_masterRaw;
        g_masterRaw = raw;
        g_masterRawFrom = raw.empty() ? std::string() : masterUrl;
    }
    if (raw.empty()) {
        DeleteCacheOf(masterUrl);
        Settle("the master serves no list");
        return;
    }
    // The same text from another master is only re-tagged, so the next boot on that master reads
    // it; the list shown did not change, so nothing is rebuilt.
    WriteCacheFor(masterUrl, raw);
    if (changed) Settle("the master's copy changed");
}

}  // namespace

bool Parse(const char* text, size_t size, List& out) {
    if (!text || size == 0 || size > kMaxFileBytes) return false;
    size_t pos = 0;
    if (size >= 3 && static_cast<uint8_t>(text[0]) == 0xEF &&
        static_cast<uint8_t>(text[1]) == 0xBB && static_cast<uint8_t>(text[2]) == 0xBF)
        pos = 3;  // a byte-order mark an editor left
    List l;
    Section cur;
    bool inSection = false;
    size_t names = 0;
    auto closeSection = [&] {
        if (inSection && !cur.names.empty() && l.sections.size() < kMaxSections)
            l.sections.push_back(std::move(cur));
        cur = Section{};
    };
    while (pos < size) {
        size_t eol = pos;
        while (eol < size && text[eol] != '\n' && text[eol] != '\r') ++eol;  // any of LF, CRLF, CR
        const std::string line = Trim(std::string(text + pos, eol - pos));
        pos = eol + 1;
        if (line.empty() || line[0] == ';') continue;
        Section header;
        if (ParseSectionHeader(line, header)) {
            closeSection();
            cur = std::move(header);
            inSection = true;
            continue;
        }
        if (!inSection) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = Trim(line.substr(0, eq));
            const std::string val = Trim(line.substr(eq + 1));
            if (key == "revision" && !ParseRevision(val, l.revision)) return false;
            if (key == "title") l.title = Clean(val, kMaxTitleBytes);
            continue;
        }
        if (names >= kMaxNames) continue;
        std::string name = Clean(line, kMaxNameBytes);
        if (name.empty()) continue;
        cur.names.push_back(std::move(name));
        ++names;
    }
    closeSection();
    if (l.sections.empty()) return false;
    out = std::move(l);
    return true;
}

// A staging file an earlier run left behind: the name carries the writing process's id, so one
// that is not ours is from a run that is over. Nothing ever reads these; they are swept at boot so
// a killed fetch cannot litter the game's folder indefinitely.
void SweepStaleStaging() {
    const std::wstring path = CachePath();
    if (path.empty()) return;
    const std::wstring pattern = path + L".tmp*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    const std::wstring mine = L".tmp" + std::to_wstring(::GetCurrentProcessId());
    const std::wstring dir = path.substr(0, path.find_last_of(L'\\') + 1);
    int swept = 0;
    do {
        const std::wstring name = fd.cFileName;
        if (name.size() >= mine.size() && name.compare(name.size() - mine.size(), mine.size(), mine) == 0)
            continue;  // ours, and a fetch may be writing it right now
        if (::DeleteFileW((dir + name).c_str())) ++swept;
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    if (swept > 0) UE_LOGI("thanks_list: swept %d abandoned staging file(s)", swept);
}

void Init() {
    RunSelftest();
    SweepStaleStaging();
    std::string raw;
    List embedded;
    if (EmbeddedBytes(raw) && Parse(raw.data(), raw.size(), embedded)) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_embedded = std::move(embedded);
    } else {
        UE_LOGE("thanks_list: the embedded list did not load -- the menu shows none until a "
                "download lands");
    }
    const std::string masterUrl = coop::net::master_slots::Selected().url;
    std::string cached;
    if (ReadCacheFor(masterUrl, cached)) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_masterRaw = std::move(cached);
        g_masterRawFrom = masterUrl;
    }
    Settle("boot");
}

void RefreshFromMaster() {
    const uint64_t now = ::GetTickCount64();
    const uint64_t last = g_fetchStartMs.load(std::memory_order_relaxed);
    if (last != 0 && now - last < kFetchFloorMs) return;
    if (g_fetchInFlight.exchange(true)) return;
    g_fetchStartMs.store(now, std::memory_order_relaxed);
    const std::string masterUrl = coop::net::master_slots::Selected().url;
    std::thread([masterUrl] {
        // Everything is caught, the shape of every other master worker: an exception out of a
        // detached thread is a terminate, and one that skipped the line below would leave the
        // latch set and turn every later refresh into a no-op.
        try {
            FetchOnce(masterUrl);
        } catch (...) {
            UE_LOGW("thanks_list: the fetch worker threw; the list stays as it was");
        }
        g_fetchInFlight.store(false, std::memory_order_release);
    }).detach();
}

uint64_t Generation() { return g_generation.load(std::memory_order_acquire); }

uint64_t Copy(List& out) {
    std::lock_guard<std::mutex> lk(g_mu);
    out = g_shown;
    return g_generation.load(std::memory_order_acquire);
}

}  // namespace coop::thanks_list
