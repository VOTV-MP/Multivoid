; loader/cppmod_stubs.asm -- the no-op vtable for the UE4SS C-ABI dummy object.
;
; 256 identical-shape stubs at a uniform stride (begin/end bracket them; the
; C side derives stride = span/256 instead of assuming encoding widths). Each
; stub, in order:
;   1. lock-inc its slot counter (telemetry: WHICH slots each UE4SS era
;      dispatches -- tripwire wire-e's runtime evidence);
;   2. on the slot's FIRST call only (lock bts bit), call the C reporter so the
;      attribution line is flushed to disk immediately -- a crash milliseconds
;      later still has its evidence (spike audit round 9);
;   3. zero RAX and XMM0, so a future scalar-returning virtual reads a
;      deterministic 0 / 0.0 / false / nullptr. Every virtual the host can
;      fire today returns VOID in all three live eras (measured: v3.0.1,
;      e31aaaa6, main-7f7cc36 headers); an sret aggregate return has NO
;      universal safe stub and stays a WATCHED coupling (wire-e), not a
;      solved one.
; The stubs never touch their arguments: x64 is caller-cleanup, so a plain ret
; is safe for ANY signature with a non-sret return.

EXTERN MultivoidCppmodSlotFirstHit:PROC

PUBLIC multivoid_cppmod_stubs_begin
PUBLIC multivoid_cppmod_stubs_end
PUBLIC multivoid_cppmod_slot_counters

.DATA
ALIGN 8
multivoid_cppmod_slot_counters QWORD 256 DUP (0)
multivoid_cppmod_slot_reported QWORD 4 DUP (0)

.CODE

ALIGN 16
multivoid_cppmod_stubs_begin LABEL BYTE

slotidx = 0
REPT 256
    ALIGN 16
    lock inc QWORD PTR [multivoid_cppmod_slot_counters + slotidx*8]
    lock bts QWORD PTR [multivoid_cppmod_slot_reported + (slotidx SHR 6)*8], (slotidx AND 63)
    jc SHORT @F
    ; First call on this slot: report it. Entry RSP is 16k+8 (post-call);
    ; 40 = 32 shadow + 8 realign so the CALL site sits at RSP%16==0.
    sub rsp, 40
    mov ecx, slotidx
    call MultivoidCppmodSlotFirstHit
    add rsp, 40
@@:
    xor eax, eax
    xorps xmm0, xmm0
    ret
    slotidx = slotidx + 1
ENDM

ALIGN 16
multivoid_cppmod_stubs_end LABEL BYTE

END
