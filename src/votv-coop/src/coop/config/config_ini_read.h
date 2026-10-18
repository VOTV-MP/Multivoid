// coop/config/config_ini_read.h -- the multivoid.ini file reader: the open, the one unbounded line
// primitive with its tri-state verdict, and the readers over them (a key by path, a key from the
// live ini, the live ini's lines, the layers below the session).
//
// TU-private, like config_internal.h: product code uses the public coop/config/config.h API.
// The layering that picks which layer answers stays in config.cpp; the lexer pieces (the key
// split, the value cooker) are config_internal.h's.

#pragma once

#include "config_internal.h"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <string>

namespace coop::config::internal {

// The default a read of the ini is given when a line's absence must be told from any real value.
inline const char* const kIniAbsent = "\x01<absent>";

// The one ini line primitive. Three fixed line buffers once served one file format, and a line
// longer than its consumer's buffer split, its tail parsing as a phantom key; one unbounded reader
// delivers whole lines to every consumer. The scan verdict is a tri-state: Ok is a clean end of
// stream (a caller's absent verdict is authoritative), Absent is ENOENT at open, Unreadable is any
// other open failure, a mid-stream read error, or bytes that are not text. A stream error once
// read as a clean end, absent for every key past it, and the writer rebuilt the file from the
// truncated prefix. The enum lives in config_internal.h for the write TU.

// The line source seam (+1 a line, 0 a clean end, -1 a stream error, -2 a byte no text holds):
// production reads a file, and the config selftest injects a failing source to prove the error
// branch yields Unreadable, never Absent.
struct LineSource {
    int (*next)(void* ctx, std::string& out);
    void* ctx;
};

// The file behind a scan, read as BYTES. Text mode failed quietly twice: a Ctrl+Z byte ended the
// stream, and fgets cannot report a zero byte, so a file saved as UTF-16 read as lines holding no
// key -- every key absent, a fail-closed row given the default its refusal exists to withhold
// (net.ice fell to `all`), and the skin mint rebuilding the file from those lines. Text mode's one
// translation stays, a CRLF ending delivered as LF, so a rebuild writes the bytes it wrote before.
struct FileBytes {
    FILE* f;
    size_t pos = 0;
    size_t len = 0;
    char buf[4096];
};

// The production line source over a FileBytes (config_ini_read.cpp).
int FileLineSourceNext(void* ctx, std::string& out);

template <typename Fn>
IniScan ScanLineSource(LineSource src, Fn&& cb, IniFault* faultOut = nullptr) {
    if (faultOut) *faultOut = IniFault::None;
    std::string line;
    bool first = true;
    for (;;) {
        const int r = src.next(src.ctx, line);
        if (r == 0) return IniScan::Ok;
        if (r < 0) {
            if (faultOut) *faultOut = r == -2 ? IniFault::NotText : IniFault::ReadFailed;
            return IniScan::Unreadable;
        }
        if (first) {
            // An editor's UTF-8 byte-order mark belongs to the file, not to its first key, which
            // it would otherwise turn into an unknown one. A UTF-16 mark says the file is not UTF-8
            // at all, even where no zero byte comes before the first newline.
            if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) {
                line.erase(0, 3);
            } else if (line.compare(0, 2, "\xFF\xFE") == 0 || line.compare(0, 2, "\xFE\xFF") == 0) {
                if (faultOut) *faultOut = IniFault::NotText;
                return IniScan::Unreadable;
            }
            first = false;
        }
        cb(line);
    }
}

template <typename Fn>
IniScan ScanIniFileAt(const std::wstring& path, Fn&& cb, IniFault* faultOut = nullptr) {
    if (faultOut) *faultOut = IniFault::None;
    // No path is no file: the mod has no data folder (the boot warning said so), and the settings
    // come from the environment twins and the row defaults. Absent by rule, never an errno's guess:
    // Unreadable refuses sessions.
    if (path.empty()) return IniScan::Absent;
    FILE* f = nullptr;
    const errno_t rc = _wfopen_s(&f, path.c_str(), L"rb");
    if (rc != 0 || !f) {
        if (rc == ENOENT) return IniScan::Absent;
        if (faultOut) *faultOut = IniFault::ReadFailed;
        return IniScan::Unreadable;
    }
    FileBytes bytes;  // the buffer stays unset: only what fread returns is read
    bytes.f = f;
    const IniScan st = ScanLineSource(LineSource{&FileLineSourceNext, &bytes}, cb, faultOut);
    std::fclose(f);
    return st;
}

// The reader core by path, unlocked (the public wrappers hold the lock; the selftest feeds corpus
// files). On Absent or Unreadable the caller still gets `def`, and the verdict reaches the
// callers that discriminate (the seeder, the mint gate, the sweep) through scanOut.
std::string ReadIniValueAt(const std::wstring& path, const char* key, const char* def,
                           IniScan* scanOut, IniFault* faultOut);

// A key from the live ini, under the ini lock, recording a failed scan's fault.
std::string ReadIniValue(const char* key, const char* def, IniScan* scanOut, IniFault* faultOut);

// A key from the live ini with its scan verdict, under the lock.
std::string ReadLiveIniWithScan(const char* key, IniScan& st, IniFault& fault);

}  // namespace coop::config::internal
