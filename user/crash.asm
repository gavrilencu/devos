; =============================================================================
; MyOS — program user care incalca intentionat protectia de memorie:
; incearca sa citeasca memoria kernelului (0x9000). Paginile kernelului nu au
; bitul U, deci CPU-ul ridica #PF, iar kernelul omoara task-ul — sistemul
; merge mai departe.
; =============================================================================

[bits 64]
[org 0x8000000000]

_start:
    mov rax, 0                  ; write
    mov rdi, msg
    mov rsi, msg_len
    int 0x80

    mov rax, [abs 0x9000]       ; citim kernelul -> #PF asteptat, aici murim

    mov rax, 1                  ; (nu se ajunge)
    int 0x80

msg:     db "  [user] incerc sa citesc memoria kernelului de la 0x9000...", 10
msg_len  equ $ - msg
