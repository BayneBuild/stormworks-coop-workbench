; Detection detour for the editor place-command 0x7F7EB0.
; Same capture as detour2 (args + 0x80 bytes of [r9] placement struct) BUT gated by
; g_suppress: while we are applying a REMOTE placement (our own forged place-cmd call),
; g_suppress != 0 -> we pass straight through without capturing, so we never re-broadcast
; our own applied blocks (echo suppression). Clobbers only rax/r10/r11; preserves rcx/rdx/r8/r9.
extern g_cap_rcx:QWORD
extern g_cap_rdx:QWORD
extern g_cap_r8:QWORD
extern g_cap_r9:QWORD
extern g_cap_arg5:QWORD
extern g_cap_flag:QWORD
extern g_tramp:QWORD
extern g_r9buf:QWORD
extern g_suppress:QWORD

COPYQ MACRO n
    mov     r11, [r10 + n]
    mov     [rax + n], r11
ENDM

.code
DetourDetect PROC
    cmp     qword ptr g_suppress, 0
    jne     passthru
    mov     g_cap_rcx, rcx
    mov     g_cap_rdx, rdx
    mov     g_cap_r8,  r8
    mov     g_cap_r9,  r9
    mov     r11, [rsp+28h]
    mov     g_cap_arg5, r11
    test    r9, r9
    jz      setflag
    lea     rax, g_r9buf
    mov     r10, r9
    COPYQ 0
    COPYQ 8
    COPYQ 16
    COPYQ 24
    COPYQ 32
    COPYQ 40
    COPYQ 48
    COPYQ 56
    COPYQ 64
    COPYQ 72
    COPYQ 80
    COPYQ 88
    COPYQ 96
    COPYQ 104
    COPYQ 112
    COPYQ 120
setflag:
    mov     qword ptr g_cap_flag, 1
passthru:
    mov     rax, qword ptr g_tramp
    jmp     rax
DetourDetect ENDP
end
