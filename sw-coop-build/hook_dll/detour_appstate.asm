; AUTO-ARM passive capture on c_application_state_game::update (0x847EE0, rcx = the app-state object).
; Stash the app-state pointer + bump a seen-counter EVERY frame so my_runcb can passively resolve the editor
; and arm with NO local edit. 18-byte prologue steal (8 pushes + lea rbp,[rsp-1Fh]). Clobbers only rax
; (caller-save at function entry); preserves everything else. Not gated - capture unconditionally.
extern g_cap_appstate:QWORD
extern g_cap_seen:QWORD
extern g_tramp_appstate:QWORD

.code
DetourAppState PROC
    mov     qword ptr g_cap_appstate, rcx
    inc     qword ptr g_cap_seen
    mov     rax, qword ptr g_tramp_appstate
    jmp     rax
DetourAppState ENDP
end
