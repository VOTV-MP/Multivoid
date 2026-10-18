// coop/config/config_ini_read.cpp -- the multivoid.ini file reader, see config_ini_read.h: the
// line source over the file's bytes, the one ini lock, the readers of a key and of the live ini's
// lines, and the layers below the session.

#include "config_ini_read.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace coop::config {

using IniScan = internal::IniScan;

namespace {

// One lock for every multivoid.ini access: writers come from two threads (the render thread's
// skins and voice panels, the game thread's boot default writes), and an unserialised
// read-modify-write pair let one writer rebuild the file from the other's half-written state.
// Readers take it too, so a read never sees the pre-rename transition.
std::mutex g_iniMutex;

// The fault of the last live-ini read that failed this launch (None while none did); the boot
// sweep reads it for its panel rows. Set for the module-dir ini only, never a selftest corpus path.
std::atomic<IniFault> g_iniFault{IniFault::None};

// A live-ini read's fault, kept for the panel and the census. A clean read leaves the last fault
// standing: they report whether any read this launch ran without the file.
void NoteLiveScan(IniScan st, IniFault fault) {
    if (st == IniScan::Unreadable) g_iniFault.store(fault, std::memory_order_relaxed);
}

}  // namespace

IniFault LastIniFault() { return g_iniFault.load(std::memory_order_relaxed); }

namespace internal {

// One line, unbounded, with its newline when the file has one. A line a read error cut short is
// not delivered: its prefix could read as a different, valid value. A zero byte ends the scan
// rather than being skipped: no UTF-8 text holds one, and reading around it could deliver a value
// the file does not show.
int FileLineSourceNext(void* ctx, std::string& out) {
    FileBytes& b = *static_cast<FileBytes*>(ctx);
    out.clear();
    for (;;) {
        if (b.pos == b.len) {
            b.pos = 0;
            b.len = std::fread(b.buf, 1, sizeof(b.buf), b.f);
            if (b.len == 0) {
                if (std::ferror(b.f)) return -1;
                return out.empty() ? 0 : 1;  // a final line without a trailing newline
            }
        }
        const char* p = b.buf + b.pos;
        const size_t avail = b.len - b.pos;
        const char* nl = static_cast<const char*>(std::memchr(p, '\n', avail));
        const size_t take = nl ? static_cast<size_t>(nl - p) + 1 : avail;
        if (std::memchr(p, '\0', take)) return -2;
        out.append(p, take);
        b.pos += take;
        if (nl) {
            if (out.size() >= 2 && out[out.size() - 2] == '\r') out.erase(out.size() - 2, 1);
            return 1;
        }
    }
}

std::mutex& IniMutex() { return g_iniMutex; }

IniScan ScanIniFile(const std::wstring& path,
                    const std::function<void(const std::string&)>& cb, IniFault* faultOut) {
    return ScanIniFileAt(path, cb, faultOut);
}

// The reader core by path, unlocked (the public wrappers hold the lock; the selftest feeds corpus
// files). On Absent or Unreadable the caller still gets `def`, and the verdict reaches the
// callers that discriminate (the seeder, the mint gate, the sweep) through scanOut.
std::string ReadIniValueAt(const std::wstring& path, const char* key, const char* def,
                           IniScan* scanOut, IniFault* faultOut) {
    std::string result = def;
    bool found = false;
    const IniScan st = ScanIniFileAt(path, [&](const std::string& line) {
        if (found) return;
        std::string k, v;
        if (ParseIniKeyValue(line, k, v) && _stricmp(k.c_str(), key) == 0) {
            result = CookIniValue(v, /*wsPrecededOnly=*/true);
            found = true;
        }
    }, faultOut);
    if (scanOut) *scanOut = st;
    return result;
}

std::string ReadIniValue(const char* key, const char* def, IniScan* scanOut, IniFault* faultOut) {
    std::lock_guard<std::mutex> lk(g_iniMutex);
    IniScan st = IniScan::Ok;
    IniFault fault = IniFault::None;
    std::string v = ReadIniValueAt(LiveIniPath(), key, def, &st, &fault);
    NoteLiveScan(st, fault);
    if (scanOut) *scanOut = st;
    if (faultOut) *faultOut = fault;
    return v;
}

// A key from the live ini with its scan verdict, under the lock.
std::string ReadLiveIniWithScan(const char* key, IniScan& st, IniFault& fault) {
    std::lock_guard<std::mutex> lk(g_iniMutex);
    std::string v = ReadIniValueAt(LiveIniPath(), key, "", &st, &fault);
    NoteLiveScan(st, fault);
    return v;
}

}  // namespace internal

// The file operations the sweep and the owner reformat use.

int ListLiveIniLines(std::vector<std::string>& out, IniFault* faultOut) {
    std::lock_guard<std::mutex> lk(g_iniMutex);
    IniFault fault = IniFault::None;
    const IniScan st = internal::ScanIniFileAt(
        internal::LiveIniPath(), [&](const std::string& line) { out.push_back(line); }, &fault);
    NoteLiveScan(st, fault);
    if (faultOut) *faultOut = fault;
    return static_cast<int>(st);
}

namespace internal {

// The same layers without the session (config_internal.h), over the ini at `iniPath`. The ini is
// read under IniMutex, as ReadIniValue reads it, and the scan's verdict is not recorded: an
// unreadable file answers like an absent line, the default.
void RawBelowSession(const std::wstring& iniPath, const config_registry::Row* row,
                     std::string& raw) {
    if (RuntimeLayerGet(row, raw)) return;
    if (row->envVar) {
        const std::string e = ReadEnv(row->envVar);
        if (!e.empty()) {
            raw = e;
            return;
        }
    }
    std::string v;
    {
        std::lock_guard<std::mutex> lk(g_iniMutex);
        v = ReadIniValueAt(iniPath, row->key, kIniAbsent, nullptr, nullptr);
    }
    raw = (v == kIniAbsent) ? DefaultText(*row) : v;
}

}  // namespace internal

}  // namespace coop::config
