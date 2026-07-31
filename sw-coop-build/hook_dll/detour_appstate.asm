; AUTO-ARM passive capture on c_application_state_game::update (0x847EE0, rcx = the app-state object).
; Stash the app-state pointer + bump a seen-counter EVERY frame so my_runcb can passively resolve the editor
; and arm with NO local edit. 18-byte prologue steal (8 pushes + lea rbp,[rsp-1Fh]). Clobbers only rax
; (caller-save at function entry); preserves everything else. Not gated - capture unconditionally.
; R8 is ALSO captured: it is the object that owns the connected-player roster (ring at +0x410, each entry a
; 0x1A8-byte c_player with its SteamID64 at +0x140). Found by reading the native behind the Lua
; server.getPlayers - we had been discarding this register.
extern g_cap_appstate:QWORD
extern g_cap_game:QWORD
extern g_cap_seen:QWORD
extern g_tramp_appstate:QWORD

.code
DetourAppState PROC
    mov     qword ptr g_cap_appstate, rcx
    mov     qword ptr g_cap_game, r8
    inc     qword ptr g_cap_seen
    mov     rax, qword ptr g_tramp_appstate
    jmp     rax
DetourAppState ENDP
end
