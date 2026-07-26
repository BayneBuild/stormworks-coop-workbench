; Capture WHICH interact key the player used (E vs Q), so entering a workbench can mean CO-OP or SOLO.
; Hook on the player input-action handler (base+0x789070). We are entered by a 14-byte abs JMP placed over
; the prologue, so RSP is exactly as the caller left it and the stack args are still in place:
;     [rsp+0x30] = arg6 = action id   (0x13 = interact-left / Q, 0x14 = interact-right / E)
;     [rsp+0x38] = arg7 = key state   (1 = pressed, 0 = released)
; The game itself does NOT distinguish the two at a workbench (both send the same use-workbench request),
; so this is purely observation - we never change what the game does, we only record the key so the mod can
; decide whether to sync. 15-byte prologue steal. Clobbers only rax (caller-save at function entry).
extern g_interact_action:QWORD
extern g_interact_state:QWORD
extern g_interact_seen:QWORD
extern g_tramp_interact:QWORD

.code
DetourInteract PROC
    ; Both are 32-bit ints in an 8-byte stack slot - reading the full qword picks up whatever the caller
    ; left in the upper half (observed: 0xB2_00000014). `mov eax` zero-extends into rax, giving a clean value.
    mov     eax, dword ptr [rsp+30h]         ; action id  (0x13 = Q, 0x14 = E)
    mov     qword ptr g_interact_action, rax
    mov     eax, dword ptr [rsp+38h]         ; key state  (1 = pressed)
    mov     qword ptr g_interact_state, rax
    inc     qword ptr g_interact_seen        ; bump so the C side can tell "fresh" from "stale"
    mov     rax, qword ptr g_tramp_interact
    jmp     rax
DetourInteract ENDP
end
