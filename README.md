# DevOS (Developer OS)

Sistem de operare x86-64 scris de la zero de **Gavrilencu Grigore**: bootloader
propriu (fără GRUB), kernel în C. Build în WSL, rulare cu QEMU pe Windows.
Rulează grafic la **Full HD 1920x1080**, cu meniu Start, aplicații în ferestre
(Terminal, Explorer, Editor, Task Manager, Browser), Setări și oprire/repornire.
(Denumit anterior „MyOS" — de aici referințele istorice din roadmap.)

## Build și rulare

```powershell
wsl make                                              # compileaza build\myos.img
powershell -ExecutionPolicy Bypass -File scripts\run.ps1   # porneste QEMU
```

Ieșirea serială (COM1) apare în terminal; fereastra QEMU arată consola VGA.

## Cum pornește (lanțul de boot)

1. **BIOS** citește sectorul 0 al discului la `0x7C00` și sare acolo.
2. **stage1** (`boot/stage1.asm`, 512 bytes) citește 384 de sectoare de pe
   disc (stage2 + kernel) la `0x8000`, folosind int 13h extins (LBA).
   *Bonus în stage2:* înainte de a părăsi real mode, cere de la BIOS harta
   memoriei (int 15h/E820) și o depune la `0x5000` pentru kernel.
3. **stage2** (`boot/stage2.asm`) activează linia A20, încarcă GDT-ul, trece
   în protected mode (32-bit), construiește tabelele de paginare (identity
   map pe primul 1 GiB cu pagini de 2 MiB), activează PAE + EFER.LME + CR0.PG
   și sare în long mode (64-bit) la kernel.
4. **kernel** (`kernel/`, linkat la `0x9000`): `entry.asm` curăță `.bss` și
   setează stiva, apoi `kmain()` inițializează driverele VGA text și serial,
   își instalează propriul GDT + IDT, remapează PIC-ul și activează
   întreruperile (PIT la 100 Hz + tastatură PS/2).

## Harta memoriei

| Adresă            | Conținut                                   |
|-------------------|--------------------------------------------|
| `0x1000`–`0x3FFF` | tabele de paginare (PML4, PDPT, PD)        |
| `0x5000`–`0x5608` | harta E820 (număr de intrări + tabel)      |
| `0x7C00`          | stage1 (+ stivă temporară sub el)          |
| `0x8000`–`0x8FFF` | stage2 (max 4 KiB)                         |
| `0x9000`–...      | kernel (max ~188 KiB, limită din stage1)   |
| `0xB8000`         | buffer VGA text 80×25                      |

## Structura

```
boot/     stage1.asm (boot sector), stage2.asm (trecerea in long mode)
kernel/
  entry.asm       punctul de intrare 64-bit: curata .bss, seteaza stiva
  kernel.c        kmain() — initializare si bucla principala
  gdt.asm         GDT-ul kernelului (cod 0x08, date 0x10)
  isr.asm         stub-urile celor 48 de vectori de intrerupere
  idt.c           construirea si incarcarea IDT-ului
  interrupts.c    dispatch: exceptii (panica cu dump de registre) si IRQ-uri
  pic.c/h         remaparea 8259A pe vectorii 32-47, EOI, masti
  pit.c/h         timerul (IRQ0), contor de tick-uri
  keyboard.c/h    tastatura PS/2 (IRQ1), scancode set 1, shift
  pmm.c/h         alocator de memorie fizica (bitmap pe cadre de 4 KiB, E820)
  vmm.c/h         memorie virtuala: tabele de paginare proprii, map/unmap/
                  translate pe pagini de 4 KiB, spatii de adrese per proces
                  (kernel partajat prin PML4[0], user privat in PML4[1])
  kheap.c/h       heap-ul kernelului: kmalloc/kfree (free-list cu coalescenta)
  task.c/h        multitasking: task-uri de kernel SI de user (ring 3),
                  scheduler round-robin preemptiv, sleep/yield/exit
  tss.c/h         TSS: stiva de kernel (RSP0) pentru intreruperi din ring 3
  syscall.c/h     syscalls prin int 0x80: write, exit, getpid, sleep, readc,
                  ticks, fread/fwrite/fdelete/flist (fisiere), spawn/alive
                  (procese), clear — toti pointerii user validati
  elf.c/h         incarcator ELF64 (ET_EXEC, segmente PT_LOAD, .bss zero)
  pipe.c/h        pipe-uri intre procese (buffer circular 4 KiB, EOF la
                  moartea scriitorului)
  ata.c/h         driver de disc ATA PIO (LBA28, polling, citire + scriere)
  fs.c/h          MyFS: sistem de fisiere propriu, read-write, persistent
  shell.c/h       shell interactiv: help, clear, echo, mem, uptime, heap,
                  ps, spawn, vmtest, ls, cat, save, rm, run, panic, reboot
  string.c/h      memset/memcpy/memmove/memcmp/strlen/strcmp
  kprintf.c/h     consola formatata (VGA + serial simultan)
  fb.c/h          driver framebuffer (VESA 1024x768x32): pixeli, dreptunghiuri
                  (si rotunjite), text cu fontul 8x16 al BIOS-ului, text scalat
  mouse.c/h       mouse PS/2 pe IRQ12 (port auxiliar 8042, pachete de 3 bytes)
  gui.c/h         splash de boot animat; desktop: wallpaper, taskbar cu buton
                  MyOS + tab-uri clickabile F1-F3 (hover) + ceas RTC real,
                  meniu Start cu info sistem, cursor de mouse cu umbra,
                  set propriu de iconite 16x16, animatie de deschidere a
                  ferestrei terminalului
  vga.c/h         terminale virtuale (Alt+F1..F3): ecran, cursor, culoare si
                  coada de tastatura per terminal; backend dublu: text VGA
                  (fallback) sau celule desenate pe framebuffer
  serial.c/h      COM1 115200 — canalul de debugging
  io.h            inb/outb
  linker.ld       kernel linkat la 0x9000, .entry prima sectiune
user/     programe user: hello, crash, guess (asm flat) + ush (shell-ul
          user!), calc, edit, basic (interpretor BASIC!), show, upper,
          lines (filtre pentru pipeline-uri) — C, ELF;
          lib/ulib.c = mini-biblioteca C (syscalls, print, readline, fisiere);
          user.ld = linker script (baza 0x8000000000); programele definesc
          umain(args) — argumentele vin de la `run <prog> <args>` prin RDI
fs/       fisiere incluse in imaginea MyFS (ex. readme.txt)
scripts/  run.ps1 (porneste QEMU pe Windows), mkfs.c (unealta de impachetat FS)
build/    artefacte (myos.img = imaginea de disc finală, 2 MiB)
```

## Layoutul imaginii de disc

| Offset          | Conținut                                     |
|-----------------|----------------------------------------------|
| 0 – 512 B       | stage1 (boot sector)                         |
| 512 B – 4,5 KiB | stage2                                       |
| 4,5 KiB – ...   | kernelul (max ~188 KiB)                      |
| 1 MiB (LBA 2048)| MyFS: superbloc `MYFS` + tabel (4 sectoare, max 63 fișiere) + date (3 MiB) |

Fișierele salvate din MyOS (`save`) persistă în `build/myos.img` peste
reboot — până la următorul `wsl make`, care regenerează imaginea.

## Roadmap

- [x] Milestone 1: boot propriu până în long mode, VGA + serial
- [x] Milestone 2: GDT/IDT proprii, handlere de excepții (panică cu dump de
      registre), kprintf, PIC remapat, PIT la 100 Hz, tastatură PS/2
- [x] Milestone 3: harta E820 din BIOS (cerută în stage2) + alocator fizic
      de memorie (bitmap pe cadre de 4 KiB, gestionează până la 1 GiB)
- [x] Milestone 4: heap de kernel (kmalloc/kfree) + shell interactiv cu
      buffer de tastatură (help, clear, echo, mem, uptime, heap, panic, reboot)
- [x] Milestone 5: multitasking preemptiv — task-uri de kernel cu stive
      proprii, scheduler round-robin pe tick-ul PIT, sleep/yield/exit,
      comenzile `ps` și `spawn` (comutarea de context schimbă RSP-ul cu care
      stub-ul ISR face iretq)
- [x] Milestone 6: manager de memorie virtuală — kernelul își construiește
      propriile tabele de paginare (pagini de 4 KiB), `vmm_map`/`vmm_unmap`/
      `vmm_translate` cu alocarea automată a tabelelor intermediare + `invlpg`;
      comanda `vmtest`
- [x] Milestone 7: userspace! Selectori user (DPL3) + TSS cu RSP0 per task,
      syscalls prin `int 0x80` (write/exit/getpid/sleep), programe user flat
      mapate la 2 GiB cu bitul U (în afara identity map-ului), excepție din
      ring 3 = doar task-ul vinovat moare (`ucrash` demonstrează izolarea)
- [x] Milestone 8: driver de disc ATA PIO + sistem de fișiere propriu (MyFS,
      read-only, construit la build de `scripts/mkfs.c`) — comenzile `ls`,
      `cat`, `run <program>` încarcă programele user de pe disc
- [x] Milestone 9: spații de adrese per proces — fiecare task user are
      propriul PML4 (user la 512 GiB = slotul 1 din PML4, kernelul partajat
      prin slotul 0); schedulerul comută CR3; mai multe programe user rulează
      simultan, complet izolate; `vmm_destroy_space` eliberează tot fără leak
- [x] Milestone 10: FS read-write — `ata_write` (+ flush), tabel MyFS cu
      regiune fixă, `fs_save`/`fs_delete`, comenzile `save`/`rm`; fișierele
      persistă peste reboot (verificat cu reset hardware complet)
- [x] Milestone 11: programe user interactive — syscalls `readc` (non-blocant,
      polling cu sleep) și `ticks`; `run` rulează în foreground (shell-ul
      cedează tastatura programului; `run <nume> &` = fundal); jocul `guess`
      în ring 3, cu echo și backspace făcute de programul user
- [x] Milestone 12: programe user în C — loader ELF64 în kernel (PT_LOAD la
      adresele lor, memsz>filesz = .bss zero), mini-biblioteca `ulib`
      (syscalls, print, readline), calculatorul `calc` compilat cu
      `-mcmodel=large` (baza la 512 GiB nu încape în relocări 32-bit)
- [x] Milestone 13: syscalls de fișiere (`fread`/`fwrite`, pointeri user
      validați) + argumente pentru programe (`run edit nota.txt`) + editorul
      de text `edit` (l/a/d/w/q) — prima aplicație reală a sistemului
- [x] Milestone 14: shell-ul user `ush` — kmain devine „init" și pornește
      `ush` (ring 3) la boot; syscalls noi: spawn/alive (procese),
      flist/fdelete, clear; comenzile sunt programe („calc", „edit x",
      „hello &") ca în Unix; `exit` te lasă în shell-ul kernel (debugging)
- [x] Milestone 15: interpretor BASIC (`basic <fisier.bas>`) — program user
      pur, zero modificări de kernel: PRINT/LET/INPUT/IF-THEN/GOTO/
      FOR-NEXT-STEP/REM/END/RND, expresii cu paranteze, variabile a-z.
      Bucla completă: `edit prog.bas` → `basic prog.bas` — programezi
      în interiorul propriului OS
- [x] Milestone 16: terminale virtuale — 3 console cu ecran/cursor/tastatură
      proprii, comutate cu Alt+F1..F3; task-urile scriu/citesc pe terminalul
      lor (moștenit de la părinte); init pornește câte un ush pe fiecare
      terminal și îl respawnează pe cel care moare; syscalls `pslist`/`meminfo`
      (ps și mem sunt acum builtin-uri în ush)
- [x] Milestone 17: pipe-uri — `prog1 | prog2` în ush: bufferul circular din
      kernel leagă stdout-ul unui proces de stdin-ul altuia (syscalls
      `pipe`/`spawn2`); write parțial când pipe-ul e plin, EOF când
      scriitorul moare; filtrele `show`, `upper`, `lines`
- [x] Milestone 18: mod grafic + GUI — stage2 setează VESA 1024x768x32 (căutare
      în lista de moduri VBE, LFB) și copiază fontul 8x16 al BIOS-ului; splash
      de boot cu logo și bară de progres; desktop cu wallpaper, taskbar
      (tab-uri F1-F3 + ceas) și terminalul într-o fereastră cu titlu; fallback
      automat pe modul text dacă VBE lipsește
- [x] Milestone 19: mouse PS/2 + GUI modern — cursor desenat de kernel (cu
      salvarea fundalului), tab-uri clickabile cu hover, meniu Start cu
      iconițe proprii și info live, ceas RTC (CMOS), colțuri rotunjite,
      animație la deschiderea ferestrei; totul verificat vizual prin
      screendump-uri
- [x] Milestone 20-21: window manager — fiecare terminal în fereastra lui,
      mutabilă cu mouse-ul FĂRĂ flicker (clipping în framebuffer + repararea
      doar a fâșiilor expuse), z-order cu ocluzie per celulă, focus prin
      click, butoane funcționale (roșu=închidere care curăță terminalul,
      galben=minimizare, verde=trimitere în fundal; restaurare din tab),
      splash cu inel de puncte rotitor + procent live
- [x] Milestone 22: imagini de fundal + finisaj vizual — `scripts/convimg.ps1`
      convertește PNG/JPG în raw 32bpp (formatul framebuffer-ului); splash-ul
      folosește `background1.png`, desktopul `background.jpg` (încărcate de
      pe MyFS direct în RAM, imaginea de disc a crescut la 12 MiB); colțuri
      cu adevărat rotunde (arc de cerc întreg), tab-uri și butoane pastilă,
      butoane de fereastră circulare, umbre rotunjite
- [x] Milestone 23: finisaj + funcționalitate (v0.23)
      - **Anti-aliasing** pe toată interfața: arcele colțurilor rotunde au
        acoperire per pixel amestecată cu fundalul (`fb_fill_round2`, moduri:
        dur / AA pe culoare solidă / AA pe ecran); la redesenarea ferestrelor,
        colțurile se restaurează întâi din fundal ca blending-ul să rămână
        deterministic; spinner-ul splash-ului se restaurează din imagine
      - **Editorul** a crescut: `p N [M]` (interval), `i N` (inserare),
        `r N` (înlocuire), `f text` (căutare), `n` (statistici), pe lângă
        l/a/d/w/q
      - **Săgeți + istoric**: driverul de tastatură livrează codurile
        sus/jos/stânga/dreapta (0x80..0x83), iar `readline` din ulib are
        istoric de 8 comenzi navigabil cu săgețile — automat în ush, edit,
        basic, calc
      - **ush**: builtin-uri noi `cp`, `mv` (max 8 KiB) și `date` (ora
        hardware prin syscall-ul nou 17 = rtc)
      - **BASIC**: `gosub N` / `return` (subrutine, max 8 nivele) — demo.bas
        actualizat
- [x] Milestone 24: **File Manager grafic** (v0.24) — Explorer-ul lui MyOS:
      - fereastră proprie în window manager (a 4-a, alături de terminale),
        deschisă din butonul cu folder din taskbar sau cu **Alt+F4**
      - listă de fișiere cu iconițe pe tip (aplicație/document), dimensiuni,
        rânduri alternate, selecție, scroll cu săgeți la peste 13 fișiere
      - toolbar: **Nou** (creează fișier), **Redenum.**, **Sterge**,
        **Editeaza** — cu introducerea numelui direct în bara de status
        (Enter=ok, Esc=anulează)
      - navigare completă din tastatură: săgeți sus/jos = selecție,
        Enter = deschide în editor; dublu-click = la fel
      - „deschide în editor" injectează comanda `edit <nume>` în shell-ul
        terminalului activ și îi dă focusul — integrare reală FM↔terminal
      - `fs_rename` nou în MyFS (redenumire instant din tabel, oricât de
        mare e fișierul); limita kernelului ridicată la 92 KiB (stage1
        citește acum 192 de sectoare)
- [x] Milestone 25: **File Manager redesenat în stil Explorer/Finder** (v0.25)
      - layout modern pe zone: **breadcrumb** sus („MyFS › categoria"),
        **toolbar** (+ Nou, Redenum., Copiaza, Sterge, Editeaza), **sidebar**
        stânga cu categorii, listă cu rânduri alternate, **status bar**
        („N elemente")
      - **categorii funcționale** în sidebar (filtre reale): Toate / Programe
        (fișiere fără punct) / Documente (cu extensie) — click sau tastele
        1/2/3; breadcrumb-ul reflectă categoria
      - **meniu contextual la click dreapta**: pe fișier → Deschide /
        Redenumeste / Copiaza / Sterge; pe spațiu gol → Fisier nou /
        Reimprospateaza (se închide cu click în afară sau Esc)
      - **Copiaza**: butonul + meniul creează `c_<nume>` (copie reală pe disc)
      - vederea filtrată e o indirecție (`fm_map`) peste fișierele MyFS, cu
        scroll propriu
- [x] Milestone 26: **foldere navigabile + fișiere de sistem protejate**
      (v0.26) — la cererea utilizatorului
      - **foldere virtuale**: MyFS rămâne plat, dar numele pot conține `/`
        ca separator de cale; File Manager-ul le afișează ierarhic. Foldere
        demo livrate: `docs/` (bun-venit.txt, idei.txt) și `sys/` (info.txt)
      - **navigare**: dublu-click sau Enter pe un folder intră în el; rândul
        `.. (inapoi)` sau click pe „MyFS" din breadcrumb urcă; breadcrumb-ul
        arată calea curentă (`MyFS › docs`)
      - **fișiere de sistem protejate**: programele și fișierele livrate de
        build (plus tot din `sys/`) sunt marcate `(sistem)` cu text estompat
        și **nu pot fi șterse sau redenumite** — nici din File Manager, nici
        din terminal (`rm ush` → „fisier de sistem, protejat"); `fs_delete`
        și `fs_rename` refuză fișierele protejate în kernel
      - creare/redenumire respectă folderul curent (numele primește prefixul
        căii); din terminal fișierele din foldere se accesează prin cale
        (`cat docs/idei.txt`)
- [x] Milestone 27: **Notepad — editor de text grafic** (v0.27), ca în
      Windows, la cererea utilizatorului
      - fereastră proprie în window manager (a 5-a), deschisă cu **Alt+F5**,
        din butonul cu carnet din taskbar, sau când deschizi un fișier text
        din File Manager (Enter/dublu-click)
      - **editare liberă** pe o „foaie" albă: tastezi și caracterele apar la
        cursor; săgeți sus/jos/stânga/dreapta, **Home/End**, **Delete**,
        Backspace, Enter (linie nouă); word-wrap la marginea foii; scroll
        automat; click în text mută cursorul
      - **toolbar** Nou / Deschide / Salveaza + scurtături **Ctrl+N / Ctrl+O
        / Ctrl+S** (detectarea Ctrl adăugată în driverul de tastatură);
        „Salveaza" cere un nume dacă documentul e nou
      - titlul reflectă numele fișierului și starea „modificat" (`*`); status
        bar cu numărul liniei curente
      - fișierele de sistem nu pot fi suprascrise din Notepad
- [x] Milestone 28: **copy/paste în Notepad + design Windows 11** (v0.28)
      - **selecție de text**: Shift+săgeți/Home/End, sau trage cu mouse-ul;
        selecția e evidențiată cu fundal albastru deschis (stil Win11)
      - **clipboard global**: Ctrl+A (tot), Ctrl+C (copiază), Ctrl+X (taie),
        Ctrl+V (lipește); clipboard-ul persistă între documente, deci poți
        copia dintr-un fișier și lipi în altul
      - scrisul peste o selecție o înlocuiește; Backspace/Delete pe selecție
        o șterg
      - **modernizare vizuală Fluent/Win11**: paletă nouă (accent albastru,
        suprafețe „Mica" gri-albastrui), toolbar Notepad cu **iconițe**
        (pagină/folder/dischetă) lângă text, colțuri și spacing mai generoase
- [x] Milestone 29: **fix cursor Notepad după Enter** (v0.29) — bug raportat
      de utilizator: după Enter, caretul apărea deplasat cu o poziție pe
      linia veche (arăta „ca un spațiu"), iar textul mergea corect pe linia
      nouă. Cauza: plasarea caretului în bucla de desenare folosea o condiție
      `np_cur <= e` care, pentru o linie terminată cu `\n`, punea cursorul la
      coloana = lungimea liniei. Rezolvat rescriind calculul caretului să
      folosească direct `np_vrow_of(np_cur)` (linia vizuală) și
      `np_cur - vs[linia]` (coloana) — o singură sursă de adevăr
- [x] Milestone 30: terminale la cerere + Task Manager + imagini de fundal
      automate (v0.30), la cererea utilizatorului
      - **imaginile de fundal se schimbă ușor**: `scripts/run.ps1` reconvertă
        automat `background1.png` (splash) și `background.jpg` (desktop) în
        `.raw` la fiecare pornire, apoi compilează și rulează — doar
        înlocuiești fișierele și rulezi run.ps1
      - **terminale la cerere**: la boot NU mai pornesc 3 terminale; taskbar-ul
        are un singur buton „Terminal" (nepornit). Click = deschide/comută;
        **click dreapta → „Terminal nou"** deschide încă unul (până la 3);
        când shell-ul iese, terminalul se închide
      - **Task Manager** (Alt+F6 sau butonul cu grafic): listă live cu toate
        procesele — PID, nume, stare, **CPU%** (contorizat pe tick-uri), RAM,
        dimensiune pe disc, locație; buton **„Termina task"** (kill); se
        actualizează o dată pe secundă
      - bug critic reparat: animația de deschidere a ferestrelor busy-aștepta
        pe `pit_ticks`/`hlt`, dar din context IRQ (click/Alt+Fx) PIT-ul e
        oprit → îngheț; înlocuită cu buclă de delay
- [x] Milestone 31: fix corupție de afișaj + note mouse (v0.31)
      - **corupția de afișaj** (dungi RGB la mișcarea mouse-ului): cauza a
        fost animația de deschidere a ferestrelor, care busy-aștepta într-o
        buclă lungă cu întreruperile oprite (context IRQ) — bloca procesarea
        și pierdea pachete de mouse; eliminată (ferestrele apar instant)
      - **serializare framebuffer**: desenarea consolei (context de task) e
        acum atomică față de IRQ-ul de mouse/timer (cli/sti în console_putc /
        console_repaint_term și în deschiderea/închiderea terminalelor), ca
        output-ul să nu fie întrerupt mid-desen
      - `-vga std` explicit în `run.ps1` (VGA stabil pe Windows) + mesaj că
        mouse-ul se „prinde" cu un click în fereastră (Ctrl+Alt+G eliberează)
- [x] Milestone 32: **double-buffering** (v0.32) — afișaj fără tearing/dungi
      - desenele merg acum într-un **back buffer** din RAM; `fb_flush` copiază
        regiunea modificată pe ecran, chemat de ~50 Hz din IRQ-ul de timer;
        QEMU nu mai prinde niciodată un desen multi-rând în curs → gata cu
        dungile la mișcarea mouse-ului / suprapunerea ferestrelor
      - `fb.c`: back buffer + dreptunghi „murdar" (dirty rect) acumulat de
        toate primitivele; `fb_getpixel` citește din back buffer
      - eliminat spinner-ul demo care se suprapunea peste butonul „Terminal"
      - Alt+F1/F2/F3 deschide terminalul dacă e închis, altfel comută la el
- [x] Milestone 33: **REȚEA** — primul pas spre SSH (v0.33)
      - **PCI** (`pci.c`): enumerare, citire BAR, activare bus-master
      - **driver RTL8139** (`rtl8139.c`): init, buffer RX inelar, TX, IRQ,
        citirea adresei MAC
      - **stivă IP** (`netstack.c`): Ethernet, ARP (răspunde + învață
        gateway-ul), IPv4, ICMP (răspunde la ping), checksum
      - **TCP** (`tcp.c`): handshake (SYN/SYN-ACK/ACK), date, ACK, FIN — o
        conexiune; **server telnet pe portul 23** cu shell de rețea
        (help/ver/ls/cat/mem/uptime/echo/exit)
      - te conectezi de pe Windows: `telnet localhost 2323` (sau PuTTY) —
        `run.ps1` face port-forward 2323 (host) -> 23 (MyOS)
      - VERIFICAT: conectare TCP din Windows + comenzi rulate peste rețea
- [x] Milestone 34: **client ping** (v0.34) — `ping <a.b.c.d>` în ush trimite
      cereri ICMP echo din MyOS și afișează RTT-ul; syscalls `net_ping`/
      `net_ping_result` (non-blocante, ca readc); verificat: `ping 10.0.2.2`
      primește răspuns
- [x] Milestone 35: **client TCP outbound + DNS + client HTTP + BROWSER WEB**
      (v0.35) — MyOS iese pe internet și navighează:
      - **DNS** (`kernel/netstack.c`): interogare UDP la `10.0.2.3`, parsare
        răspuns (inclusiv pointeri de compresie), `dns_query`/`dns_result`
      - **client TCP** (`kernel/tcp.c`): active open (SYN → SYN|ACK → ACK),
        buffer de recepție inelar, alături de serverul telnet; API neblocant
        `tcp_connect`/`tcp_status`/`tcp_csend`/`tcp_crecv`/`tcp_cclose`
      - **programe user**: `nslookup <nume>`, `telnet <gazda> [port]`
        (interactiv, Ctrl+Q iese), `fetch <gazda> [cale]` (GET HTTP)
      - **BROWSER GRAFIC** (`kernel/browser.c`) — fereastra a 4-a (Alt+F7 sau
        butonul din taskbar): bară de adresă, înapoi/înainte/reload, istoric,
        parser HTML cu word-wrap, titluri, **linkuri clicabile**, derulare,
        urmărire redirect (301/302). Rețeaua rulează pe un fir de kernel
        (`task_create`) ca să aștepte politicos fără să blocheze IRQ-urile.
      - verificat: browserul a adus și afișat `http://info.cern.ch` real de pe
        internet (878 octeți, cu linkuri), plus pagini HTTP locale
      - stage1 încarcă acum 384 sectoare (kernel până la ~188 KiB)
- [x] Milestone 36: **HTTPS / TLS 1.2 scris de la zero** (v0.36) — browserul
      navighează și pe site-uri `https://`:
      - **criptografie proprie**, verificată cu vectori de test cunoscuți:
        `kernel/sha256.c` (SHA-256 + HMAC), `kernel/aes.c` (AES-128/256 + GCM),
        `kernel/x25519.c` (Curve25519 ECDH)
      - **`kernel/tls.c`**: client TLS 1.2 complet — ClientHello (cu SNI,
        supported_groups x25519, signature_algorithms), ECDHE x25519,
        AES-128-GCM, PRF (P_SHA256), ChangeCipherSpec + Finished; rulează peste
        clientul TCP, pe firul de kernel al browserului. **Nu** validează
        certificatul (OS de învățare, nu pentru securitate reală)
      - bara de adresă editabilă complet: click = pui cursorul unde vrei,
        insert/Delete/Home/End/săgeți, derulare orizontală
      - verificat: `https://example.com` afișat corect (828 octeți prin TLS);
        `https://www.google.com` — handshake TLS reușit, 22 KB descărcați și
        decriptați (dar pagina e 99% JavaScript, deci nu se randează vizual)
- [x] Milestone 37: **motor de randare CSS** (v0.37) — browserul afișează
      pagini stilizate aproape ca un browser real:
      - parsează `<style>` și `style="..."` (selectoare simple: tag, `.class`,
        `#id`); cascadă de stiluri cu moștenire pe o stivă de elemente
      - proprietăți: `color`, `background`/`background-color`, `font-size`
        (mapat la 1x/2x/3x pe fontul 8x16), `font-weight`, `text-align`
        (stânga/centru/dreapta), `display:none`
      - culori: `#rgb`, `#rrggbb`, `rgb(...)`, ~25 de nume
      - HTML mai bogat: titluri cu mărimi reale, liste `ul`/`ol` cu marcatori,
        `<hr>`, `blockquote`, comentarii, entități
      - verificat vizual: titlu mare albastru centrat, subtitlu auriu, cutie cu
        fundal galben, text 24px, listă cu marcatori, paragraf centrat roșu —
        toate din CSS
- [x] Milestone 38: **MOTOR JAVASCRIPT de la zero** (v0.38) — browserul rulează
      JavaScript din pagini:
      - `kernel/js.c` + `kernel/js_lib.h`: interpretor tree-walking (lexer,
        parser cu precedență, evaluator), arenă bump resetată per pagină;
        variabile (`var`/`let`/`const`), funcții (inclusiv arrow), `if`/`for`/
        `for-in`/`while`/`do`, obiecte, array-uri, string-uri, operatori,
        închideri (closures), recursie; bibliotecă: `console`, `Math`,
        metode `String`/`Array` (map/filter/forEach/split/join...), `parseInt` etc.
      - **DOM**: `kernel/browser.c` construiește un arbore DOM din HTML, rulează
        `<script>`-urile, apoi re-serializează + reașează. API DOM: `document.write`,
        `getElementById`, `.innerHTML`, `.textContent`, `createElement`,
        `appendChild`, `setAttribute`, `.style.*`
      - verificat pe host cu 28 de teste + vizual în MyOS: o pagină care
        modifică DOM-ul cu `getElementById().innerHTML`, generează o listă
        dintr-un `for`, scrie cu `document.write` și folosește `Array.map` —
        toate se randează corect
      - a necesitat SSE (numerele JS sunt `double`): `js.o` compilat cu SSE,
        activat la boot (doar firul browserului îl folosește); stiva firelor
        de kernel mărită la 128 KiB (parserul/evaluatorul recursează)
- [x] Milestone 39: **IMAGINI (PNG)** in browser (v0.39):
      - `kernel/inflate.c` (decompresor DEFLATE, RFC 1951) + `kernel/png.c`
        (decodor PNG: tipuri gri/RGB/paleta/RGBA pe 8 biti, defiltrare); testate
        pe host cu vectori reali
      - browserul afiseaza `<img>`: din `data:` URI (base64) SI descarcate de la
        URL (http/https, prin `fetch_url_binary` peste acelasi client TCP/TLS),
        scalate ca sa incapa in latimea paginii
      - verificat vizual: un PNG (benzi colorate) decodat de MyOS si afisat in
        pagina de start dintr-un `data:` URI
- [x] Milestone 40: **Formulare HTML** in browser (v0.40):
      - `<input>` (text/password), `<textarea>`, checkbox, `<button>`/submit;
        focus cu click sau Tab, editare cu cursor
      - trimitere GET (`action?query`) si POST (Content-Length + corp), cookies
        de sesiune (Set-Cookie -> jar per gazda -> antet Cookie) pentru login
- [x] Milestone 41: **Full HD + desktop DevOS** (v0.41):
      - driver video propriu prin interfata Bochs VBE **dispi** (0x1CE/0x1CF):
        boot la **1920x1080**, schimbare de rezolutie la cald din Setari
        (`fb_set_mode`); wallpaper procedural independent de rezolutie
      - **redenumire MyOS -> DevOS** (dezvoltat de Gavrilencu Grigore)
      - **meniu Start = launcher**: Terminal, Explorer, Editor, Task Manager,
        Browser, Setari + **Repornire** + **Oprire** (tasta Windows/Super)
      - **Setari** (Alt+F8): sectiunile *Display* (rezolutie) si *Despre*
      - **ferestre redimensionabile**: verde/F11 = maximizeaza, galben =
        minimizeaza, rosu = inchide + termina procesele; browser si aplicatii
        se re-incadreaza la dimensiunea ferestrei
      - Task Manager live; taskbar mereu jos la orice rezolutie; QEMU full-screen
- [x] Milestone 42: **UI stil Windows 11 + iconuri colorate** (v0.42):
      - **iconuri dintr-o librarie de iconuri** (fontul Segoe Fluent/MDL2):
        `scripts/genicons.ps1` le randeaza pe host cu anti-aliasing (tile colorat
        + glifa) in `fs/ic_*.raw` (RGBA 40x40); kernelul le incarca de pe disc si
        le deseneaza cu alpha blending (`fb_blit_rgba`) — nu mai sunt pixel-art
      - **taskbar centrat** (stil Win11) cu iconurile aplicatiilor + indicator sub
        aplicatia activa; **meniu Start** centrat cu grid de iconuri + Repornire/Oprire
      - fix: imaginea de disc marita la **24 MiB** (`FS_MAX_SECTORS` marit) ca sa
        incapa wallpaper-ele Full HD (2 x 8.3 MiB) + iconurile fara truncheare
      - limita ramasa: textul interfetei foloseste inca fontul bitmap 8x16 al
        BIOS-ului (un motor de font anti-aliasing e un pas separat)
- [ ] Font UI anti-aliasing (proportional), tabele, mai mult CSS, decodor JPEG
- [ ] SSH: schimb de chei Diffie-Hellman + cifru, peste TCP

**Limită onestă:** motorul JS rulează JavaScript simplu/vanilla, dar **nu** e V8:
nu rulează framework-uri mari (React/Angular) și nu are toate API-urile web.
Site-uri ca Google, care își construiesc interfața integral cu JS de framework,
se descarcă și se decriptează dar nu se randează complet. Un clon exact de Chrome
(V8 + DOM complet + sutele de API-uri = ~35M de linii în Chromium) nu e
realizabil de la zero. Paginile de conținut HTML+CSS+JS-vanilla se afișează bine.

## Cum testezi o panică

Scrie comanda `panic` în shell — kernelul scrie deliberat la `0x40000000`
(prima adresă nemapată) și vei vedea ecranul roșu de panică cu
`#PF page fault`, adresa vinovată din CR2 și dump-ul complet al registrelor.
#   d e v o s  
 