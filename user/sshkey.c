/* sshkey — gestioneaza cheia SSH Ed25519 a DevOS.
 *   sshkey            arata cheia publica (authorized_keys)
 *   sshkey gen        genereaza o cheie noua si o arata
 * Cheia privata se pastreaza pe disc in fisierul "id_ed25519".
 * Pune linia afisata in ~/.ssh/authorized_keys pe server ca sa te
 * poti conecta cu `ssh <gazda> <utilizator>` fara parola. */

#include <stdint.h>
#include "lib/ulib.h"

static void show_key(void)
{
    char line[256];
    int n = ssh_pubkey(line, sizeof(line));
    if (n <= 0) {
        print("Nu exista inca o cheie. Ruleaza: sshkey gen\n");
        return;
    }
    print("Cheia ta publica (pune-o in ~/.ssh/authorized_keys pe server):\n\n");
    print(line);
    print("\n");
}

int umain(const char *args)
{
    if (!args) args = "";
    int i = 0;
    while (args[i] == ' ') i++;

    if (args[i] == 'g' && args[i+1] == 'e' && args[i+2] == 'n') {
        print("Generez o cheie Ed25519 noua ...\n");
        if (ssh_keygen() != 0) {
            print("Eroare: nu pot salva cheia pe disc.\n");
            return 1;
        }
        print("Cheie generata si salvata (id_ed25519).\n\n");
        show_key();
        print("\nApoi conecteaza-te:  ssh <gazda> <utilizator>\n");
        return 0;
    }

    show_key();
    return 0;
}
