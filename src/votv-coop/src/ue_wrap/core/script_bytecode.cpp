// ue_wrap/core/script_bytecode.cpp -- the decoder of script_bytecode.h. The opcode operand table follows
// UAssetAPI's Kismet expression readers (MIT, Atenfyr; THIRD-PARTY-NOTICES.md), whose iCode accounting is
// the in-memory layout; every rule of the canonical text is docs/bytecode-fingerprint.md's.

#include "script_bytecode.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <iterator>

namespace ue_wrap::script_fingerprint::detail {
namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

namespace {

// Operand kinds: an expression, a list of them, an object, a property, a name, the fixed-size values, a code
// offset, and the derived skips and counts the canonical text drops.
enum Kind : uint8_t {
    kNone = 0, kExpr, kList, kObj, kProp, kName, kI32, kCode, kSkip, kU8, kU16, kBool, kI64, kU64, kF32, kF64,
    kStr, kWStr, kCount,
};
enum Special : uint8_t { sTable = 0, sText, sSwitch, sCast, sFloats3, sTransform, sInstrumentation };

struct Op {
    const char* name = nullptr;  // without EX_; null = not an opcode
    Kind        ops[4] = {};
    Special     special = sTable;
    uint8_t     listEnd = 0;     // the token a kList operand ends on
};

const Op* Table() {
    static Op t[256];
    static bool built = false;
    if (built) return t;
    auto set = [](uint8_t code, const char* name, std::initializer_list<Kind> ops, Special sp = sTable,
                  uint8_t end = 0) {
        Op& o = t[code];
        o.name = name;
        int i = 0;
        for (Kind k : ops) o.ops[i++] = k;
        o.special = sp;
        o.listEnd = end;
    };
    constexpr uint8_t kParms = 0x16;
    set(0x00, "LocalVariable", {kProp});
    set(0x01, "InstanceVariable", {kProp});
    set(0x02, "DefaultVariable", {kProp});
    set(0x04, "Return", {kExpr});
    set(0x06, "Jump", {kCode});
    set(0x07, "JumpIfNot", {kCode, kExpr});
    set(0x09, "Assert", {kU16, kBool, kExpr});
    set(0x0B, "Nothing", {});
    set(0x0F, "Let", {kProp, kExpr, kExpr});
    set(0x12, "ClassContext", {kExpr, kSkip, kProp, kExpr});
    set(0x13, "MetaCast", {kObj, kExpr});
    set(0x14, "LetBool", {kExpr, kExpr});
    set(0x15, "EndParmValue", {});
    set(0x16, "EndFunctionParms", {});
    set(0x17, "Self", {});
    set(0x18, "Skip", {kSkip, kExpr});
    set(0x19, "Context", {kExpr, kSkip, kProp, kExpr});
    set(0x1A, "Context_FailSilent", {kExpr, kSkip, kProp, kExpr});
    set(0x1B, "VirtualFunction", {kName, kList}, sTable, kParms);
    set(0x1C, "FinalFunction", {kObj, kList}, sTable, kParms);
    set(0x1D, "IntConst", {kI32});
    set(0x1E, "FloatConst", {kF32});
    set(0x1F, "StringConst", {kStr});
    set(0x20, "ObjectConst", {kObj});
    set(0x21, "NameConst", {kName});
    set(0x22, "RotationConst", {}, sFloats3);
    set(0x23, "VectorConst", {}, sFloats3);
    set(0x24, "ByteConst", {kU8});
    set(0x25, "IntZero", {});
    set(0x26, "IntOne", {});
    set(0x27, "True", {});
    set(0x28, "False", {});
    set(0x29, "TextConst", {}, sText);
    set(0x2A, "NoObject", {});
    set(0x2B, "TransformConst", {}, sTransform);
    set(0x2C, "IntConstByte", {kU8});
    set(0x2D, "NoInterface", {});
    set(0x2E, "DynamicCast", {kObj, kExpr});
    set(0x2F, "StructConst", {kObj, kI32, kList}, sTable, 0x30);
    set(0x30, "EndStructConst", {});
    set(0x31, "SetArray", {kExpr, kList}, sTable, 0x32);
    set(0x32, "EndArray", {});
    set(0x33, "PropertyConst", {kProp});
    set(0x34, "UnicodeStringConst", {kWStr});
    set(0x35, "Int64Const", {kI64});
    set(0x36, "UInt64Const", {kU64});
    set(0x37, "DoubleConst", {kF64});
    set(0x38, "PrimitiveCast", {}, sCast);
    set(0x39, "SetSet", {kExpr, kCount, kList}, sTable, 0x3A);
    set(0x3A, "EndSet", {});
    set(0x3B, "SetMap", {kExpr, kCount, kList}, sTable, 0x3C);
    set(0x3C, "EndMap", {});
    set(0x3D, "SetConst", {kProp, kCount, kList}, sTable, 0x3E);
    set(0x3E, "EndSetConst", {});
    set(0x3F, "MapConst", {kProp, kProp, kCount, kList}, sTable, 0x40);
    set(0x40, "EndMapConst", {});
    set(0x42, "StructMemberContext", {kProp, kExpr});
    set(0x43, "LetMulticastDelegate", {kExpr, kExpr});
    set(0x44, "LetDelegate", {kExpr, kExpr});
    set(0x45, "LocalVirtualFunction", {kName, kList}, sTable, kParms);
    set(0x46, "LocalFinalFunction", {kObj, kList}, sTable, kParms);
    set(0x48, "LocalOutVariable", {kProp});
    set(0x4A, "DeprecatedOp4A", {});
    set(0x4B, "InstanceDelegate", {kName});
    set(0x4C, "PushExecutionFlow", {kCode});
    set(0x4D, "PopExecutionFlow", {});
    set(0x4E, "ComputedJump", {kExpr});
    set(0x4F, "PopExecutionFlowIfNot", {kExpr});
    set(0x50, "Breakpoint", {});
    set(0x51, "InterfaceContext", {kExpr});
    set(0x52, "ObjToInterfaceCast", {kObj, kExpr});
    set(0x53, "EndOfScript", {});
    set(0x54, "CrossInterfaceCast", {kObj, kExpr});
    set(0x55, "InterfaceToObjCast", {kObj, kExpr});
    set(0x5A, "WireTracepoint", {});
    set(0x5B, "SkipOffsetConst", {kCode});
    set(0x5C, "AddMulticastDelegate", {kExpr, kExpr});
    set(0x5D, "ClearMulticastDelegate", {kExpr});
    set(0x5E, "Tracepoint", {});
    set(0x5F, "LetObj", {kExpr, kExpr});
    set(0x60, "LetWeakObjPtr", {kExpr, kExpr});
    set(0x61, "BindDelegate", {kName, kExpr, kExpr});
    set(0x62, "RemoveMulticastDelegate", {kExpr, kExpr});
    set(0x63, "CallMulticastDelegate", {kObj, kExpr, kList}, sTable, kParms);
    set(0x64, "LetValueOnPersistentFrame", {kProp, kExpr});
    set(0x65, "ArrayConst", {kProp, kCount, kList}, sTable, 0x66);
    set(0x66, "EndArrayConst", {});
    set(0x67, "SoftObjectConst", {kExpr});
    set(0x68, "CallMath", {kObj, kList}, sTable, kParms);
    set(0x69, "SwitchValue", {}, sSwitch);
    set(0x6A, "InstrumentationEvent", {}, sInstrumentation);
    set(0x6B, "ArrayGetByRef", {kExpr, kExpr});
    set(0x6C, "ClassSparseDataVariable", {kProp});
    set(0x6D, "FieldPathConst", {kExpr});
    built = true;
    return t;
}

void AppendFolded(std::string& out, uint32_t unit, bool lower) {
    if (unit == '"' || unit == '\\') {
        out += '\\';
        out += static_cast<char>(unit);
    } else if (unit >= 0x20 && unit <= 0x7E) {
        char c = static_cast<char>(unit);
        if (lower && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        out += c;
    } else {
        out += '?';
    }
}

std::string FoldLower(const std::wstring& s) {
    std::string out;
    out.reserve(s.size());
    for (wchar_t ch : s) AppendFolded(out, static_cast<uint16_t>(ch), true);
    return out;
}


std::string StripNumber(const std::string& name) {
    std::string out;
    size_t start = 0;
    bool first = true;
    while (start <= name.size()) {
        size_t end = name.find('_', start);
        if (end == std::string::npos) end = name.size();
        std::string part = name.substr(start, end - start);
        bool digits = !part.empty() && std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (first || !digits) {
            if (!first) out += '_';
            out += part;
        }
        first = false;
        start = end + 1;
    }
    return out;
}

}  // namespace

std::string NameCache::Of(int32_t comparisonIndex, uint32_t number) {
    auto it = base.find(comparisonIndex);
    if (it == base.end()) {
        R::FName n{comparisonIndex, 0};
        it = base.emplace(comparisonIndex, FoldLower(R::ToString(n))).first;
    }
    if (number == 0) return it->second;
    return it->second + "_" + std::to_string(number - 1);
}

void Ctx::Fail(const char* what, size_t at) {
    if (!problem.empty()) return;
    char b[96];
    std::snprintf(b, sizeof b, "%s at %zu", what, at);
    problem = b;
}


uint64_t Fnv1a64(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}



std::string ObjPath(Ctx& c, void* obj) {
    std::vector<std::string> parts;
    for (int guard = 0; obj && R::OuterOf(obj) && guard < 64; ++guard) {
        const R::FName& n = R::NameOf(obj);
        parts.push_back(c.names->Of(n.ComparisonIndex, static_cast<uint32_t>(n.Number)));
        obj = R::OuterOf(obj);
    }
    std::string out;
    for (size_t i = parts.size(); i-- > 0;) {
        out += parts[i];
        if (i) out += '.';
    }
    return out;
}

std::string FieldName(Ctx& c, const void* field) {
    const R::FName n = Rd<R::FName>(field, P::off::FField_NamePrivate);
    return c.names->Of(n.ComparisonIndex, static_cast<uint32_t>(n.Number));
}

namespace {

bool IsFunctionClass(Ctx& c, void* obj) {
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    const R::FName& n = R::NameOf(cls);
    const std::string s = c.names->Of(n.ComparisonIndex, static_cast<uint32_t>(n.Number));
    return s == "function" || s == "delegatefunction" || s == "sparsedelegatefunction";
}

std::string PropText(Ctx& c, void* field) {
    if (!field) return "p:-";
    std::vector<std::string> chain;
    void* f = field;
    void* ownerObj = nullptr;
    for (int guard = 0; f && guard < 16; ++guard) {
        chain.push_back(FieldName(c, f));
        void* owner = Rd<void*>(f, P::off::FField_Owner);
        if (Rd<uint8_t>(f, P::off::FField_OwnerIsUObject)) {
            ownerObj = owner;
            break;
        }
        f = owner;
    }
    std::string path = "p:" + (ownerObj ? ObjPath(c, ownerObj) : std::string()) + ":";
    for (size_t i = chain.size(); i-- > 0;) {
        path += chain[i];
        if (i) path += '.';
    }
    if (ownerObj && IsFunctionClass(c, ownerObj)) {
        const int k = c.locals.emplace(field, static_cast<int>(c.locals.size())).first->second;
        return "l" + std::to_string(k) + ":" + StripNumber(chain.front());
    }
    return path;
}

// ---- the walker: one expression, scanned (targets only) or rendered ----

struct Walker {
    Ctx&           c;
    const uint8_t* s;
    size_t         n;
    size_t         pos = 0;
    bool           render = false;
    std::string*   out = nullptr;
    std::vector<uint32_t>* targets = nullptr;
    // the call expressions met, pre-order (a nested call after the call around it); null = not collected
    std::vector<script_calls::CallSite>* calls = nullptr;
    // label of a code offset (render mode); null = scan mode
    const std::unordered_map<uint32_t, int>* labels = nullptr;
    uint32_t (*resolve)(const void*, uint32_t) = nullptr;
    const void* resolveCtx = nullptr;
    int depth = 0;  // expressions open around the one being read
    static constexpr int kMaxNesting = 256;
    // The call expressions being read, innermost last: the record's index in `calls` and the call's own
    // expression depth. Touched only when `calls` is collected.
    struct OpenCall {
        size_t site;
        int    depth;
    };
    std::vector<OpenCall> open;

    bool Need(size_t k) {
        if (pos + k > n) {
            c.Fail("script ends inside an expression", pos);
            return false;
        }
        return true;
    }
    template <typename T>
    bool Take(T& v) {
        if (!Need(sizeof(T))) return false;
        std::memcpy(&v, s + pos, sizeof(T));
        pos += sizeof(T);
        return true;
    }
    void Emit(const std::string& t) {
        if (render) *out += t;
    }
    void Emit(const char* t) {
        if (render) *out += t;
    }
    void Hex32(const char* pre, uint32_t v) {
        if (!render) return;
        char b[24];
        std::snprintf(b, sizeof b, "%s%08x", pre, v);
        *out += b;
    }

    bool Operand(Kind k, uint8_t listEnd, bool& wrote) {
        auto sep = [&]() {
            if (wrote) Emit(",");
            wrote = true;
        };
        switch (k) {
        case kExpr: sep(); return Expr();
        case kList: {
            sep();
            Emit("[");
            for (bool firstItem = true;;) {
                if (!Need(1)) return false;
                if (s[pos] == listEnd) {
                    ++pos;
                    break;
                }
                if (!firstItem) Emit(",");
                firstItem = false;
                if (!Expr()) return false;
            }
            Emit("]");
            return true;
        }
        case kObj: {
            void* o = nullptr;
            if (!Take(o)) return false;
            sep();
            if (render) Emit(o ? "o:" + ObjPath(c, o) : std::string("o:-"));
            return true;
        }
        case kProp: {
            void* f = nullptr;
            if (!Take(f)) return false;
            sep();
            if (render) Emit(PropText(c, f));
            return true;
        }
        case kName: {
            int32_t ci = 0, di = 0;
            uint32_t num = 0;
            if (!Take(ci) || !Take(di) || !Take(num)) return false;
            sep();
            if (render) Emit("n:" + c.names->Of(ci, num));
            return true;
        }
        case kI32: { int32_t v; if (!Take(v)) return false; sep(); if (render) Emit("i:" + std::to_string(v)); return true; }
        case kU8: { uint8_t v; if (!Take(v)) return false; sep(); if (render) Emit("b:" + std::to_string(v)); return true; }
        case kU16: { uint16_t v; if (!Take(v)) return false; sep(); if (render) Emit("h:" + std::to_string(v)); return true; }
        case kBool: { uint8_t v; if (!Take(v)) return false; sep(); Emit(v ? "B:1" : "B:0"); return true; }
        case kI64: { int64_t v; if (!Take(v)) return false; sep(); if (render) Emit("l:" + std::to_string(v)); return true; }
        case kU64: { uint64_t v; if (!Take(v)) return false; sep(); if (render) Emit("u:" + std::to_string(v)); return true; }
        case kF32: { uint32_t v; if (!Take(v)) return false; sep(); Hex32("f:", v); return true; }
        case kF64: {
            uint64_t v;
            if (!Take(v)) return false;
            sep();
            if (render) {
                char b[32];
                std::snprintf(b, sizeof b, "d:%016llx", static_cast<unsigned long long>(v));
                Emit(b);
            }
            return true;
        }
        case kCode: {
            uint32_t v;
            if (!Take(v)) return false;
            sep();
            if (targets) targets->push_back(v);
            if (render) {
                const uint32_t r = resolve ? resolve(resolveCtx, v) : v;
                const auto it = labels ? labels->find(r) : decltype(labels->find(r)){};
                Emit(labels && it != labels->end() ? "@" + std::to_string(it->second) : std::string("@?"));
            }
            return true;
        }
        case kSkip:
        case kCount: { uint32_t v; return Take(v); }  // derived from the layout: dropped
        case kStr: {
            sep();
            Emit("s:\"");
            for (;;) {
                uint8_t ch;
                if (!Take(ch)) return false;
                if (!ch) break;
                if (render) AppendFolded(*out, ch, false);
            }
            Emit("\"");
            return true;
        }
        case kWStr: {
            sep();
            Emit("s:\"");
            for (;;) {
                uint16_t ch;
                if (!Take(ch)) return false;
                if (!ch) break;
                if (render) AppendFolded(*out, ch, false);
            }
            Emit("\"");
            return true;
        }
        default: return true;
        }
    }

    // The record of the call opcode just read at `at`, reserved before its operands so a call nested in
    // them lists after it; the callee is its first operand (a pointer, or an FName's index and number).
    size_t BeginCall(uint8_t code, size_t at) {
        script_calls::CallSite site{code, nullptr, 0, 0, static_cast<int32_t>(at), 0, {}, 0, false};
        if (IsObjectCallOp(code)) {
            if (pos + sizeof(void*) <= n) std::memcpy(&site.callee, s + pos, sizeof(void*));
        } else if (pos + 12 <= n) {
            std::memcpy(&site.nameComparisonIndex, s + pos, 4);
            std::memcpy(&site.nameNumber, s + pos + 8, 4);
        }
        calls->push_back(site);
        open.push_back({calls->size() - 1, depth});
        return calls->size() - 1;
    }

    // A local read as an operand of the innermost open call, when this expression is one of that call's
    // own parameters: one level inside the call's expression, not inside a parameter's.
    void NoteLocal(uint8_t code) {
        if (open.empty() || depth != open.back().depth + 1) return;
        if (code != EX_LocalVariable && code != EX_LocalOutVariable) return;
        if (pos + sizeof(void*) > n) return;
        script_calls::CallSite& site = (*calls)[open.back().site];
        if (site.localCount < std::size(site.locals)) {
            std::memcpy(&site.locals[site.localCount++], s + pos, sizeof(void*));
            return;
        }
        site.localsTruncated = true;
        static std::atomic<bool> said{false};
        if (!said.exchange(true, std::memory_order_relaxed))
            UE_LOGI("script_calls: a call takes more than %zu locals; the first %zu are kept", std::size(site.locals),
                    std::size(site.locals));
    }

    bool Expr() {
        if (!Need(1)) return false;
        // A compiled Blueprint nests a few dozen deep; a script deeper than this is corrupt, and the
        // recursion must not run the stack out reading it.
        if (depth >= kMaxNesting) {
            c.Fail("expressions nest too deep", pos);
            return false;
        }
        struct Nest {
            int& d;
            explicit Nest(int& x) : d(x) { ++d; }
            ~Nest() { --d; }
        } nest(depth);
        const size_t at = pos;
        const uint8_t code = s[pos++];
        const Op& op = Table()[code];
        if (!op.name) {
            c.Fail("unknown opcode", at);
            return false;
        }
        Emit(op.name);
        switch (op.special) {
        case sInstrumentation: {
            // the event type, and for an inline event (4) the event's name (UAssetAPI's EX_InstrumentationEvent)
            uint8_t type;
            if (!Take(type)) return false;
            Emit("(b:" + std::to_string(type));
            if (type == 4) {
                int32_t ci = 0, di = 0;
                uint32_t num = 0;
                if (!Take(ci) || !Take(di) || !Take(num)) return false;
                if (render) Emit(",n:" + c.names->Of(ci, num));
            }
            Emit(")");
            return true;
        }
        case sFloats3:
        case sTransform: {
            const int count = op.special == sTransform ? 10 : 3;
            Emit("(");
            for (int i = 0; i < count; ++i) {
                uint32_t v;
                if (!Take(v)) return false;
                if (i) Emit(",");
                Hex32("f:", v);
            }
            Emit(")");
            return true;
        }
        case sText: {
            uint8_t kind;
            if (!Take(kind)) return false;
            static const char* kKinds[] = {"Empty", "LocalizedText", "InvariantText", "LiteralString", "StringTableEntry"};
            if (kind > 4) {
                c.Fail("unknown text literal type", pos - 1);
                return false;
            }
            Emit("(t:");
            Emit(kKinds[kind]);
            const int subs = kind == 1 ? 3 : (kind == 2 || kind == 3) ? 1 : kind == 4 ? 2 : 0;
            if (kind == 4) {
                void* o = nullptr;
                if (!Take(o)) return false;
                if (render) Emit(o ? ",o:" + ObjPath(c, o) : std::string(",o:-"));
            }
            for (int i = 0; i < subs; ++i) {
                Emit(",");
                if (!Expr()) return false;
            }
            Emit(")");
            return true;
        }
        case sSwitch: {
            uint16_t cases;
            uint32_t endGoto;
            if (!Take(cases) || !Take(endGoto)) return false;
            Emit("(");
            if (!Expr()) return false;
            Emit(",[");
            for (uint16_t i = 0; i < cases; ++i) {
                if (i) Emit(",");
                Emit("(");
                if (!Expr()) return false;
                uint32_t next;
                if (!Take(next)) return false;
                Emit(",");
                if (!Expr()) return false;
                Emit(")");
            }
            Emit("],");
            if (!Expr()) return false;
            Emit(")");
            return true;
        }
        case sCast: {
            uint8_t ct;
            if (!Take(ct)) return false;
            const char* name = ct == 0x46 ? "ObjectToInterface" : ct == 0x47 ? "ObjectToBool" : ct == 0x49 ? "InterfaceToBool" : nullptr;
            Emit("(k:");
            if (name) {
                Emit(name);
            } else if (render) {
                Emit("?" + std::to_string(ct));
            }
            if (ct == 0x46) {
                void* o = nullptr;
                if (!Take(o)) return false;
                if (render) Emit(o ? ",o:" + ObjPath(c, o) : std::string(",o:-"));
            }
            Emit(",");
            if (!Expr()) return false;
            Emit(")");
            return true;
        }
        default: break;
        }
        if (op.ops[0] == kNone) return true;  // no operand: the name alone
        if (calls) NoteLocal(code);
        const bool listed = calls && IsCallOp(code);
        const size_t site = listed ? BeginCall(code, at) : 0;
        Emit("(");
        bool wrote = false;
        for (Kind k : op.ops) {
            if (k == kNone) break;
            if (!Operand(k, op.listEnd, wrote)) return false;
        }
        Emit(")");
        if (listed) {
            (*calls)[site].end = static_cast<int32_t>(pos);
            open.pop_back();
        }
        return true;
    }
};

}  // namespace

bool IsUbergraph(Ctx& c, void* fn) {
    if (!fn) return false;
    const R::FName& n = R::NameOf(fn);
    return c.names->Of(n.ComparisonIndex, 0).rfind("executeubergraph_", 0) == 0;
}

bool Scan(Ctx& c, const uint8_t* s, size_t n, std::vector<Stmt>& out, bool wantCalls) {
    size_t pos = 0;
    while (pos < n) {
        Stmt st;
        st.off = static_cast<uint32_t>(pos);
        st.op = s[pos];
        Walker w{c, s, n};
        w.pos = pos;
        w.targets = &st.targets;
        w.calls = wantCalls ? &st.calls : nullptr;
        if (!w.Expr()) return false;
        st.end = static_cast<uint32_t>(w.pos);
        // an event stub's call into its ubergraph: (Local)FinalFunction(ExecuteUbergraph_*, IntConst(entry), ...)
        if ((st.op == EX_LocalFinalFunction || st.op == EX_FinalFunction) && st.end - st.off >= 14) {
            void* fn = Rd<void*>(s, pos + 1);
            if (s[pos + 9] == EX_IntConst && IsUbergraph(c, fn)) {
                st.uberFn = fn;
                st.uberEntry = Rd<int32_t>(s, pos + 10);
            }
        }
        pos = w.pos;
        out.push_back(std::move(st));
    }
    return true;
}

size_t RenderExpr(Ctx& c, const uint8_t* s, size_t n, size_t pos, const Labels& labels, std::string& out) {
    Walker w{c, s, n};
    w.pos = pos;
    w.render = true;
    w.out = &out;
    w.labels = labels.map;
    w.resolve = labels.resolve;
    w.resolveCtx = labels.ctx;
    return w.Expr() ? w.pos : 0;
}

const char* OpName(uint8_t op) {
    const char* name = Table()[op].name;
    return name ? name : "?";
}

}  // namespace ue_wrap::script_fingerprint::detail
