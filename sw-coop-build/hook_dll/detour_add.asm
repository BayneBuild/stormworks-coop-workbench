; Detection detour for the universal "register one placed component" routine 0x4BFE50.
; BOTH single-click (0x7F7EB0) and click-DRAG (0x7F3440) funnel through here, once per placed
; block, with a fully-populated component struct in r8 (arg3):
;   voxel x/y/z @ r8+0x18/+0x1c/+0x20, rotation 3x3 @ r8+0x30..0x50, template ptr @ r8+0x58.
; A drag commits its whole line in a BURST of rapid calls, so a single shared slot loses all
; but the last. Instead we enqueue each call into a lock-free SPSC RING (1024 slots x 0x80):
; producer (this detour, game thread) writes slot then bumps g_ring_wr; consumer (worker) drains
; g_ring_rd..g_ring_wr. x86-TSO keeps the data store ordered before the index bump - no fence.
; Gated by g_suppress so our own applied forges (re-enter via 0x7F7EB0) are never enqueued.
; Clobbers only rax/r10/r11; preserves rcx/rdx/r8/r9; does NOT touch rsp (prologue splice-safe).
extern g_ring_wr:QWORD
extern g_ring:QWORD          ; base of the ring byte array (1024 * 0x80 bytes)
extern g_ring_ptr:QWORD      ; parallel array of the component pointers (1024 qwords)
extern g_tramp_add:QWORD
extern g_suppress:QWORD

COPYA MACRO n
    mov     r11, [r10 + n]
    mov     [rax + n], r11
ENDM

.code
DetourAdd PROC
    cmp     qword ptr g_suppress, 0
    jne     passthru
    test    r8, r8
    jz      passthru
    ; dest = &g_ring + (g_ring_wr & 1023) * 0x80
    mov     rax, qword ptr g_ring_wr
    and     rax, 3FFh
    shl     rax, 7
    lea     r10, g_ring
    add     rax, r10
    ; copy 0x80 bytes from [r8] into the slot
    mov     r10, r8
    COPYA 0
    COPYA 8
    COPYA 16
    COPYA 24
    COPYA 32
    COPYA 40
    COPYA 48
    COPYA 56
    COPYA 64
    COPYA 72
    COPYA 80
    COPYA 88
    COPYA 96
    COPYA 104
    COPYA 112
    COPYA 120
    ; store the component pointer (r10==r8) for a deferred live re-read
    mov     r11, qword ptr g_ring_wr
    and     r11, 3FFh
    lea     rax, g_ring_ptr
    mov     [rax + r11*8], r10
    ; publish (data stores above are ordered before this on x86)
    inc     qword ptr g_ring_wr
passthru:
    mov     rax, qword ptr g_tramp_add
    jmp     rax
DetourAdd ENDP
end
