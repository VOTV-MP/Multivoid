// coop/player/player_profile_store.cpp -- see coop/player/player_profile_store.h.

#include "coop/player/player_profile_store.h"

#include "coop/atomic_file/atomic_file.h"
#include "coop/mod_data/legacy_note.h"
#include "coop/net/blob_chunks.h"  // Fnv64, the file's integrity hash
#include "coop/save/save_transfer.h"  // HostSlot, the world the profiles belong to
#include "coop/session/player_handshake.h"  // IsValidGuid
#include "coop/text/utf8_codec.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace coop::player_profile_store {
namespace {

namespace fs = std::filesystem;

// The newest profile per GUID this host session has been handed, and the copy of it taken the last
// time the world was gathered, which is the one a save writes.
struct Held {
    std::vector<uint8_t> blob;
    std::string          nick;
    uint64_t             hash = 0;
    bool                 changed = false;   // since the last gather
    std::vector<uint8_t> atGather;          // valid while `uncut`
    std::string          nickAtGather;
    bool                 uncut = false;     // gathered, not yet on disk
};
std::unordered_map<std::string, Held> g_held;
std::unordered_set<std::string> g_quarantined;  // stored profile unreadable: never written over

// The slot whose directory holds this world's profile set on disk: the slot the world was loaded
// from, then whichever slot a save of it was last written to. Empty while the world has no file.
std::wstring g_setSlot;

// What is held belongs to ONE loaded world. A process names the slot once per world it loads
// (save_transfer::SetHostSlot), so a new serial means another world took this one's place -- and
// that world is as old as its save, so the profiles to go with it are the files', not the ones
// held from the world before.
uint32_t g_heldForSerial = 0;
void DropHeldOfAnotherWorld() {
    const uint32_t serial = coop::save_transfer::HostSlotSerial();
    if (serial == g_heldForSerial) return;
    if (!g_held.empty())
        UE_LOGI("player_profile: another world was loaded to host -- %zu held profile(s) of the "
                "previous one dropped", g_held.size());
    g_held.clear();
    g_quarantined.clear();
    g_setSlot = coop::save_transfer::HostSlot();
    g_heldForSerial = serial;
}

// <mod data dir>/coop_players/<slot>, empty if a piece is missing. The slot name is the game's and
// becomes ONE path component, so a name that could be more than one is refused.
fs::path SlotDir(const std::wstring& slot) {
    if (slot.empty() || slot == L"." || slot == L".." ||
        slot.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos)
        return {};
    const std::wstring base = ue_wrap::paths::ModDataDir();
    if (base.empty()) return {};
    return fs::path(base) / L"coop_players" / slot;
}

// <mod data dir>/coop_players/<the set's slot>/<guid>.json, empty if any piece is missing.
fs::path PlayerFilePath(const std::string& guid) {
    // Defence in depth (the wire boundary already validates): a GUID that is not exactly 32 hex
    // characters never becomes a path component; an empty path makes every write no-op, so a
    // non-hex GUID cannot traverse.
    if (!coop::player_handshake::IsValidGuid(guid)) return {};
    const fs::path dir = SlotDir(g_setSlot);
    if (dir.empty()) return {};
    return dir / (std::wstring(guid.begin(), guid.end()) + L".json");
}

// Is this one of the set's files: <32 hex>.json, or the .bak of one?
bool IsProfileFile(const fs::path& file) {
    std::wstring name = file.filename().wstring();
    if (name.size() > 4 && name.compare(name.size() - 4, 4, L".bak") == 0) name.resize(name.size() - 4);
    if (name.size() != 32 + 5 || name.compare(32, 5, L".json") != 0) return false;
    std::string guid;
    for (size_t i = 0; i < 32; ++i) {
        if (name[i] > 0x7F) return false;
        guid.push_back(static_cast<char>(name[i]));
    }
    return coop::player_handshake::IsValidGuid(guid);
}

// The set's files in `dir`. False when the directory could not be read to its end: a listing cut
// short is not the set, and a caller that removes what is "not in the set" must not act on one.
bool ProfileFilesIn(const fs::path& dir, std::vector<fs::path>& out) {
    out.clear();
    bool everyEntryAnswered = true;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        // Its own error code: the step to the next entry writes `ec` again, and a failure left in
        // it would be gone by the time the loop is over.
        std::error_code entryEc;
        const bool regular = it->is_regular_file(entryEc);
        if (entryEc) { everyEntryAnswered = false; continue; }
        if (regular && IsProfileFile(it->path())) out.push_back(it->path());
    }
    return everyEntryAnswered && !ec;
}

std::string Hex(const std::vector<uint8_t>& b) {
    static const char k[] = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (uint8_t c : b) { s.push_back(k[c >> 4]); s.push_back(k[c & 0xF]); }
    return s;
}

// Decode a hex string of either case to bytes. False on an odd length or a non-hex digit.
bool UnHex(const std::string& s, std::vector<uint8_t>& out) {
    if (s.size() & 1) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    out.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        const int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

// Extract the value of a string field from our flat JSON (no nesting or escapes are used).
bool JsonStr(const std::string& doc, const char* key, std::string& out) {
    std::string needle = std::string("\"") + key + "\":\"";
    const size_t k = doc.find(needle);
    if (k == std::string::npos) return false;
    const size_t v = k + needle.size();
    const size_t e = doc.find('"', v);
    if (e == std::string::npos) return false;
    out = doc.substr(v, e - v);
    return true;
}

// Parse one stored file into `outBlob`. Defensive against a hand edit: requires the magic, a
// parseable hex blob and a matching FNV. False on any failure (the caller tries the .bak); never
// throws, never returns unverified bytes.
bool ParseBlobFile(const fs::path& file, std::vector<uint8_t>& outBlob) {
    std::error_code ec;
    if (file.empty() || !fs::exists(file, ec)) return false;
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    std::string doc((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string magic;
    if (!JsonStr(doc, "magic", magic) || magic != "VCPI") {
        UE_LOGW("player_profile: '%ls' missing/bad magic -- treating as corrupt", file.c_str());
        return false;
    }
    std::string blobHex, fnvHex;
    if (!JsonStr(doc, "blob", blobHex) || !JsonStr(doc, "fnv", fnvHex)) return false;
    std::vector<uint8_t> blob;
    if (!UnHex(blobHex, blob)) {
        UE_LOGW("player_profile: '%ls' blob hex unparseable -- corrupt", file.c_str());
        return false;
    }
    char want[17] = {};
    std::snprintf(want, sizeof(want), "%016llx",
                  static_cast<unsigned long long>(coop::blob_chunks::Fnv64(blob)));
    if (fnvHex != want) {
        UE_LOGW("player_profile: '%ls' FNV mismatch (have %s, stored %s) -- corrupt/tampered",
                file.c_str(), want, fnvHex.c_str());
        return false;
    }
    outBlob = std::move(blob);
    return true;
}

bool ReadFile(const std::string& guid, std::vector<uint8_t>& outBlob) {
    const fs::path file = PlayerFilePath(guid);
    if (file.empty()) return false;
    if (ParseBlobFile(file, outBlob)) return true;
    const fs::path bak = fs::path(file).concat(L".bak");
    if (ParseBlobFile(bak, outBlob)) {
        UE_LOGW("player_profile: recovered guid=%s inventory from .bak (primary corrupt)",
                guid.c_str());
        return true;
    }
    return false;
}

bool WriteFile(const std::string& guid, const std::vector<uint8_t>& blob, const std::string& nickJson) {
    const fs::path file = PlayerFilePath(guid);
    if (file.empty()) return false;
    std::error_code ec;
    // Keep the last good file as .bak before overwriting.
    if (fs::exists(file, ec))
        fs::copy_file(file, fs::path(file).concat(L".bak"), fs::copy_options::overwrite_existing, ec);
    const uint64_t fnv = coop::blob_chunks::Fnv64(blob);
    const long long epoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    char fnvhex[17] = {};
    std::snprintf(fnvhex, sizeof(fnvhex), "%016llx", static_cast<unsigned long long>(fnv));
    std::string json = "{\"magic\":\"VCPI\",\"ver\":1,\"fnv\":\"";
    json += fnvhex;
    json += "\",\"nick\":\"";
    json += nickJson;
    json += "\",\"lastSeen\":";
    json += std::to_string(epoch);
    json += ",\"blob\":\"";
    json += Hex(blob);
    json += "\"}\n";
    const atomic_file::Result r =
        atomic_file::Write(file, json, atomic_file::Mode::Replace, atomic_file::Sync::ToDisk);
    if (!r.ok()) {
        UE_LOGE("player_profile: write of '%ls' failed (%s)", file.c_str(),
                atomic_file::Describe(r).c_str());
        return false;
    }
    return true;
}

// A save of this world was written to `written`, which is not where the set stands: a quicksave
// names a new <main>_SUB_<n> file, and a plain save made after loading a subsave names the main
// slot (saveSlot_C::saveToSlot). That file now holds THIS world, so the directory beside it has to
// hold this world's set and nothing else: what stood there belonged to the world the file held
// before, and a player of that world would come back carrying items this one still has on the
// floor. The set is copied byte for byte -- a file that does not read stays as it is there too --
// and stands under `written` from then on. False leaves everything as it was for the next save.
//
// Nothing is removed before every copy has landed, and nothing at all on an answer that is not an
// answer: a directory that cannot be asked about is not an absent one, and a listing cut short is
// not the set. With no slot known for this world (a process never told which file it came from)
// the lineage is unknown too, so the directory beside the written file is taken as it stands; a
// save the host picker creates is cleared where it is created (ForgetSlot).
bool FollowWrittenSlot(const std::wstring& written) {
    if (written == g_setSlot) return true;
    const fs::path to = SlotDir(written);
    std::error_code ec;
    if (!to.empty()) fs::create_directories(to, ec);
    if (to.empty() || ec) {
        UE_LOGW("player_profile: the profile directory of the slot '%ls' could not be made (%s) -- "
                "nothing cut, everything stays for the next save", written.c_str(),
                to.empty() ? "not a name for one directory, or no data folder" : ec.message().c_str());
        return false;
    }
    if (g_setSlot.empty()) {
        UE_LOGI("player_profile: the world was saved to '%ls' and no slot was known for it -- its "
                "profiles stand there from now on, the directory taken as it is", written.c_str());
        g_setSlot = written;
        return true;
    }
    const fs::path from = SlotDir(g_setSlot);
    const fs::file_type fromIs = from.empty() ? fs::file_type::not_found : fs::status(from, ec).type();
    if (fromIs != fs::file_type::directory && fromIs != fs::file_type::not_found) {
        UE_LOGE("player_profile: '%ls' could not be asked about (%s) -- nothing cut, everything "
                "stays for the next save", from.c_str(), ec.message().c_str());
        return false;
    }
    const bool haveFrom = fromIs == fs::file_type::directory;
    if (haveFrom && fs::equivalent(from, to, ec)) {  // one directory under two spellings
        g_setSlot = written;
        return true;
    }
    std::vector<fs::path> set, there;
    if ((haveFrom && !ProfileFilesIn(from, set)) || !ProfileFilesIn(to, there)) {
        UE_LOGE("player_profile: the profiles under '%ls' or '%ls' could not be listed -- nothing "
                "cut, everything stays for the next save", g_setSlot.c_str(), written.c_str());
        return false;
    }
    // Names compared the way the file system compares them: one file under two spellings of its
    // hex would otherwise be copied and then removed as a stranger.
    auto folded = [](const fs::path& file) {
        std::wstring name = file.filename().wstring();
        for (wchar_t& c : name)
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        return name;
    };
    std::unordered_set<std::wstring> names;
    for (const fs::path& file : set) {
        // Cached: the old slot keeps the intact set, so a power cut just after the copy can cost
        // the new slot's copy and never the old one.
        // A stream iterator stops at the first read error without setting badbit, so the bytes are
        // checked against the size the file system gave before the read.
        std::string bytes;
        std::string why = "could not read";
        {
            std::error_code sizeEc;
            const std::uintmax_t size = fs::file_size(file, sizeEc);
            std::ifstream in(file, std::ios::binary);
            if (in && !sizeEc) {
                bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                if (bytes.size() == size) why.clear();
            }
        }
        if (why.empty()) {
            const atomic_file::Result r = atomic_file::Write(
                to / file.filename(), bytes, atomic_file::Mode::Replace, atomic_file::Sync::Cached);
            if (!r.ok()) why = atomic_file::Describe(r);
        }
        if (!why.empty()) {
            UE_LOGE("player_profile: copying '%ls' beside the slot '%ls' failed (%s) -- nothing "
                    "removed there, nothing cut, everything stays for the next save", file.c_str(),
                    written.c_str(), why.c_str());
            return false;
        }
        names.insert(folded(file));
    }
    size_t removed = 0;
    for (const fs::path& stale : there)
        if (!names.count(folded(stale)) && fs::remove(stale, ec)) ++removed;
    UE_LOGI("player_profile: the world was saved to '%ls', its profiles stood under '%ls' -- %zu "
            "file(s) of the set copied beside the new file, %zu left by the world that slot held "
            "before removed", written.c_str(), g_setSlot.c_str(), set.size(), removed);
    g_setSlot = written;
    return true;
}

}  // namespace

// Encoding is the codec's job; escaping is this site's, because the container is JSON. Raw UTF-8
// is valid inside a JSON string, only the two structural metacharacters have to go, and they
// cannot appear inside a multi-byte sequence (continuation bytes are all above 0x7F), so dropping
// them cannot corrupt one.
std::string NickForJson(const std::wstring& nick) {
    std::string s = coop::text::CapUtf8Bytes(coop::text::ToUtf8(nick), coop::text::kNickMaxBytes);
    s.erase(std::remove_if(s.begin(), s.end(),
                           [](char c) { return c == '"' || c == '\\'; }),
            s.end());
    return s;
}

bool Put(const std::string& guid, std::vector<uint8_t> blob, const std::string& nickJson) {
    if (!coop::player_handshake::IsValidGuid(guid)) return false;
    DropHeldOfAnotherWorld();
    if (g_quarantined.count(guid)) return false;
    const uint64_t hash = coop::blob_chunks::Fnv64(blob);
    Held& h = g_held[guid];
    if (h.hash == hash && h.blob == blob) return false;
    h.blob = std::move(blob);
    h.nick = nickJson;
    h.hash = hash;
    h.changed = true;
    return true;
}

Found Get(const std::string& guid, std::vector<uint8_t>& outBlob) {
    DropHeldOfAnotherWorld();
    const auto it = g_held.find(guid);
    if (it != g_held.end()) { outBlob = it->second.blob; return Found::Yes; }
    if (ReadFile(guid, outBlob)) return Found::Yes;
    const fs::path file = PlayerFilePath(guid);
    std::error_code ec;
    const bool onDisk = !file.empty() &&
                        (fs::exists(file, ec) || fs::exists(fs::path(file).concat(L".bak"), ec));
    return onDisk ? Found::Unreadable : Found::Absent;
}

void Quarantine(const std::string& guid) {
    DropHeldOfAnotherWorld();
    g_held.erase(guid);
    if (g_quarantined.insert(guid).second)
        UE_LOGE("player_profile: the stored profile of guid=%s did NOT read -- nothing of this player "
                "is held or written for the rest of this hosted world; the files are left as they "
                "are for recovery", guid.c_str());
}

void NoteLegacyFolder() {
    coop::legacy_note::Once(coop::legacy_note::LegacyFile::CoopPlayers, L"coop_players");
}

size_t ForgetSlot(const std::wstring& slot) {
    const fs::path dir = SlotDir(slot);
    size_t removed = 0;
    std::error_code ec;
    // A listing cut short still names files that are there, and every one of them is a stranger
    // to a save that was just created: what it did list goes.
    std::vector<fs::path> files;
    if (!dir.empty() && fs::is_directory(dir, ec)) ProfileFilesIn(dir, files);
    for (const fs::path& file : files)
        if (fs::remove(file, ec)) ++removed;
    if (removed)
        UE_LOGI("player_profile: the save '%ls' is new -- %zu profile file(s) a deleted save of that "
                "name had left beside it removed", slot.c_str(), removed);
    return removed;
}

bool AnythingPending() {
    DropHeldOfAnotherWorld();
    for (const auto& [guid, h] : g_held)
        if (h.changed || h.uncut) return true;
    return false;
}

void MarkWorldGathered() {
    DropHeldOfAnotherWorld();
    for (auto& [guid, h] : g_held) {
        if (!h.changed) continue;
        h.atGather = h.blob;
        h.nickAtGather = h.nick;
        h.uncut = true;
        h.changed = false;
    }
}

size_t CutToDisk(const std::wstring& writtenSlot) {
    DropHeldOfAnotherWorld();
    if (!FollowWrittenSlot(writtenSlot)) return 0;
    size_t written = 0;
    bool dirReady = false;
    for (auto it = g_held.begin(); it != g_held.end();) {
        Held& h = it->second;
        if (h.uncut) {
            if (!dirReady) {  // one directory for every profile of this world
                const fs::path dir = SlotDir(g_setSlot);
                std::error_code ec;
                if (!dir.empty()) fs::create_directories(dir, ec);
                if (dir.empty() || ec) {
                    UE_LOGW("player_profile: the profile directory could not be made (%s) -- "
                            "nothing cut, everything stays for the next save",
                            dir.empty() ? "no data folder" : ec.message().c_str());
                    return 0;
                }
                dirReady = true;
            }
            if (WriteFile(it->first, h.atGather, h.nickAtGather)) {
                h.uncut = false;
                h.atGather = {};
                ++written;
            }  // else: stays for the next cut
        }
        // What is on disk and unchanged since is the file's to give back (Get reads it), so it
        // need not be held: a host meets new identities for as long as its world lives.
        if (!h.uncut && !h.changed) it = g_held.erase(it);
        else ++it;
    }
    return written;
}

}  // namespace coop::player_profile_store
