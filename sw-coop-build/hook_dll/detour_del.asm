; Detection detour for the universal component-remove funnel 0x4C0940.
; A click-DRAG eraser removes many components in a rapid BURST — a single shared slot loses all but
; the last, so we RING-BUFFER the removed voxels (256 slots x {int x,y,z}). The removed component is
; arg5, on the stack at [rsp+0x28] at raw entry. We read the voxel SYNCHRONOUSLY (comp+0x18/1c/20)
; here, before the component is freed. Gated by g_suppress so our own applied (remote) deletes — which
; re-enter 0x4C0940 — are never re-broadcast.
; Clobbers rax/r10/r11 (rax re-set by the stolen prologue 'mov rax,rsp'); does NOT touch rsp.
extern g_delring:DWORD        ; base of 256 * {int x,y,z} = 256*3 int32
extern g_delring_wr:QWORD
extern g_tramp_del:QWORD
extern g_suppress:QWORD

.code
DetourDel PROC
    cmp     qword ptr g_suppress, 0
    jne     passthru
    mov     rax, [rsp+28h]              ; arg5 = component being removed
    test    rax, rax
    jz      passthru
    ; slot = &g_delring + (g_delring_wr & 0xFF) * 12
    mov     r10, qword ptr g_delring_wr
    and     r10, 0FFh                   ; idx (256 slots)
    lea     r11, [r10 + r10*2]          ; idx*3
    lea     r10, g_delring              ; base
    lea     r11, [r10 + r11*4]          ; base + idx*12 = slot
    ; read voxel comp+0x18/1c/20 synchronously into the slot
    mov     r10d, dword ptr [rax+18h]
    mov     dword ptr [r11], r10d
    mov     r10d, dword ptr [rax+1Ch]
    mov     dword ptr [r11+4], r10d
    mov     r10d, dword ptr [rax+20h]
    mov     dword ptr [r11+8], r10d
    inc     qword ptr g_delring_wr      ; publish (data stores ordered before this on x86)
passthru:
    mov     rax, qword ptr g_tramp_del
    jmp     rax
DetourDel ENDP
end
