// ue_wrap/core/script_calls.h -- the calls inside a Blueprint function's bytecode, in the order the
// bytecode fingerprint walks it (script_fingerprint.h). A native post hook names the call that reached
// it by the call's END offset (ufunction_hook::CallerFrame::codeOffset); this lists a function's calls
// with their [start, end) offsets and the locals they take, so a consumer can pin one by its ordinal
// while the function's fingerprint hash matches, or by the local it writes. There is no
// script_calls.cpp: the three functions (EnumerateCalls, SameCallee, CallHasLocal) are defined in
// script_fingerprint.cpp, beside the walk and the decoder they share. Engine-wrapper layer
// (principle 7): no gameplay or network logic.

#pragma once

#include <cstdint>
#include <vector>

namespace ue_wrap::script_calls {

// One call expression. `op` is the call opcode: 0x1C EX_FinalFunction, 0x46 EX_LocalFinalFunction and
// 0x68 EX_CallMath name a UFunction (`callee`); 0x1B EX_VirtualFunction and 0x45 EX_LocalVirtualFunction
// name a function by its FName (`callee` null, the FName's comparison index and number). Offsets count
// from the start of the function's own Script data: `start` is the opcode's, `end` the first byte after
// the call's last operand, which is where a native post hook sees the frame's code pointer. `locals`
// holds the FProperty* of each EX_LocalVariable / EX_LocalOutVariable that is itself one of the call's
// parameters (not one nested inside a parameter's expression), in parameter order; `localCount` is how
// many it holds, the first six at most; `localsTruncated` is set when the call took more than six, so
// `locals` is then not the whole list.
struct CallSite {
    uint8_t  op;
    void*    callee;
    uint32_t nameComparisonIndex;
    uint32_t nameNumber;
    int32_t  start;
    int32_t  end;
    void*    locals[6];
    uint8_t  localCount;
    bool     localsTruncated;
};

// Every call expression of `ufunction`'s bytecode, the fingerprint's own order: a plain function in its
// flow order, an ubergraph by its event stubs' entries (name order) each in flow order with a statement
// already listed under an earlier entry skipped, so each call is listed once. A call nested in another's
// operands (an EX_Context's inner call, an argument that is itself a call) comes right after the call it
// is nested in. `out` is cleared first; false, and `out` empty, when the function does not decode. A
// native function, or one with no script, returns true with `out` empty, as the fingerprint treats it.
// Not cached: a call decodes the whole function, and for an ubergraph the whole class's script, so a
// consumer enumerates once per function, while that function's fingerprint hash holds, and keeps the
// result. Game thread.
bool EnumerateCalls(void* ufunction, std::vector<CallSite>& out);

// Does `site` call `ufunction`: the pointer for the opcodes that carry one, the FName's {comparison
// index, number} for the ones that carry a name.
bool SameCallee(const CallSite& site, void* ufunction);

// Does one of `site`'s recorded locals carry exactly the name `localName`: the whole name, its number
// included and case ignored, so "x_overlappingactors" does not match "x_overlappingactors_1" (the
// engine renders a name of number n+1 as "_n"). A site with `localsTruncated` whose stored locals do not
// hold the name answers false and logs once: the name may be among the dropped ones, so false there means
// "not known", never "absent". A CallSite's `locals` are property pointers read at EnumerateCalls and are
// valid only while the function's class stays loaded: a consumer calls CallHasLocal in the same resolve
// as EnumerateCalls and re-resolves when object_index::ClassUnlistVersion has moved. The read runs under
// a fault guard and answers false on a fault. So false covers absent, truncated and faulted alike (the
// two unusual ones log once): a consumer treats all three as "not this call". Game thread.
bool CallHasLocal(const CallSite& site, const wchar_t* localName);

}  // namespace ue_wrap::script_calls
