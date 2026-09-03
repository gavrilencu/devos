; =============================================================================
; MyOS — program user demo (ruleaza in ring 3)
; Comunica cu kernelul EXCLUSIV prin syscalls (int 0x80):
;   RAX=0 write(RDI=ptr, RSI=len), RAX=1 exit(), RAX=3 sleep(RDI=ms)
; Asamblat ca binar flat, incarcat de kernel la 0x8000000000 (512 GiB),
; in spatiul de adrese propriu al procesului.
; =============================================================================

[bits 64]
[org 0x8000000000]

_start:
    mov rax, 0                  ; write
    mov rdi, msg_salut
    mov rsi, msg_salut_len
    int 0x80

    mov rbx, 3
.loop:
    mov rax, 0
    mov rdi, msg_lucru
    mov rsi, msg_lucru_len
    int 0x80

    mov rax, 3                  ; sleep
    mov rdi, 400                ; 400 ms
    int 0x80

    dec rbx
    jnz .loop

    mov rax, 0
    mov rdi, msg_gata
    mov rsi, msg_gata_len
    int 0x80

    mov rax, 1                  ; exit
    int 0x80

msg_salut:     db "  [user] Salut din ring 3! Nu pot atinge kernelul.", 10
msg_salut_len  equ $ - msg_salut
msg_lucru:     db "  [user] lucrez (sleep 400 ms)...", 10
msg_lucru_len  equ $ - msg_lucru
msg_gata:      db "  [user] am terminat, exit()", 10
msg_gata_len   equ $ - msg_gata
