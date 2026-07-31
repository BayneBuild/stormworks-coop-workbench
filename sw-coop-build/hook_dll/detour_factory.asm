; AUTO-ARM capture on the component factory 0x45EB50.
; rdx = the placement struct: BOTH single-click (0x7F7EB0) and click-drag (0x7F3440) funnel through
; here, passing the struct as rdx. We copy 0x80 bytes so a drag-first session can arm the forge path
; (the struct the forge copies and overwrites the voxel of). Active only while g_da_arm_needed != 0;
; gated by g_suppress so our own applied forges never trigger it.
; 15-byte prologue steal (mov [rsp+0x10],rbx + 5 pushes + mov rbp,rsp). rdx untouched by that prologue.
; Clobbers rax/r10/r11; preserves rcx/rdx/r8/r9 and the pushed callee-saved regs.
extern g_da_struct:QWORD     ; 0x80-byte buffer
extern g_da_flag:QWORD
extern g_da_ptr:QWORD        ; rdx itself - we need the ADDRESS to tell a stack local from an editor field
extern g_da_arm_needed:QWORD
extern g_tramp_factory:QWORD
extern g_suppress:QWORD

COPYF MACRO n
    mov     r11, [r10 + n]
    mov     [rax + n], r11
ENDM

.code
DetourFactory PROC
    cmp     qword ptr g_suppress, 0
    jne     passthru
    cmp     qword ptr g_da_arm_needed, 0
    je      passthru
    test    rdx, rdx
    jz      passthru
    lea     rax, g_da_struct
    mov     r10, rdx
    COPYF 0
    COPYF 8
    COPYF 16
    COPYF 24
    COPYF 32
    COPYF 40
    COPYF 48
    COPYF 56
    COPYF 64
    COPYF 72
    COPYF 80
    COPYF 88
    COPYF 96
    COPYF 104
    COPYF 112
    COPYF 120
    mov     qword ptr g_da_ptr, rdx
    mov     qword ptr g_da_flag, 1
passthru:
    mov     rax, qword ptr g_tramp_factory
    jmp     rax
DetourFactory ENDP
end
