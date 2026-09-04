; =============================================================================
; MyOS — stage2
; Incarcat de stage1 la 0x8000, inca in real mode (16-bit).
; Rol: A20 -> GDT -> protected mode (32-bit) -> tabele de paginare ->
;      long mode (64-bit) -> salt in kernel la 0x9000.
;
; Harta memoriei folosita aici:
;   0x1000 : PML4   (nivelul 4 de paginare)
;   0x2000 : PDPT   (nivelul 3)
;   0x3000 : PD     (nivelul 2, 512 intrari x pagini de 2 MiB = 1 GiB identity map)
;   0x5000 : numarul de intrari E820 (dword), tabelul de la 0x5008 (24 B/intrare)
;   0x7C00 : stiva temporara (creste in jos)
;   0x8000 : acest cod
;   0x9000 : kernelul
; =============================================================================

[org 0x8000]
[bits 16]

KERNEL_ADDR equ 0x9000

E820_COUNT  equ 0x5000
E820_TABLE  equ 0x5008
E820_MAX    equ 64

; Informatiile despre modul grafic, pentru kernel (vezi kernel/fb.c):
;   +0 w (word), +2 h (word), +4 pitch (word), +6 bpp (byte),
;   +7 activ (byte), +8 adresa fizica a framebuffer-ului (dword)
VBE_SAVE    equ 0x5F00
VBE_CTRL    equ 0x7000          ; buffer info controller (512 B)
VBE_MODE    equ 0x7200          ; buffer info mod (256 B)
FONT_ADDR   equ 0x6000          ; fontul 8x16 al BIOS-ului, copiat aici (4 KiB)
GFX_W       equ 1024        ; mod VBE standard folosit doar ca sa aflam LFB
GFX_H       equ 768
PREF_W      equ 1920        ; rezolutia dorita (Full HD), setata prin dispi
PREF_H      equ 1080

CODE32_SEL  equ 0x08
DATA_SEL    equ 0x10
CODE64_SEL  equ 0x18

stage2:
    mov si, msg_stage2
    call print16

    ; --- Harta memoriei (int 15h, EAX=E820) — posibila doar in real mode.
    ; Kernelul o va citi de la E820_COUNT / E820_TABLE.
    xor ax, ax
    mov es, ax
    mov dword [E820_COUNT], 0
    xor ebx, ebx                ; cookie de continuare, 0 la primul apel
    mov di, E820_TABLE
.e820_loop:
    cmp dword [E820_COUNT], E820_MAX
    jae .e820_done
    mov eax, 0xE820
    mov ecx, 24
    mov edx, 0x534D4150         ; semnatura 'SMAP'
    mov dword [es:di+20], 1     ; unele BIOS-uri intorc 20 de bytes; preumplem atributul
    int 0x15
    jc .e820_done               ; CF la primul apel = E820 nesuportat; apoi = gata
    cmp eax, 0x534D4150
    jne .e820_done
    inc dword [E820_COUNT]
    add di, 24
    test ebx, ebx               ; EBX=0 inseamna ultima intrare
    jnz .e820_loop
.e820_done:

    ; --- Copiem fontul text 8x16 al BIOS-ului la FONT_ADDR: in modul
    ; grafic nu mai exista font hardware, desenam noi caracterele.
    push ds
    mov ax, 0x1130
    mov bh, 0x06
    int 0x10                    ; ES:BP = fontul 8x16 (256 caractere)
    mov ax, es
    mov ds, ax
    mov si, bp
    xor ax, ax
    mov es, ax
    mov di, FONT_ADDR
    mov cx, 4096 / 2
    rep movsw
    pop ds

    ; --- Mod grafic VESA: cautam in lista de moduri unul de
    ; GFX_W x GFX_H cu 32 bpp si linear framebuffer.
    mov byte [VBE_SAVE+7], 0    ; implicit: fara mod grafic (fallback text)

    mov ax, 0x4F00              ; informatii controller VBE
    mov di, VBE_CTRL
    mov dword [di], 'VBE2'
    int 0x10
    cmp ax, 0x004F
    jne .vbe_done

    mov si, [VBE_CTRL+14]       ; pointerul (off:seg) la lista de moduri
    mov ax, [VBE_CTRL+16]
    mov fs, ax
.vbe_next:
    mov cx, [fs:si]
    add si, 2
    cmp cx, 0xFFFF
    je .vbe_done                ; lista epuizata: raman pe text

    mov ax, 0x4F01              ; informatii despre modul CX
    mov di, VBE_MODE
    int 0x10
    cmp ax, 0x004F
    jne .vbe_next
    cmp byte [VBE_MODE+0x19], 32        ; 32 bpp
    jne .vbe_next
    cmp word [VBE_MODE+0x12], GFX_W
    jne .vbe_next
    cmp word [VBE_MODE+0x14], GFX_H
    jne .vbe_next
    test byte [VBE_MODE], 0x80          ; suporta linear framebuffer
    jz .vbe_next

    mov bx, cx
    or bx, 0x4000               ; bitul de LFB la setarea modului
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .vbe_done

    mov ax, [VBE_MODE+0x12]     ; salvam parametrii pentru kernel
    mov [VBE_SAVE+0], ax
    mov ax, [VBE_MODE+0x14]
    mov [VBE_SAVE+2], ax
    mov ax, [VBE_MODE+0x10]     ; pitch (bytes pe linie)
    mov [VBE_SAVE+4], ax
    mov byte [VBE_SAVE+6], 32
    mov eax, [VBE_MODE+0x28]    ; adresa fizica a framebuffer-ului
    mov [VBE_SAVE+8], eax
    mov byte [VBE_SAVE+7], 1
.vbe_done:

    ; --- Full HD prin interfata Bochs VBE dispi (QEMU/Bochs stdvga).
    ; Am deja adresa LFB din modul VBE de mai sus; dispi reprogrameaza doar
    ; rezolutia (aceeasi adresa fizica de framebuffer). Daca dispi lipseste
    ; sau nu accepta, ramanem pe modul VBE deja setat.
    cmp byte [VBE_SAVE+7], 1
    jne .dispi_done
    mov dx, 0x01CE                  ; index ID
    xor ax, ax
    out dx, ax
    mov dx, 0x01CF
    in ax, dx
    cmp ax, 0xB0C0                  ; semnatura dispi (0xB0C0..0xB0C5)
    jb .dispi_done
    cmp ax, 0xB0C5
    ja .dispi_done
    call dispi_set                  ; incearca PREF_W x PREF_H
    mov dx, 0x01CE                  ; recitim XRES ca sa confirmam
    mov ax, 1
    out dx, ax
    mov dx, 0x01CF
    in ax, dx
    cmp ax, PREF_W
    jne .dispi_done
    mov word [VBE_SAVE+0], PREF_W   ; parametrii noi pentru kernel
    mov word [VBE_SAVE+2], PREF_H
    mov word [VBE_SAVE+4], PREF_W*4 ; pitch = latime * 4 (32bpp)
.dispi_done:

    ; Activam linia A20 (metoda "fast A20", portul 0x92).
    ; Grija la bitul 0: scris cu 1 ar reseta procesorul.
    in al, 0x92
    or al, 0x02
    and al, 0xFE
    out 0x92, al

    cli                         ; gata cu intreruperile pana avem IDT propriu
    lgdt [gdt.descriptor]

    mov eax, cr0
    or eax, 1                   ; CR0.PE = protected mode
    mov cr0, eax

    jmp CODE32_SEL:pm_entry     ; far jump ca sa incarcam CS nou

print16:
    mov ah, 0x0E
.loop:
    lodsb
    test al, al
    jz .done
    int 0x10
    jmp .loop
.done:
    ret

; Scrie un registru dispi: BX=index, CX=valoare
dispi_reg:
    mov dx, 0x01CE
    mov ax, bx
    out dx, ax
    mov dx, 0x01CF
    mov ax, cx
    out dx, ax
    ret

; Seteaza modul dispi la PREF_W x PREF_H, 32bpp, LFB activ
dispi_set:
    mov bx, 4                   ; ENABLE = 0 (dezactiveaza)
    xor cx, cx
    call dispi_reg
    mov bx, 1                   ; XRES
    mov cx, PREF_W
    call dispi_reg
    mov bx, 2                   ; YRES
    mov cx, PREF_H
    call dispi_reg
    mov bx, 3                   ; BPP
    mov cx, 32
    call dispi_reg
    mov bx, 6                   ; VIRT_WIDTH = XRES (pitch = latime*4)
    mov cx, PREF_W
    call dispi_reg
    mov bx, 4                   ; ENABLE = ENABLED | LFB
    mov cx, 0x41
    call dispi_reg
    ret

msg_stage2: db "MyOS stage2: trec in long mode...", 13, 10, 0

; -----------------------------------------------------------------------------
[bits 32]
pm_entry:
    mov ax, DATA_SEL
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov esp, 0x7C00

    ; Curatam zona tabelelor de paginare (0x1000..0x3FFF)
    cld
    mov edi, 0x1000
    xor eax, eax
    mov ecx, 0x0C00             ; 3 pagini * 4096 / 4 bytes
    rep stosd

    ; PML4[0] -> PDPT, PDPT[0] -> PD  (flags: present | writable)
    mov dword [0x1000], 0x2000 | 0x03
    mov dword [0x2000], 0x3000 | 0x03

    ; PD: 512 pagini de 2 MiB => identity map pentru primul 1 GiB
    mov edi, 0x3000
    mov eax, 0x83               ; present | writable | page size (2 MiB)
    mov ecx, 512
.fill_pd:
    mov [edi], eax
    mov dword [edi+4], 0
    add eax, 0x200000
    add edi, 8
    loop .fill_pd

    mov eax, cr4
    or eax, 1 << 5              ; CR4.PAE — obligatoriu pentru long mode
    mov cr4, eax

    mov eax, 0x1000
    mov cr3, eax                ; radacina tabelelor de paginare

    mov ecx, 0xC0000080         ; MSR EFER
    rdmsr
    or eax, 1 << 8              ; EFER.LME = long mode enable
    wrmsr

    mov eax, cr0
    or eax, 1 << 31             ; CR0.PG = paginare activa => long mode activ
    mov cr0, eax

    jmp CODE64_SEL:lm_entry

; -----------------------------------------------------------------------------
[bits 64]
lm_entry:
    mov ax, DATA_SEL
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov rsp, 0x7C00
    jmp KERNEL_ADDR             ; kernelul isi seteaza singur stiva finala

; -----------------------------------------------------------------------------
align 8
gdt:
    dq 0                        ; 0x00: descriptor nul (obligatoriu)
    dq 0x00CF9A000000FFFF       ; 0x08: cod 32-bit (baza 0, limita 4 GiB)
    dq 0x00CF92000000FFFF       ; 0x10: date (baza 0, limita 4 GiB)
    dq 0x00AF9A000000FFFF       ; 0x18: cod 64-bit (bitul L setat)
.descriptor:
    dw .descriptor - gdt - 1
    dd gdt
