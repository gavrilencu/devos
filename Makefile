# MyOS — build in WSL: ruleaza `wsl make` din Windows sau `make` din WSL.

BUILD := build

CFLAGS := -m64 -ffreestanding -fno-pie -fno-pic -fno-stack-protector \
          -fno-asynchronous-unwind-tables -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
          -O2 -Wall -Wextra

# js.c foloseste double (numerele JS) -> are nevoie de SSE. Doar firul
# browserului ruleaza JS, iar restul kernelului e -mno-sse, deci registrele
# xmm nu sunt atinse de nimeni altcineva (nu trebuie salvate la context switch).
CFLAGS_SSE := $(filter-out -mno-mmx -mno-sse -mno-sse2,$(CFLAGS)) -msse -msse2

KOBJS := $(BUILD)/entry.o $(BUILD)/gdt.o $(BUILD)/isr.o \
         $(BUILD)/kernel.o $(BUILD)/kprintf.o $(BUILD)/vga.o $(BUILD)/serial.o \
         $(BUILD)/idt.o $(BUILD)/interrupts.o $(BUILD)/pic.o $(BUILD)/pit.o \
         $(BUILD)/keyboard.o $(BUILD)/pmm.o $(BUILD)/string.o \
         $(BUILD)/kheap.o $(BUILD)/shell.o $(BUILD)/task.o $(BUILD)/vmm.o \
         $(BUILD)/tss.o $(BUILD)/syscall.o $(BUILD)/ata.o $(BUILD)/fs.o \
         $(BUILD)/elf.o $(BUILD)/pipe.o $(BUILD)/fb.o $(BUILD)/gui.o \
         $(BUILD)/mouse.o $(BUILD)/pci.o $(BUILD)/rtl8139.o $(BUILD)/netstack.o \
         $(BUILD)/tcp.o $(BUILD)/browser.o $(BUILD)/sha256.o $(BUILD)/aes.o \
         $(BUILD)/x25519.o $(BUILD)/tls.o $(BUILD)/js.o

# Limite impuse de lantul de boot: stage1 citeste 192 de sectoare
# (8 pentru stage2 + 184 pentru kernel).
STAGE2_MAX := 4096
KERNEL_MAX := 192512

all: $(BUILD)/myos.img

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/stage1.bin: boot/stage1.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/stage2.bin: boot/stage2.asm | $(BUILD)
	nasm -f bin $< -o $@
	@sz=$$(stat -c%s $@); if [ $$sz -gt $(STAGE2_MAX) ]; then \
		echo "EROARE: stage2 are $$sz bytes, peste limita de $(STAGE2_MAX)"; exit 1; fi
	truncate -s $(STAGE2_MAX) $@

$(BUILD)/%.o: kernel/%.asm | $(BUILD)
	nasm -f elf64 $< -o $@

# Programele user: binare flat, livrate prin sistemul de fisiere MyFS.
$(BUILD)/hello.bin: user/hello.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/crash.bin: user/crash.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/guess.bin: user/guess.asm | $(BUILD)
	nasm -f bin $< -o $@

# Programe user in C: freestanding + ulib, linkate ca ELF static la
# 0x8000000000 — kernelul le incarca prin loaderul ELF.
# -mcmodel=large: adresa de baza nu incape in relocari pe 32 de biti.
UCFLAGS := $(CFLAGS) -mcmodel=large
UPROGS  := ush calc edit basic show upper lines nslookup telnet fetch

$(BUILD)/ulib.o: user/lib/ulib.c user/lib/ulib.h | $(BUILD)
	gcc $(UCFLAGS) -c $< -o $@

# .uo (obiect user) ca sa nu se bata cap in cap cu regula pentru kernel/%.c
$(BUILD)/%.uo: user/%.c user/lib/ulib.h | $(BUILD)
	gcc $(UCFLAGS) -c $< -o $@

$(BUILD)/%.elf: $(BUILD)/%.uo $(BUILD)/ulib.o user/user.ld
	ld -T user/user.ld -z max-page-size=0x1000 -o $@ $< $(BUILD)/ulib.o

# Unealta mkfs (ruleaza pe host) si imaginea sistemului de fisiere.
$(BUILD)/mkfs: scripts/mkfs.c | $(BUILD)
	gcc -O2 -o $@ $<

UELFS    := $(patsubst %,$(BUILD)/%.elf,$(UPROGS))
FS_FILES := $(foreach p,$(UPROGS),$(p)=$(BUILD)/$(p).elf) \
            hello=$(BUILD)/hello.bin crash=$(BUILD)/crash.bin \
            guess=$(BUILD)/guess.bin demo.bas=fs/demo.bas \
            readme.txt=fs/readme.txt \
            splash.raw=fs/splash.raw desk.raw=fs/desk.raw \
            docs/bun-venit.txt=fs/docs-bun-venit.txt \
            docs/idei.txt=fs/docs-idei.txt \
            sys/info.txt=fs/sys-info.txt

$(BUILD)/fs.img: $(BUILD)/mkfs $(UELFS) $(BUILD)/hello.bin \
                 $(BUILD)/crash.bin $(BUILD)/guess.bin fs/demo.bas \
                 fs/readme.txt fs/splash.raw fs/desk.raw \
                 fs/docs-bun-venit.txt fs/docs-idei.txt fs/sys-info.txt
	$(BUILD)/mkfs $@ $(FS_FILES)

$(BUILD)/%.o: kernel/%.c | $(BUILD)
	gcc $(CFLAGS) -c $< -o $@

# js.o compilat cu SSE (double)
$(BUILD)/js.o: kernel/js.c kernel/js.h kernel/js_lib.h | $(BUILD)
	gcc $(CFLAGS_SSE) -c $< -o $@

$(KOBJS): $(wildcard kernel/*.h)

$(BUILD)/kernel.elf: $(KOBJS) kernel/linker.ld
	ld -T kernel/linker.ld -o $@ $(KOBJS)

$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	objcopy -O binary $< $@
	@sz=$$(stat -c%s $@); if [ $$sz -gt $(KERNEL_MAX) ]; then \
		echo "EROARE: kernelul are $$sz bytes, peste limita de $(KERNEL_MAX)"; exit 1; fi

# Imaginea finala: boot (stage1+stage2+kernel) in primul 1 MiB,
# sistemul de fisiere MyFS de la 1 MiB (LBA 2048) incolo — zona FS: 3 MiB.
# NOTA: fisierele salvate din MyOS persista in build/myos.img pana la
# urmatorul `make` care o regenereaza.
$(BUILD)/myos.img: $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(BUILD)/kernel.bin $(BUILD)/fs.img
	cat $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(BUILD)/kernel.bin > $@
	truncate -s 1M $@
	cat $(BUILD)/fs.img >> $@
	truncate -s 12M $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
