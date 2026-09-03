Salut! Acest text e citit de pe disc, prin driverul ATA al lui MyOS,
dintr-un sistem de fisiere scris de la zero (MyFS).

RETEA: MyOS are placa de retea (RTL8139), stiva IP completa (ARP/IPv4/ICMP/
UDP/TCP), DNS, server telnet SI client TCP outbound. Iese pe internet!
  - server telnet: de pe Windows  telnet localhost 2323  (sau PuTTY).
  - in terminalul MyOS:  ping 10.0.2.2 ,  nslookup <nume> ,
    telnet <gazda> [port]  (client, Ctrl+Q iese),  fetch <gazda> [cale]  (HTTP).

BROWSER WEB (Alt+F7 sau butonul cu glob din taskbar): scrie o adresa in bara
de sus si apasa Enter, sau click pe un link. Are inapoi/inainte/reload, istoric,
word-wrap, titluri, linkuri clicabile si derulare (sagetile sus/jos).
Momentan doar HTTP (http://...); HTTPS/TLS urmeaza. Incearca info.cern.ch.

Taskbar: buton Terminal (click = deschide/comuta; click dreapta = Terminal
nou), File Manager (Alt+F4), Notepad (Alt+F5), Task Manager (Alt+F6),
Browser (Alt+F7). La pornire nu e deschis niciun terminal - il deschizi cand ai nevoie.

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
