// coop/bug_report/report_bundle.cpp -- the request, the status and the worker; see
// coop/bug_report/report_bundle.h. The files and the chunked reader are report_files.cpp.
//
// Thread map: Request, GetStatus and ListEntries run on any thread; the capture closure on the
// game thread (the role, slot, player count, game mode and ids live there); the worker on its own
// detached thread, touching no UObject. A Building status is the report in flight; the worker's
// final Publish (Done or Failed) is its release.

#include "coop/bug_report/report_bundle.h"

#include "l10n/mark.h"
#include "report_stream.h"

#include "coop/atomic_file/atomic_file.h"
#include "coop/build_trust/build_trust.h"
#include "coop/config/config_report.h"
#include "coop/net/peer_identity.h"
#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/session/shutdown.h"
#include "coop/version.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"
#include "ue_wrap/world/game_rules.h"

#include "miniz.h"
#include <nlohmann/json.hpp>

#include <windows.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>

namespace coop::bug_report {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

std::mutex g_statusMu;
Status g_status;

void Publish(Status s) {
    std::lock_guard<std::mutex> lk(g_statusMu);
    g_status = std::move(s);
}

// The check and the claim are one step: a Building status is the report in flight, and the final
// Publish (Done or Failed) is its release, so a Request that sees a final status is never refused.
bool ClaimBuilding() {
    std::lock_guard<std::mutex> lk(g_statusMu);
    if (g_status.phase == Phase::Building) return false;
    g_status = Status{};
    g_status.phase = Phase::Building;
    return true;
}

// The Failed status a player sees: the sentence only. `cause` is the internal reason, written
// to the log, which the report then carries (the sentence alone cannot tell seven causes apart).
Status Fail(const char* sentence, const std::string& cause) {
    UE_LOGW("bug_report: the report failed (%s): %s", sentence, cause.c_str());
    Status s;
    s.phase = Phase::Failed;
    s.error = sentence;
    return s;
}

std::string ErrnoText(int err) { return "errno " + std::to_string(err); }

// What the game thread knows, read before the worker starts.
struct Capture {
    std::string role = "solo";  // host | client | solo
    int slot = -1;              // -1 = none (solo, or not yet assigned)
    int players = 0;
    std::string gameMode, selfId, selfKey;
    std::string utc, stamp;     // ISO 8601 Z, and yyyymmdd-hhmmss for the zip's name
};

Capture CaptureOnGameThread() {
    Capture c;
    coop::roster::Snapshot snap;
    coop::roster::GetSnapshot(snap);
    if (coop::roster::LocalIsHost()) c.role = "host";
    else if (snap.inSession) c.role = "client";
    if (c.role != "solo") {
        const uint8_t id = coop::players::Registry::Get().LocalPeerId();
        if (id != coop::players::kPeerIdUnknown) c.slot = id;
    }
    c.players = snap.count;
    ue_wrap::game_rules::Snapshot rules;
    if (ue_wrap::game_rules::ReadLocal(rules)) c.gameMode = rules.gamemodeName;
    c.selfId = coop::net::peer_identity::LocalGuid();
    c.selfKey = coop::net::peer_identity::LocalIdentityString();
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    ::gmtime_s(&tm, &now);
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    c.utc = buf;
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    c.stamp = buf;
    return c;
}

std::wstring Widen(const char* ascii) { return std::wstring(ascii, ascii + std::strlen(ascii)); }

// A FILE closed on every path out, an exception included; Close reports the close's own result
// where the data written matters. Opens as the tree spells _wfopen (_wfopen_s).
class OwnedFile {
public:
    OwnedFile() = default;
    ~OwnedFile() { Close(); }
    OwnedFile(const OwnedFile&) = delete;
    OwnedFile& operator=(const OwnedFile&) = delete;

    bool Open(const std::wstring& path, const wchar_t* mode, int& err) {
        Close();
        err = static_cast<int>(::_wfopen_s(&f_, path.c_str(), mode));
        if (err != 0) f_ = nullptr;
        return f_ != nullptr;
    }
    FILE* get() const { return f_; }
    // True when nothing was open or the close succeeded.
    bool Close() {
        if (!f_) return true;
        const bool ok = std::fclose(f_) == 0;
        f_ = nullptr;
        return ok;
    }

private:
    FILE* f_ = nullptr;
};

// Removes what a run leaves in the work folder, and the half-written zip unless it was renamed.
// Declared before anything that holds a file open, so it runs after those close.
struct Cleanup {
    std::wstring tmpDir, part;
    ~Cleanup() {
        std::error_code ec;
        fs::remove(fs::path(part), ec);
        for (fs::directory_iterator it(fs::path(tmpDir), ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code rm;
            fs::remove(it->path(), rm);
        }
    }
};

// A crashed run's leftovers: every file in the work folder and every half-written zip.
void RemoveLeftovers(const fs::path& reports, const fs::path& tmpDir) {
    std::error_code ec;
    for (fs::directory_iterator it(tmpDir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code rm;
        fs::remove(it->path(), rm);
    }
    atomic_file::RemoveLeftovers(reports, L"report-*.zip");
}

// The zip being written, on a FILE the writer owns; closed on every path out. A failing call
// leaves its internal cause in Cause(), for the log.
class Zip {
public:
    Zip() { mz_zip_zero_struct(&zip_); }
    ~Zip() { Close(); }
    Zip(const Zip&) = delete;
    Zip& operator=(const Zip&) = delete;

    bool Open(const std::wstring& part) {
        int err = 0;
        if (!file_.Open(part, L"wb", err)) {
            cause_ = "open the temporary zip: " + ErrnoText(err);
            return false;
        }
        if (!mz_zip_writer_init_cfile(&zip_, file_.get(), 0)) {
            Remember("mz_zip_writer_init_cfile");
            Close();
            return false;
        }
        open_ = true;
        return true;
    }
    bool AddFile(const char* name, FILE* src, uint64_t size) {
        if (mz_zip_writer_add_cfile(&zip_, name, src, size, nullptr, nullptr, 0, MZ_DEFAULT_LEVEL,
                                    nullptr, 0, nullptr, 0) != 0)
            return true;
        Remember("mz_zip_writer_add_cfile");
        return false;
    }
    bool AddMem(const char* name, const std::string& data) {
        if (mz_zip_writer_add_mem(&zip_, name, data.data(), data.size(), MZ_DEFAULT_LEVEL) != 0)
            return true;
        Remember("mz_zip_writer_add_mem");
        return false;
    }
    // The central directory written, the writer ended, the file closed: a close that fails is a
    // failed zip, since the last buffered bytes are written by it.
    bool Finish() {
        bool ok = open_ && mz_zip_writer_finalize_archive(&zip_) != 0;
        if (!ok) Remember("mz_zip_writer_finalize_archive");
        const bool closed = Close();
        if (ok && !closed) {
            cause_ = "fclose of the temporary zip failed: " + ErrnoText(errno);
            ok = false;
        }
        return ok;
    }
    const std::string& Cause() const { return cause_; }

private:
    bool Close() {
        if (open_) {
            mz_zip_writer_end(&zip_);
            open_ = false;
        }
        return file_.Close();
    }
    void Remember(const char* call) {
        cause_ = std::string(call) + ": " + mz_zip_get_error_string(mz_zip_get_last_error(&zip_));
    }
    mz_zip_archive zip_;
    OwnedFile file_;
    bool open_ = false;
    std::string cause_;
};

enum class AddResult { Ok, ReadFailed, WriteFailed };
using Feed = std::function<bool(const detail::LineFn&)>;

// Removes a path when it goes out of scope.
struct RemoveOnExit {
    const std::wstring& path;
    ~RemoveOnExit() {
        std::error_code ec;
        fs::remove(fs::path(path), ec);
    }
};

// One entry: the lines `feed` produces go through the Redactor into a temporary file, which
// miniz deflates into the zip in its own buffered loop; the temporary file is deleted. A failure
// leaves its internal cause in `cause`.
AddResult AddRedacted(Zip& zip, Redactor& redactor, const std::wstring& tmpDir, const char* name,
                      const Feed& feed, std::string& cause) {
    const std::wstring tmp = tmpDir + L"\\" + Widen(name);
    RemoveOnExit removeTmp{tmp};  // declared first: it runs after the handles below close
    OwnedFile out;
    int err = 0;
    if (!out.Open(tmp, L"wb", err)) {
        cause = std::string("open the temporary file of ") + name + ": " + ErrnoText(err);
        return AddResult::WriteFailed;
    }
    bool writeOk = true;
    int writeErr = 0;
    uint64_t written = 0;
    const bool read = feed([&](std::string_view line) {
        if (!writeOk) return;
        const std::string redacted = redactor.Apply(line);
        if (!redacted.empty() && std::fwrite(redacted.data(), 1, redacted.size(), out.get()) != redacted.size())
            writeOk = false;
        else if (std::fputc('\n', out.get()) == EOF)
            writeOk = false;
        else
            written += redacted.size() + 1;
        if (!writeOk) writeErr = errno;
    });
    if (!read) {
        cause = std::string("reading ") + name + " failed";
        return AddResult::ReadFailed;
    }
    const bool closed = out.Close();
    if (!writeOk || !closed) {
        cause = std::string("writing the temporary file of ") + name + " failed: " +
                ErrnoText(writeOk ? errno : writeErr);
        return AddResult::WriteFailed;
    }
    OwnedFile in;
    if (!in.Open(tmp, L"rb", err)) {
        cause = std::string("reopen the temporary file of ") + name + ": " + ErrnoText(err);
        return AddResult::WriteFailed;
    }
    if (!zip.AddFile(name, in.get(), written)) {
        cause = std::string("adding ") + name + " to the zip: " + zip.Cause();
        return AddResult::WriteFailed;
    }
    return AddResult::Ok;
}

// A text's lines, each without its '\n'; a last line with no '\n' counts.
bool FeedText(const std::string& text, const detail::LineFn& fn) {
    std::string_view v(text);
    while (!v.empty()) {
        const size_t nl = v.find('\n');
        if (nl == std::string_view::npos) {
            fn(v);
            break;
        }
        fn(v.substr(0, nl));
        v.remove_prefix(nl + 1);
    }
    return true;
}

detail::ReadPlan PlanFor(const Entry& e) {
    detail::ReadPlan plan;
    plan.tailOnly = e.tailOnly;
    plan.completeLinesOnly = std::strcmp(e.name, "multivoid.log") == 0;  // the log still being written
    return plan;
}

// Moves the finished temporary zip to report-<stamp>.zip; a name already taken (two saves in one
// UTC second) gets -2, -3 ... -99 and an existing report is never overwritten. Returns the final
// path, or empty with the Win32 error in `err`.
std::wstring MoveToFreeName(const std::wstring& part, const std::wstring& base, DWORD& err) {
    constexpr int kLastSuffix = 99;
    const std::wstring stem = base.substr(0, base.size() - 4);  // base ends in ".zip"
    for (int n = 1; n <= kLastSuffix; ++n) {
        const std::wstring target = n == 1 ? base : stem + L"-" + std::to_wstring(n) + L".zip";
        const atomic_file::Result r = atomic_file::Commit(part, target, atomic_file::Mode::CreateOnly);
        if (r.ok()) return target;
        err = r.error;
        if (!atomic_file::TargetExisted(r)) return {};
    }
    return {};
}

Status Build(const Form& form, const Capture& cap) {
    // The status sentences are marked for translation and drawn translated by the pane (l10n::T on
    // Status::error); the log keeps them English.
    constexpr const char* kWriteFailed = L10N_MARK("Could not write the report file.");
    constexpr const char* kReadLog = L10N_MARK("Could not read the log.");
    constexpr const char* kClosing = L10N_MARK("The game is closing.");
    const std::wstring exeDir = ue_wrap::paths::ExeDir();
    if (exeDir.empty()) return Fail(kWriteFailed, "the game folder's path is empty");
    const std::wstring reports = exeDir + L"\\multivoid_reports";
    const std::wstring tmpDir = reports + L"\\.tmp";
    const std::wstring zipPath = reports + L"\\report-" + Widen(cap.stamp.c_str()) + L".zip";
    Cleanup cleanup{tmpDir, atomic_file::TempPathFor(zipPath).wstring()};

    std::error_code ec;
    fs::create_directories(fs::path(tmpDir), ec);
    if (ec) return Fail(kWriteFailed, "create_directories of the work folder: " + ec.message());
    RemoveLeftovers(fs::path(reports), fs::path(tmpDir));

    RedactContext ctx = ReadThisMachine(cap.selfId, cap.selfKey);
    std::string iniText;
    const coop::config::IniReport iniRead = coop::config::IniTextForReport(iniText);
    const bool iniOk = iniRead == coop::config::IniReport::Ok;
    std::vector<Entry> entries = ListEntries();
    if (!entries[0].leftOut.empty())
        return Fail(L10N_MARK("Could not find the log."), "multivoid.log is " + entries[0].leftOut);
    Redactor redactor(std::move(ctx));
    const std::string reportText = ReportText(form);

    // Pass 1: every player id of every entry, and the bytes of each that pass 2 reads.
    FeedText(reportText, [&](std::string_view line) { redactor.Learn(line); });
    std::vector<detail::Span> spans(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].leftOut.empty()) continue;
        if (coop::shutdown::IsShuttingDown())
            return Fail(kClosing, std::string("shutting down before pass 1 of ") + entries[i].name);
        if (!detail::StreamLines(entries[i].path, PlanFor(entries[i]), false, spans[i],
                                 [&](std::string_view line) { redactor.Learn(line); })) {
            if (i == 0) return Fail(kReadLog, "pass 1 could not read multivoid.log");
            entries[i].leftOut = "could not be read";
        }
    }
    if (iniOk) FeedText(iniText, [&](std::string_view line) { redactor.Learn(line); });

    // Pass 2, in zip order.
    Zip zip;
    if (!zip.Open(cleanup.part)) return Fail(kWriteFailed, zip.Cause());
    std::string cause;  // the internal reason of the last addText / addEntry that failed
    const auto addText = [&](const char* name, const std::string& text) {
        return AddRedacted(zip, redactor, tmpDir, name,
                           [&](const detail::LineFn& fn) { return FeedText(text, fn); }, cause);
    };
    const auto addEntry = [&](size_t i) -> const char* {
        Entry& e = entries[i];
        if (!e.leftOut.empty()) return nullptr;
        if (coop::shutdown::IsShuttingDown()) {
            cause = std::string("shutting down before pass 2 of ") + e.name;
            return kClosing;
        }
        const detail::ReadPlan plan = PlanFor(e);
        const AddResult r = AddRedacted(zip, redactor, tmpDir, e.name, [&](const detail::LineFn& fn) {
            return detail::StreamLines(e.path, plan, true, spans[i], fn);
        }, cause);
        if (r == AddResult::WriteFailed) return kWriteFailed;
        if (r == AddResult::ReadFailed) {
            if (i == 0) return kReadLog;
            e.leftOut = "could not be read";
        }
        return nullptr;
    };

    if (addText(kMadeEntries[0], reportText) != AddResult::Ok) return Fail(kWriteFailed, cause);
    for (const size_t i : {size_t(0), size_t(1), size_t(2)})
        if (const char* why = addEntry(i)) return Fail(why, cause);
    if (iniOk) {
        if (coop::shutdown::IsShuttingDown())
            return Fail(kClosing, "shutting down before pass 2 of multivoid.ini");
        if (addText(kMadeEntries[1], iniText) != AddResult::Ok) return Fail(kWriteFailed, cause);
    }
    if (const char* why = addEntry(3)) return Fail(why, cause);

    Json meta;
    meta["format"] = 1;
    meta["game_target"] = coop::version::kGameTarget;
    meta["build"] = coop::net::kProtocolVersion;
    meta["build_official"] = coop::build_trust::SelfIsOfficial();
    meta["build_sha"] = coop::build_trust::SelfShaHex();
    meta["role"] = cap.role;
    meta["slot"] = cap.slot < 0 ? Json(nullptr) : Json(cap.slot);
    meta["players"] = cap.players;
    meta["game_mode"] = cap.gameMode;
    meta["utc"] = cap.utc;
    meta["reporter"] = "player#self";
    const RedactCounts& counts = redactor.Counts();
    meta["redactions"] = {{"profile", counts.profile}, {"players", counts.players},
                          {"keys", counts.keys}, {"addresses", counts.addresses}};
    Json leftOut = Json::object();
    for (const Entry& e : entries)
        if (!e.leftOut.empty()) leftOut[e.name] = e.leftOut;
    if (iniRead == coop::config::IniReport::NotPresent) leftOut[kMadeEntries[1]] = "not present";
    else if (!iniOk) leftOut[kMadeEntries[1]] = "could not be read";
    meta["left_out"] = leftOut;
    if (!zip.AddMem(kMadeEntries[2], meta.dump(2, ' ', false, Json::error_handler_t::replace)))
        return Fail(kWriteFailed, "adding meta.json to the zip: " + zip.Cause());
    if (!zip.Finish()) return Fail(kWriteFailed, "finishing the zip: " + zip.Cause());
    DWORD moveErr = 0;
    const std::wstring finalPath = MoveToFreeName(cleanup.part, zipPath, moveErr);
    if (finalPath.empty())
        return Fail(kWriteFailed, "the move of the temporary zip: error " + std::to_string(moveErr));

    Status done;
    done.phase = Phase::Done;
    done.zipPath = finalPath;
    done.zipBytes = fs::file_size(fs::path(finalPath), ec);
    if (ec) done.zipBytes = 0;
    done.counts = counts;
    return done;
}

void FailToStart(const std::string& cause) {
    Publish(Fail(L10N_MARK("Could not start the report."), cause));
}

// The worker thread's whole body. An exception escaping a detached thread is std::terminate, so
// nothing leaves it, as in session_manager's workers.
void WorkerMain(const Form& form, const Capture& cap) {
    // A report reads two logs twice and deflates them while the game runs: the game's own threads
    // come first.
    if (!::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL))
        UE_LOGW("bug_report: the worker keeps normal priority (SetThreadPriority error %lu)",
                ::GetLastError());
    try {
        Status result;
        try {
            result = Build(form, cap);
        } catch (const std::exception& e) {
            result = Fail(L10N_MARK("The report could not be made."), std::string("exception: ") + e.what());
        } catch (...) {
            result = Fail(L10N_MARK("The report could not be made."), "a non-standard exception");
        }
        Publish(std::move(result));
    } catch (...) {
        // Publish itself failed: all that can be done is to say so; the status stays Building.
        UE_LOGW("bug_report: the final status could not be published; the report stays Building");
    }
}

}  // namespace

bool Request(Form form) {
    if (ValidateForm(form) != nullptr) return false;
    try {
        if (!ClaimBuilding()) return false;
        ue_wrap::game_thread::Post([form = std::move(form)]() mutable {
            try {
                Capture cap = CaptureOnGameThread();
                ue_wrap::log::Flush();
                std::thread([form = std::move(form), cap = std::move(cap)] { WorkerMain(form, cap); })
                    .detach();
            } catch (const std::exception& e) {
                FailToStart(std::string("the capture or the worker's start threw: ") + e.what());
            } catch (...) {
                FailToStart("the capture or the worker's start threw a non-standard exception");
            }
        });
    } catch (const std::exception& e) {
        FailToStart(std::string("posting the request threw: ") + e.what());
    } catch (...) {
        FailToStart("posting the request threw a non-standard exception");
    }
    return true;
}

Status GetStatus() {
    std::lock_guard<std::mutex> lk(g_statusMu);
    return g_status;
}

}  // namespace coop::bug_report
