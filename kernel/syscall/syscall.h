#pragma once
#include <stdint.h>
#include "interrupts.h"

/* Interfata de syscall (int 0x80). Conventia:
 *   RAX = numarul syscall-ului, RDI/RSI/RDX = argumente, RAX = rezultatul.
 *
 *   0  write(ptr, len)  - scrie pe consola; ptr trebuie sa fie in user space
 *   1  exit()           - termina task-ul curent
 *   2  getpid()         - id-ul task-ului curent
 *   3  sleep(ms)        - doarme cel putin `ms` milisecunde
 *   4  readc()          - un caracter de la tastatura sau -1 daca nu e nimic
 *                         (non-blocant: blocarea in kernel ar opri sistemul,
 *                         asa ca programul face polling cu sleep)
 *   5  ticks()          - tick-urile de timer de la boot (10 ms/tick)
 *   6  fread(nume, buf, maxlen)  - citeste un fisier MyFS in bufferul user;
 *                                  intoarce numarul de bytes sau -1
 *   7  fwrite(nume, buf, len)    - creeaza/suprascrie un fisier MyFS;
 *                                  0 la succes, negativ la eroare
 *   8  spawn(nume, args)         - porneste un program de pe disc ca task
 *                                  user nou; intoarce id-ul sau -1
 *   9  alive(id)                 - 1 daca task-ul mai exista (pentru wait
 *                                  prin polling), altfel 0
 *   10 flist(buf, maxlen)        - listarea fisierelor ("nume marime\n");
 *                                  intoarce numarul de bytes scrisi
 *   11 fdelete(nume)             - sterge un fisier; 0 la succes
 *   12 clear()                   - curata terminalul task-ului curent
 *   13 pslist(buf, maxlen)       - tabela de task-uri, ca text
 *   14 meminfo()                 - memoria fizica libera, in bytes
 *   15 pipe()                    - creeaza un pipe; intoarce id-ul
 *   17 rtc()                     - ora hardware: (hh<<16)|(mm<<8)|ss
 *   16 spawn2(nume, args, in_pipe, out_pipe)  - spawn cu stdin/stdout
 *      redirectate prin pipe-uri (-1 = consola); al 4-lea argument in RCX.
 *      Cu redirectare: write intoarce cati bytes au incaput (0 = pipe plin,
 *      -2 = cititor mort), readc intoarce -1 = gol, -2 = EOF.
 *
 * Toti pointerii user sunt validati contra [USER_CODE_BASE, USER_STACK_TOP).
 * Intoarce cadrul cu care se continua executia (permite comutarea de
 * context la exit/sleep). */
uint64_t syscall_handler(struct int_frame *f);
