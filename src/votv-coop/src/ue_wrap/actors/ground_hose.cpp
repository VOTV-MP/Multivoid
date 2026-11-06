// ue_wrap/actors/ground_hose.cpp -- see ue_wrap/actors/ground_hose.h.

#include "ue_wrap/actors/ground_hose.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <cstdint>
#include <cstring>

namespace ue_wrap::ground_hose {
namespace {

namespace R = reflection;
namespace SR = save_record;

template <class T>
bool ReadField(void* obj, const wchar_t* name, T& out) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(obj) + off, sizeof(out));
    return true;
}

template <class T>
bool WriteField(void* obj, const wchar_t* name, const T& v) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return false;
    std::memcpy(static_cast<uint8_t*>(obj) + off, &v, sizeof(v));
    return true;
}

void* LiveObj(void* obj, const wchar_t* name) {
    void* v = nullptr;
    return ReadField(obj, name, v) && v && R::IsLive(v) ? v : nullptr;
}

bool CallNoArgs(void* obj, const wchar_t* name) {
    void* fn = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), name) : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && Call(obj, f);
}

std::wstring SaveKey(void* actor) {
    R::FName key{};
    return ReadField(actor, L"Key", key) ? R::ToString(key) : std::wstring();
}

bool Default(void* obj) { return R::NameStartsWith(R::NameOf(obj), L"Default__"); }

}  // namespace

void* HoseClass() { return object_index::ClassByName(L"groundHose_C"); }

void ListHoses(std::vector<void*>& out) {
    out.clear();
    void* cls = HoseClass();
    if (!cls) return;
    object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        if (R::IsLive(obj) && !Default(obj)) static_cast<std::vector<void*>*>(ctx)->push_back(obj);
    }, &out);
}

bool IsLaying(void* hose) { return engine::IsActorTickEnabled(hose); }

std::wstring KeyOf(void* hose) { return SaveKey(hose); }

void* FaucetOf(void* hose) { return LiveObj(hose, L"faucet"); }
void* SprinklerOf(void* hose) { return LiveObj(hose, L"sprinkler"); }

bool Capture(void* hose, SR::SaveRecord& out) {
    void* fn = hose ? R::FindDispatchFunctionCached(R::ClassOf(hose), L"getData") : nullptr;
    if (!fn || !SR::CaptureRecordVia(hose, fn, out)) return false;
    // getData refreshes a key only from a tied end, so an end cut since keeps its old key: the record says None.
    if (!out.names.empty() && out.names[0].size() == 2) {
        if (!FaucetOf(hose)) out.names[0][0] = L"None";
        if (!SprinklerOf(hose)) out.names[0][1] = L"None";
    }
    return true;
}

void* FindFaucet(const std::wstring& key) {
    void* cls = object_index::ClassByName(L"faucet_C");
    if (!cls || key.empty() || key == L"None") return nullptr;
    struct Find {
        const std::wstring* key;
        void* found;
    } find{&key, nullptr};
    object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        auto* f = static_cast<Find*>(ctx);
        if (!f->found && R::IsLive(obj) && !Default(obj) && SaveKey(obj) == *f->key) f->found = obj;
    }, &find);
    return find.found;
}

void* Spawn(const FVector& loc) {
    void* cls = HoseClass();
    if (!cls) return nullptr;
    void* actor = engine::BeginDeferredSpawn(cls, loc, {});
    if (!actor) return nullptr;
    if (!engine::FinishDeferredSpawn(actor, loc, {})) {
        engine::DestroyActor(actor);
        return nullptr;
    }
    return actor;
}

bool Apply(void* hose, const SR::SaveRecord& r, void* faucet, void* sprinkler) {
    void* load = hose ? R::FindDispatchFunctionCached(R::ClassOf(hose), L"loadData") : nullptr;
    if (!load || !SR::ApplyRecordVia(hose, load, r)) return false;
    void* had = SprinklerOf(hose);
    if (had && had != sprinkler) CallDetachSprinkler(hose);
    WriteField(hose, L"faucet", faucet);
    if (faucet) WriteField(faucet, L"hose", hose);
    if (sprinkler) {
        WriteField(hose, L"sprinkler", sprinkler);
        WriteField(sprinkler, L"hose", hose);
    }
    // Finish: the tick off, the meshes built from the points, collision on and the faucet refreshed, which refreshes
    // the sprinkler through the hose.
    return CallNoArgs(hose, L"Finish");
}

bool CallDetachSprinkler(void* hose) { return CallNoArgs(hose, L"detachSprinkler"); }

bool RecordKeys(const SR::SaveRecord& r, std::wstring& faucetKey, std::wstring& sprinklerKey) {
    if (r.names.empty() || r.names[0].size() != 2) return false;
    faucetKey = r.names[0][0];
    sprinklerKey = r.names[0][1];
    return true;
}

}  // namespace ue_wrap::ground_hose
