#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace fixture {
struct Actor {
    bool opened = false;
    uint64_t name = 0;
    bool lid = false;
    float top = -1, bottom = -1;
    std::wstring key = L"box";
    int record = 0;
};
inline std::vector<int> records;
inline bool publishOk = true, quiesced = true;
}

namespace coop::net {
enum class Role { Host, Client };
struct Session {
    Role side = Role::Host;
    bool live = true;
    bool connected() const { return live; }
    Role role() const { return side; }
};
}
namespace coop::join_membership_sweep {
inline bool HasLoadTailQuiesced() { return fixture::quiesced; }
}
namespace coop::prop_save_data {
inline bool Publish(net::Session*, void* actor, const std::wstring&) {
    fixture::records.push_back(static_cast<fixture::Actor*>(actor)->record);
    return fixture::publishOk;
}
}
namespace coop::props::prop_record_refresh {
void Install(net::Session*);
void OnDisconnect();
}

namespace ue_wrap::game_thread { inline bool IsGameThread() { return true; } }
namespace ue_wrap::reflection {
struct FName { uint64_t value = 0; };
inline void* ClassOf(void* actor) { return actor; }
inline void* FindClass(const wchar_t*) { return nullptr; }
inline void* FindDispatchFunctionCached(void*, const wchar_t*) { return nullptr; }
inline int32_t FindPropertyOffset(void*, const wchar_t* name) {
    if (std::wcscmp(name, L"opened") == 0) return offsetof(fixture::Actor, opened);
    if (std::wcscmp(name, L"name") == 0) return offsetof(fixture::Actor, name);
    if (std::wcscmp(name, L"lid") == 0) return offsetof(fixture::Actor, lid);
    if (std::wcscmp(name, L"reeltop") == 0) return offsetof(fixture::Actor, top);
    if (std::wcscmp(name, L"reelBottom") == 0) return offsetof(fixture::Actor, bottom);
    return -1;
}
}
namespace ue_wrap::prop {
inline std::wstring GetPropNameString(void*) { return L"drivebox_s"; }
inline std::wstring GetInteractableKeyString(void* actor) {
    return static_cast<fixture::Actor*>(actor)->key;
}
}
namespace ue_wrap::fname_utils {
inline reflection::FName StringToFName(const wchar_t*) { return {}; }
}
namespace ue_wrap {
struct ParamFrame {
    explicit ParamFrame(void*) {}
    bool valid() const { return false; }
};
inline void Call(void*, ParamFrame&) {}
}
namespace ue_wrap::facts {
struct Function {
    const wchar_t* cls;
    const wchar_t* name;
    const wchar_t* Class() const { return cls; }
    const wchar_t* Name() const { return name; }
};
struct Resolved {
    void* fn = nullptr;
    void* Fn() const { return fn; }
    explicit operator bool() const { return fn != nullptr; }
};
inline Resolved Resolve(const Function&) { return {}; }
}
namespace ue_wrap::script_gate {
struct Call { void* object; int tag; bool fromOurCode; };
using PostFn = void (*)(const Call&);
inline bool Watch(facts::Resolved, int, std::nullptr_t, PostFn) { return true; }
inline bool Unwatch(facts::Resolved, int, std::nullptr_t, PostFn) { return true; }
}
#define UE_FACT_FN(row, owner, cls, name, lookup) \
    const ue_wrap::facts::Function row{cls, name}
#define UE_LOGI(...) ((void)0)
#define UE_LOGW(...) ((void)0)
