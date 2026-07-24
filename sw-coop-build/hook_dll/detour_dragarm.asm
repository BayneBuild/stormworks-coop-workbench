; AUTO-ARM capture on the DRAG / area-fill command 0x7F3440.
; rcx = the editor (the function itself tests [rcx+0xe7c], the current-tool field). We stash it so a
; click-DRAG can arm the apply path with no prior single-click. Active only while g_da_arm_needed != 0
; (i.e. until we're armed); gated by g_suppress so our own applied forges never trigger it.
; 15-byte prologue steal (mov rax,rsp + 8 pushes). Clobbers only rax; preserves everything else.
extern g_da_editor:QWORD
extern g_da_arm_needed:QWORD
extern g_tramp_dragarm:QWORD
extern g_suppress:QWORD

.code
DetourDragArm PROC
    cmp     qword ptr g_suppress, 0
    jne     passthru
    cmp     qword ptr g_da_arm_needed, 0
    je      passthru
    mov     qword ptr g_da_editor, rcx       ; capture the editor (direct store, no clobber)
passthru:
    mov     rax, qword ptr g_tramp_dragarm
    jmp     rax
DetourDragArm ENDP
end
