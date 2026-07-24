; DETECT a new connection (wire). Hook on connlist_push_back 0x8B70C0 — the leaf mutator that appends
; one logic_node_link to the connection deque at vehicle+0x50 (vehicle=*(editor+0x13C8)).
; At entry rcx = &deque. The record is EMPTY here; the caller fills voxelA/voxelB/type right after
; push_back returns — so we just capture the deque ptr + flag, and read the (now-filled) TAIL element
; a frame later on the main thread. Gated by g_suppress so our own forged adds don't re-broadcast.
; Prologue steal = 16 bytes (push rbx | sub rsp,20 | mov rbx,rcx | mov ecx,[rcx+10] | mov r8d,[rbx+8]);
; that prologue clobbers ecx, so we capture rcx BEFORE the trampoline runs. Clobbers only rax.
extern g_conn_deque:QWORD
extern g_conn_add_flag:QWORD
extern g_tramp_conn_add:QWORD
extern g_suppress:QWORD

.code
DetourConnAdd PROC
    cmp     qword ptr g_suppress, 0
    jne     passthru
    mov     qword ptr g_conn_deque, rcx      ; &deque (== vehicle+0x50)
    mov     qword ptr g_conn_add_flag, 1
passthru:
    mov     rax, qword ptr g_tramp_conn_add
    jmp     rax
DetourConnAdd ENDP
end
