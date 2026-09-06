Salut! Acest text e citit de pe disc, prin driverul ATA al lui DevOS,
dintr-un sistem de fisiere scris de la zero (MyFS).

ECRAN: DevOS ruleaza Full HD 1920x1080 (driver video propriu, prin
interfata VBE dispi). Din meniul Start -> Setari -> Display poti schimba
rezolutia la cald (1920x1080 / 1280x720 / 1024x768) sau cu tastele 1/2/3.

MENIUL START: apasa butonul DevOS din stanga-jos SAU tasta Windows (Super).
De acolo pornesti Terminal, Explorer, Editor, Task Manager, Browser, Setari
si poti da Repornire sau Oprire. FERESTRE: butonul verde = maximizeaza
(sau tasta F11), galben = minimizeaza, rosu = inchide (si termina procesele).

RETEA: DevOS are placa de retea (RTL8139), stiva IP completa (ARP/IPv4/ICMP/
UDP/TCP), DNS, server telnet SI client TCP outbound. Iese pe internet!
  - server telnet: de pe Windows  telnet localhost 2323  (sau PuTTY).
  - in terminalul DevOS:  ping 10.0.2.2 ,  nslookup <nume> ,
    telnet <gazda> [port]  (client, Ctrl+Q iese),  fetch <gazda> [cale]  (HTTP).
  - CLIENT SSH:  ssh <gazda> [utilizator] [port]  - shell securizat, criptat de
    la zero (curve25519-sha256 + AES-128-CTR + HMAC-SHA256). Autentificare cu
    parola, keyboard-interactive SAU CHEIE PUBLICA Ed25519 (alege automat).
    Ex.: ssh test.rebex.net demo  (parola: password). Ctrl+Q iese.
    NOTA: multe servere (ex. Debian) NU permit login cu parola pentru "root" -
    foloseste un cont ne-root sau cheie. La esec, se arata metodele acceptate.
  - CHEIE SSH:  sshkey gen  genereaza o pereche Ed25519 si afiseaza linia de pus
    in ~/.ssh/authorized_keys pe server. Apoi:  ssh <gazda> <utilizator>  si la
    promptul de parola apasa Enter -> te conectezi cu cheia, fara parola.
    Terminalul intelege ANSI/VT100: merg mc, nano, vim, top, less, ls --color.
    Tasta Caps Lock functioneaza (majuscule fara Shift).

BROWSER WEB (Alt+F7 sau butonul cu glob din taskbar): scrie o adresa in bara
de sus si apasa Enter, sau click pe un link. Merge HTTP si HTTPS (TLS 1.2 scris
de la zero: x25519 + AES-GCM + SHA-256). Randare cu CSS: marimi de font, culori,
fundaluri, aliniere, liste, bold, linkuri. Are si un MOTOR JAVASCRIPT propriu:
ruleaza JS din pagini (variabile, functii, bucle, obiecte, array-uri, DOM -
document.write, getElementById, innerHTML, createElement). Afiseaza si IMAGINI
PNG (din data: URI sau descarcate de la URL). Inapoi/inainte/reload,
istoric, word-wrap, derulare. Bara de adresa: click ca sa pui cursorul unde vrei.
Incearca https://example.com sau info.cern.ch.
Nota onesta: motorul JS ruleaza JavaScript simplu/vanilla, NU framework-uri mari
(V8/Chromium = milioane de linii). Site-urile care isi construiesc totul cu
React/Angular etc. (ex. Google) se descarca dar nu se randeaza complet.

Taskbar (centrat, stil Win11) + meniu Start (butonul DevOS sau tasta Windows):
de acolo pornesti Terminal, File Manager (Alt+F4), Notepad (Alt+F5),
Task Manager (Alt+F6), Browser (Alt+F7), Setari (Alt+F8), Calculator (Alt+F9)
si poti da Repornire / Oprire. Ferestre: verde/F11 maximizeaza, galben
minimizeaza, rosu inchide. La pornire nu e deschis niciun terminal.

Calculator (Alt+F9 sau din meniul Start): +, -, *, / cu zecimale, procent,
+/-, backspace; merge cu mouse-ul sau de la tastatura (Enter = , Esc = C).

Task Manager: vezi toate procesele, CPU%, RAM, dimensiune pe disc si
locatia; selecteaza un proces si apasa "Termina task" ca sa-l opresti.

Programe pentru ring 3 (porneste-le cu "run"):
  run hello    - programul demo cu syscalls (asm)
  run crash    - programul care incearca sa atace kernelul (si moare)
  run guess    - joc interactiv: ghiceste numarul intre 1 si 100 (asm)
  run calc     - calculator interactiv scris in C, incarcat ca ELF
  run edit X   - editor de text pe linii (l/a/d/w/q), salveaza pe disc
  basic X.bas  - interpretor BASIC! scrie programe cu edit si ruleaza-le
                 (incearca: basic demo.bas)
Adauga " &" la final ca sa ruleze in fundal (ex: run hello &).

File Manager (Alt+F4 sau butonul cu folder din taskbar):
  - foldere navigabile (docs/, sys/) - dublu-click sau Enter intra,
    ".. (inapoi)" sau click pe "MyFS" in breadcrumb urca
  - fisierele de sistem sunt marcate "(sistem)" si protejate
  - click dreapta = meniu contextual (Deschide/Redenumeste/Copiaza/Sterge)
  - Enter/dublu-click pe un fisier text il deschide in Notepad

Notepad - editor grafic modern (Alt+F5 sau butonul din taskbar):
  - tastezi liber; sagetile/Home/End misca cursorul; Delete/Backspace sterg
  - selectie: Shift+sageti/Home/End sau trage cu mouse-ul
  - Ctrl+A = tot, Ctrl+C = copiaza, Ctrl+X = taie, Ctrl+V = lipeste
  - Ctrl+N = document nou, Ctrl+O = deschide, Ctrl+S = salveaza
  - clipboard-ul se pastreaza intre documente (Deschide alt fisier + lipeste)
