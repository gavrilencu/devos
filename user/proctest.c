/* proctest — demonstreaza modelul de procese (Milestone 59):
 * fork() creeaza un copil (o copie a procesului), parintele si copilul
 * ruleaza concurent, iar parintele asteapta copilul cu wait() si ii afla
 * codul de iesire. */

#include <stdint.h>
#include "lib/ulib.h"

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* proctest exec — testeaza fork + exec: copilul isi inlocuieste imaginea cu
 * programul "hi", care iese cu codul 7. */
static int demo_exec(void)
{
    print("proctest exec: parinte PID=");
    print_num(getpid());
    print("\n");

    int64_t pid = fork();
    if (pid < 0) {
        print("proctest: fork a esuat\n");
        return 1;
    }
    if (pid == 0) {
        print("  [copil] PID=");
        print_num(getpid());
        print(", inlocuiesc imaginea cu 'hi' prin exec...\n");
        exec("hi", "salut-din-exec");
        print("  [copil] exec a esuat!\n");   /* nu ar trebui sa se ajunga aici */
        return 1;
    }
    int code = wait_pid(pid);
    print("parinte: procesul (dupa exec) a iesit cu codul ");
    print_num(code);
    print("\n");
    return 0;
}

int umain(const char *args)
{
    if (args && streq(args, "exec"))
        return demo_exec();

    print("proctest: parinte PID=");
    print_num(getpid());
    print(", PPID=");
    print_num(getppid());
    print("\n");

    int64_t pid = fork();

    if (pid < 0) {
        print("proctest: fork a esuat (fara slot/memorie)\n");
        return 1;
    }

    if (pid == 0) {
        /* --- codul copilului --- */
        print("  [copil] rulez, PID=");
        print_num(getpid());
        print(", PPID=");
        print_num(getppid());
        print("\n");
        for (int i = 1; i <= 3; i++) {
            print("  [copil] pasul ");
            print_num(i);
            print("/3\n");
            sleep_ms(120);
        }
        print("  [copil] ies cu codul 42\n");
        return 42;                 /* codul de iesire */
    }

    /* --- codul parintelui --- */
    print("parinte: am creat copilul PID=");
    print_num(pid);
    print(", astept sa se termine...\n");

    int code = wait_pid(pid);

    print("parinte: copilul (PID=");
    print_num(pid);
    print(") a iesit cu codul ");
    print_num(code);
    print("\n");
    return 0;
}
