// coop/voice/camcorder_voice.cpp -- see coop/voice/camcorder_voice.h.

#include "coop/voice/camcorder_voice.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "coop/voice/voice_playback.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/process_launch.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace coop::voice::camcorder_voice {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

constexpr uint32_t kTrackRate = 48000;        // the voice mixer's rate; the track is stereo s16
constexpr uint64_t kPendingBoundMs = 120000;  // a stopped recording's track waits this long for its merger
constexpr int      kSilentPeak = 64;          // a track whose peak stays under this holds no voice: nothing to add
constexpr size_t   kMixChunkFrames = 8192;
constexpr uint64_t kWavWaitMs = 5000;        // the longest the merger's launch waits for the engine to finish the wav
constexpr DWORD    kWavPollMs = 50;

uint64_t NowMs() { return ::GetTickCount64(); }

// ---- the camera manager, read by reflection ----

void* Obj(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const char*>(obj) + off) : nullptr;
}

bool Bool(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask)) return false;
    return (static_cast<const uint8_t*>(obj)[off] & mask) != 0;
}

std::wstring Str(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    return off >= 0 ? ue_wrap::field_io::ReadFStringAt(obj, off) : std::wstring();
}

void* Manager() {
    void* const m = Obj(ue_wrap::world_singleton::Gamemode(), L"rcm_manager");
    return m && R::IsLive(m) ? m : nullptr;
}

// ---- the track: the game thread writes it, the merger's launch reads it ----

struct Pending {
    std::wstring rawPath;
    uint64_t frames = 0;
    uint64_t deadlineMs = 0;
    bool voiced = false;
};

std::mutex g_mu;          // guards g_pending (the game thread and the launching thread)
bool g_havePending = false;
Pending g_pending;

bool g_recording = false;   // the manager's bIsRecording, last tick
bool g_paused = false;
bool g_wantsSelf = false;
FILE* g_raw = nullptr;
std::wstring g_rawPath;
uint64_t g_frames = 0;
int g_peak = 0;
uint64_t g_droppedAtStart = 0;
std::vector<int16_t> g_drain;
coop::voice::Playback* g_pb = nullptr;   // the mixer the last tick read; the launch finishes a track through it

void DropPendingLocked() {
    if (g_havePending && !g_pending.rawPath.empty()) ::DeleteFileW(g_pending.rawPath.c_str());
    g_havePending = false;
    g_pending = Pending{};
}

std::wstring RawPathFor(void* manager) {
    std::wstring dir = Str(manager, L"VideoCapDir");
    if (dir.empty()) {
        wchar_t tmp[MAX_PATH] = {};
        ::GetTempPathW(MAX_PATH, tmp);
        dir = tmp;
    }
    if (!dir.empty() && dir.back() != L'/' && dir.back() != L'\\') dir += L'/';
    return dir + L"multivoid_voice.raw";
}

void Begin(void* manager, coop::voice::Playback& pb) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        DropPendingLocked();   // an older recording's track its merger never took
    }
    g_rawPath = RawPathFor(manager);
    g_raw = _wfopen(g_rawPath.c_str(), L"wb");
    g_frames = 0;
    g_peak = 0;
    g_drain.clear();
    g_droppedAtStart = pb.DrainCamera(g_drain);   // whatever an earlier camera left queued
    g_drain.clear();
    UE_LOGI("camcorder_voice: a recording began -- the voices as heard at the camera stream to '%ls' (%hs)",
            g_rawPath.c_str(), g_raw ? "open" : "could NOT be opened; this recording gets no voice");
}

void Append(coop::voice::Playback& pb) {
    g_drain.clear();
    pb.DrainCamera(g_drain);
    if (g_drain.empty()) return;
    for (int16_t s : g_drain) {
        const int a = s < 0 ? -static_cast<int>(s) : s;
        if (a > g_peak) g_peak = a;
    }
    if (g_raw) std::fwrite(g_drain.data(), sizeof(int16_t), g_drain.size(), g_raw);
    g_frames += g_drain.size() / 2;
}

void End(coop::voice::Playback& pb) {
    Append(pb);
    const uint64_t dropped = pb.DrainCamera(g_drain) - g_droppedAtStart;
    g_drain.clear();
    if (g_raw) std::fclose(g_raw);
    const bool ok = g_raw != nullptr;
    g_raw = nullptr;
    const bool voiced = ok && g_peak >= kSilentPeak;
    UE_LOGI("camcorder_voice: the recording ended -- %.2f s of voice track, peak %d, %llu frame(s) dropped%hs",
            static_cast<double>(g_frames) / kTrackRate, g_peak, static_cast<unsigned long long>(dropped),
            voiced ? "; it waits for the merger" : " (no voice: nothing to add)");
    std::lock_guard<std::mutex> lk(g_mu);
    if (!voiced) {
        if (ok) ::DeleteFileW(g_rawPath.c_str());
        return;
    }
    g_havePending = true;
    g_pending.rawPath = g_rawPath;
    g_pending.frames = g_frames;
    g_pending.deadlineMs = NowMs() + kPendingBoundMs;
    g_pending.voiced = true;
}

// ---- the merge, on the merger's launch ----

// The merger's MergeInfo.ini, beside its exe: line 2 the capture's name, line 3 the capture folder; the wav the
// master-mix capture wrote is <folder><name>.wav. Empty when the file does not read.
std::wstring WavFromMergeInfo(const std::wstring& exe) {
    const size_t slash = exe.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    const std::wstring ini = exe.substr(0, slash + 1) + L"MergeInfo.ini";
    FILE* f = _wfopen(ini.c_str(), L"rb");
    if (!f) return {};
    std::string bytes;
    char buf[1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && bytes.size() < 65536) bytes.append(buf, n);
    std::fclose(f);
    std::wstring text;
    if (bytes.size() >= 2 && static_cast<uint8_t>(bytes[0]) == 0xFF && static_cast<uint8_t>(bytes[1]) == 0xFE) {
        text.assign(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / 2);
    } else {
        size_t at = bytes.size() >= 3 && static_cast<uint8_t>(bytes[0]) == 0xEF ? 3 : 0;
        const int len = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data() + at, static_cast<int>(bytes.size() - at), nullptr, 0);
        text.resize(len > 0 ? static_cast<size_t>(len) : 0);
        if (len > 0) ::MultiByteToWideChar(CP_UTF8, 0, bytes.data() + at, static_cast<int>(bytes.size() - at), text.data(), len);
    }
    std::vector<std::wstring> lines;
    std::wstring cur;
    for (wchar_t c : text) {
        if (c == L'\r') continue;
        if (c == L'\n') { lines.push_back(cur); cur.clear(); } else cur += c;
    }
    lines.push_back(cur);
    if (lines.size() < 3 || lines[1].empty() || lines[2].empty()) return {};
    std::wstring dir = lines[2];
    if (dir.back() != L'/' && dir.back() != L'\\') dir += L'/';
    return dir + lines[1] + L".wav";
}

struct WavLayout {
    uint16_t channels = 0;
    uint32_t rate = 0;
    long     dataAt = 0;
    uint32_t dataBytes = 0;
};

// A 16-bit PCM wav's format and data chunk. False for anything else.
bool ReadWavLayout(FILE* f, WavLayout& out) {
    uint8_t riff[12];
    if (std::fread(riff, 1, 12, f) != 12 || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0)
        return false;
    bool haveFmt = false;
    uint16_t bits = 0, format = 0;
    for (;;) {
        uint8_t hdr[8];
        if (std::fread(hdr, 1, 8, f) != 8) return false;
        uint32_t size = 0;
        std::memcpy(&size, hdr + 4, 4);
        if (std::memcmp(hdr, "fmt ", 4) == 0) {
            uint8_t fmt[40] = {};
            const size_t take = size < sizeof(fmt) ? size : sizeof(fmt);
            if (std::fread(fmt, 1, take, f) != take) return false;
            std::memcpy(&format, fmt + 0, 2);
            std::memcpy(&out.channels, fmt + 2, 2);
            std::memcpy(&out.rate, fmt + 4, 4);
            std::memcpy(&bits, fmt + 14, 2);
            if (size > take) std::fseek(f, static_cast<long>(size - take), SEEK_CUR);
            haveFmt = true;
        } else if (std::memcmp(hdr, "data", 4) == 0) {
            out.dataAt = std::ftell(f);
            out.dataBytes = size;
            return haveFmt && (format == 1 || format == 0xFFFE) && bits == 16 && out.channels >= 1 && out.rate > 0;
        } else {
            std::fseek(f, static_cast<long>(size + (size & 1)), SEEK_CUR);
        }
    }
}

// The track added into the wav's first two channels (its front left and right; a mono wav takes the two halves),
// each sample clamped. The track is read at the wav's own rate by the nearest frame. False when a file does not open
// or the wav is not 16-bit PCM.
bool MixInto(const std::wstring& wavPath, const Pending& p, uint64_t* mixedFrames) {
    // The engine writes the wav on its own task, still open when the merger launches: waited for until it opens for
    // writing and its size holds still across two polls, within kWavWaitMs.
    FILE* wav = nullptr;
    uint64_t lastSize = ~0ull;
    const uint64_t until = NowMs() + kWavWaitMs;
    for (;;) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        const uint64_t size = ::GetFileAttributesExW(wavPath.c_str(), GetFileExInfoStandard, &fa)
                                  ? (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow
                                  : ~0ull;
        if (size != ~0ull && size == lastSize && (wav = _wfopen(wavPath.c_str(), L"r+b")) != nullptr) break;
        lastSize = size;
        if (NowMs() > until) break;
        ::Sleep(kWavPollMs);
    }
    if (!wav) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        const bool there = ::GetFileAttributesExW(wavPath.c_str(), GetFileExInfoStandard, &fa) != 0;
        UE_LOGW("camcorder_voice: the wav did not open for writing (errno %d; %hs, %lu bytes)", errno,
                there ? "it exists" : "it is not there", there ? fa.nFileSizeLow : 0ul);
        return false;
    }
    WavLayout L;
    if (!ReadWavLayout(wav, L)) {
        std::fclose(wav);
        return false;
    }
    FILE* raw = _wfopen(p.rawPath.c_str(), L"rb");
    if (!raw) {
        std::fclose(wav);
        return false;
    }
    const uint64_t wavFrames = L.dataBytes / (2u * L.channels);
    std::vector<int16_t> block(kMixChunkFrames * L.channels);
    std::vector<int16_t> track;   // the track's frames this wav chunk spans, read per chunk
    uint64_t mixed = 0;
    for (uint64_t at = 0; at < wavFrames; at += kMixChunkFrames) {
        const size_t n = static_cast<size_t>(wavFrames - at < kMixChunkFrames ? wavFrames - at : kMixChunkFrames);
        const long pos = L.dataAt + static_cast<long>(at * 2u * L.channels);
        std::fseek(wav, pos, SEEK_SET);
        if (std::fread(block.data(), 2u * L.channels, n, wav) != n) break;
        // The track's frame at a wav frame's time is frame * 48000 / rate: this chunk reads [k0, k1].
        const uint64_t k0 = at * kTrackRate / L.rate;
        const uint64_t k1 = (at + n - 1) * kTrackRate / L.rate;
        if (k0 >= p.frames) break;
        track.assign(2 * static_cast<size_t>(k1 - k0 + 1), 0);
        _fseeki64(raw, static_cast<int64_t>(k0 * 2 * sizeof(int16_t)), SEEK_SET);
        const uint64_t got = k0 + std::fread(track.data(), 2 * sizeof(int16_t), static_cast<size_t>(k1 - k0 + 1), raw);
        bool touched = false;
        for (size_t i = 0; i < n; ++i) {
            const uint64_t k = (at + i) * kTrackRate / L.rate;
            if (k >= got) break;
            const int vl = track[2 * (k - k0)], vr = track[2 * (k - k0) + 1];
            auto add = [](int16_t& s, int v) {
                int x = s + v;
                if (x > 32767) x = 32767;
                if (x < -32768) x = -32768;
                s = static_cast<int16_t>(x);
            };
            if (L.channels >= 2) {
                add(block[i * L.channels + 0], vl);
                add(block[i * L.channels + 1], vr);
            } else {
                add(block[i], (vl + vr) / 2);
            }
            touched = true;
            ++mixed;
        }
        if (!touched) break;
        std::fseek(wav, pos, SEEK_SET);
        std::fwrite(block.data(), 2u * L.channels, n, wav);
    }
    std::fclose(raw);
    std::fclose(wav);
    *mixedFrames = mixed;
    return true;
}

bool Contains(const wchar_t* s, const wchar_t* needle) { return s && std::wcsstr(s, needle) != nullptr; }

// The executable a launch names: the application, else the command line's first token.
std::wstring ExeOf(const wchar_t* app, const wchar_t* cmd) {
    if (app && *app) return app;
    if (!cmd) return {};
    std::wstring c(cmd);
    if (!c.empty() && c[0] == L'"') {
        const size_t end = c.find(L'"', 1);
        return c.substr(1, end == std::wstring::npos ? std::wstring::npos : end - 1);
    }
    return c.substr(0, c.find(L' '));
}

void BeforeLaunch(const wchar_t* app, const wchar_t* cmd) {
    if (!Contains(app, L"MSC_Merger") && !Contains(cmd, L"MSC_Merger")) return;
    // The plugin launches its merger from inside the manager's stopRecord, before a tick sees the recording end: the
    // track is finished here, on the game thread that stops it.
    if (g_recording) {
        if (g_pb && ue_wrap::game_thread::IsGameThread()) {
            g_pb->SetCamera(false, 0, 0, 0, 0, -1);
            End(*g_pb);
            g_recording = false;
            g_wantsSelf = false;
        } else {
            UE_LOGW("camcorder_voice: the merger launches off the game thread while the track still records -- not added");
            return;
        }
    }
    Pending p;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_havePending || NowMs() > g_pending.deadlineMs) {
            DropPendingLocked();
            UE_LOGI("camcorder_voice: the recorder's merger launches with no voice track to add");
            return;
        }
        p = g_pending;
        g_havePending = false;
        g_pending = Pending{};
    }
    const std::wstring wav = WavFromMergeInfo(ExeOf(app, cmd));
    // [dev] VOTVCOOP_CAMVOICE_KEEP=<path stem>: the wav as the game wrote it and as it is merged, kept for a check.
    static const std::string keep = coop::config::ReadEnv("VOTVCOOP_CAMVOICE_KEEP");
    const std::wstring keepW(keep.begin(), keep.end());
    if (!keep.empty() && !wav.empty()) ::CopyFileW(wav.c_str(), (keepW + L".before.wav").c_str(), FALSE);
    uint64_t mixed = 0;
    const bool ok = !wav.empty() && MixInto(wav, p, &mixed);
    if (!keep.empty() && ok) ::CopyFileW(wav.c_str(), (keepW + L".after.wav").c_str(), FALSE);
    ::DeleteFileW(p.rawPath.c_str());
    if (ok)
        UE_LOGI("camcorder_voice: the voice track went into '%ls' before its merge (%llu frame(s) of it mixed)",
                wav.c_str(), static_cast<unsigned long long>(mixed));
    else
        UE_LOGW("camcorder_voice: the voice track could not go into the capture's wav ('%ls'); the video keeps the game's "
                "sound alone", wav.empty() ? L"MergeInfo.ini did not name one" : wav.c_str());
}

}  // namespace

void Install() {
    static bool s_done = false;
    if (s_done) return;
    s_done = ue_wrap::process_launch::Install(&BeforeLaunch);
}

bool WantsSelf() { return g_wantsSelf; }

void Tick(coop::voice::Playback& pb, int localSlot, bool loopback) {
    g_pb = &pb;
    void* const m = Manager();
    const bool recording = m && Bool(m, L"bIsRecording");
    const bool paused = recording && Bool(m, L"bIsPaused");
    if (recording && !g_recording) Begin(m, pb);
    if (!recording && g_recording) {
        pb.SetCamera(false, 0, 0, 0, 0, -1);
        End(pb);
    }
    g_recording = recording;
    g_paused = paused;
    if (!recording) {
        g_wantsSelf = false;
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_havePending && NowMs() > g_pending.deadlineMs) {
            UE_LOGI("camcorder_voice: no merger took the last recording's voice track in %llu s -- dropped",
                    static_cast<unsigned long long>(kPendingBoundMs / 1000));
            DropPendingLocked();
        }
        return;
    }
    // The camera's place: the camcorder the manager records with, held or on a tripod; the player when unread.
    void* const player = coop::players::Registry::Get().Local();
    void* camera = Obj(m, L"camprop");
    ue_wrap::FVector at{};
    ue_wrap::FRotator rot{};
    if (!camera || !R::IsLive(camera) || !E::TryGetActorLocation(camera, at)) camera = player;
    if (camera) {
        E::TryGetActorLocation(camera, at);
        E::TryGetActorRotation(camera, rot);
    }
    // This peer's own voice: the game's microphone records it when the audio settings let it; else the camera hears
    // its voice chat from where the player stands.
    const bool gameMic = Bool(m, L"micEnabled") && Bool(m, L"micPermission");
    g_wantsSelf = !paused && !gameMic && !loopback && localSlot >= 0 && localSlot < coop::players::kMaxPeers;
    ue_wrap::FVector me{};
    if (g_wantsSelf && player && E::TryGetActorLocation(player, me)) pb.SetSpeaker(localSlot, me.X, me.Y, me.Z, true);
    pb.SetCamera(!paused, at.X, at.Y, at.Z, rot.Yaw, g_wantsSelf ? localSlot : -1);
    Append(pb);
}

}  // namespace coop::voice::camcorder_voice
