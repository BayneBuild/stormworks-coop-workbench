; Hook on the single-delete funnel 0x804300. Two jobs:
;  - GATE: set g_user_delete=1 so the universal remove hook 0x4C0940 only treats removes that
;    happen inside a user delete as real deletes (not craft-unload / internal body ops).
;  - ARM: capture {editor=rcx, rdx, r8} from the first real delete so we can forge deletes later.
; Gated by g_suppress (our own applied deletes must not re-arm/re-broadcast). Clobbers only rax
; (stolen prologue starts 'mov rax,rsp'); preserves rcx/rdx/r8; does NOT touch rsp.
extern g_delarm_flag:QWORD
extern g_delarm_editor:QWORD
extern g_delarm_rdx:QWORD
extern g_delarm_r8:QWORD
extern g_user_delete:QWORD
extern g_tramp_delarm:QWORD
extern g_suppress:QWORD

.code
DetourDelArm PROC
    cmp     qword ptr g_suppress, 0
    jne     passthru
    mov     qword ptr g_user_delete, 1
    cmp     qword ptr g_delarm_flag, 0
    jne     passthru
    mov     qword ptr g_delarm_flag, 1
    mov     g_delarm_editor, rcx
    mov     g_delarm_rdx, rdx
    mov     g_delarm_r8, r8
passthru:
    mov     rax, qword ptr g_tramp_delarm
    jmp     rax
DetourDelArm ENDP
end
