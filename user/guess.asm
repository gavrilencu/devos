; =============================================================================
; MyOS — "Ghiceste numarul": joc interactiv care ruleaza in ring 3.
; Foloseste syscalls (int 0x80):
;   0 write(rdi=ptr, rsi=len)   3 sleep(rdi=ms)
;   1 exit()                    4 readc() -> rax (sau -1)
;                               5 ticks() -> rax
; =============================================================================

[bits 64]
[org 0x8000000000]
default rel                     ; referintele la memorie devin RIP-relative —
                                ; obligatoriu la adrese care nu incap in disp32

%macro WRITE 2
    mov rax, 0
    mov rdi, %1
    mov rsi, %2
    int 0x80
%endmacro

_start:
    ; numarul secret = (ticks % 100) + 1
    mov rax, 5
    int 0x80
    xor rdx, rdx
    mov rcx, 100
    div rcx
    inc rdx
    mov r15, rdx                ; r15 = secretul
    xor r14, r14                ; r14 = numarul de incercari

    WRITE msg_intro, msg_intro_len

.round:
    inc r14
    WRITE msg_prompt, msg_prompt_len
    call read_number            ; -> rax
    cmp rax, r15
    je .win
    jb .too_low
    WRITE msg_high, msg_high_len
    jmp .round
.too_low:
    WRITE msg_low, msg_low_len
    jmp .round

.win:
    WRITE msg_win1, msg_win1_len
    mov rax, r14
    call print_number
    WRITE msg_win2, msg_win2_len
    mov rax, 1                  ; exit
    int 0x80

; -----------------------------------------------------------------------------
; read_number: citeste cifre de la tastatura (cu echo si backspace) pana la
; Enter; intoarce numarul in RAX.
read_number:
    xor rbx, rbx                ; acumulator
    xor r13, r13                ; cate cifre am
.loop:
    mov rax, 4                  ; readc
    int 0x80
    cmp rax, -1
    jne .have_char
    mov rax, 3                  ; nimic inca: sleep 20 ms si reincearca
    mov rdi, 20
    int 0x80
    jmp .loop
.have_char:
    cmp al, 10                  ; Enter
    je .done
    cmp al, 8                   ; Backspace
    je .backspace
    cmp al, '0'
    jb .loop
    cmp al, '9'
    ja .loop
    cmp r13, 3                  ; maxim 3 cifre
    jae .loop

    movzx rdx, al               ; acumulam cifra
    sub rdx, '0'
    imul rbx, rbx, 10
    add rbx, rdx
    inc r13

    mov [echo_ch], al           ; echo
    WRITE echo_ch, 1
    jmp .loop
.backspace:
    test r13, r13
    jz .loop
    dec r13
    mov rax, rbx                ; acumulator /= 10
    xor rdx, rdx
    mov rcx, 10
    div rcx
    mov rbx, rax
    mov byte [echo_ch], 8
    WRITE echo_ch, 1
    jmp .loop
.done:
    mov byte [echo_ch], 10
    WRITE echo_ch, 1
    mov rax, rbx
    ret

; -----------------------------------------------------------------------------
; print_number: afiseaza RAX in zecimal.
print_number:
    mov rbx, 10
    lea rdi, [numbuf_end]
    xor rcx, rcx
.digit:
    xor rdx, rdx
    div rbx
    add dl, '0'
    dec rdi
    mov [rdi], dl
    inc rcx
    test rax, rax
    jnz .digit
    mov rsi, rcx                ; write(rdi = primul digit, rsi = lungimea)
    mov rax, 0
    int 0x80
    ret

; -----------------------------------------------------------------------------
msg_intro:      db "Ghiceste numarul (1-100)! Scrie un numar si apasa Enter.", 10
msg_intro_len   equ $ - msg_intro
msg_prompt:     db "incercarea ta: "
msg_prompt_len  equ $ - msg_prompt
msg_low:        db "prea mic!", 10
msg_low_len     equ $ - msg_low
msg_high:       db "prea mare!", 10
msg_high_len    equ $ - msg_high
msg_win1:       db "CORECT! Ai ghicit din "
msg_win1_len    equ $ - msg_win1
msg_win2:       db " incercari. Bravo!", 10
msg_win2_len    equ $ - msg_win2

echo_ch:        db 0
numbuf:         times 24 db 0
numbuf_end:
