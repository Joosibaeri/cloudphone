#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#define BUF_SIZE 256

int main() {
    printf("CLOUDPHONE OS v0.1\n");

    printf("1. QR-Code Inhalt eingeben (Text)\n");
    printf("2. Manuelle IP/Port Eingabe\n");
    printf("Selection: ");

    int choice;
    if (scanf("%d", &choice) != 1) {
        fprintf(stderr, "Invalid input.\n");
        return 1;
    }

    char ip[BUF_SIZE];
    int port = 0;

    if (choice == 1) {
        printf("\nQR-Text eingeben (z.B. 192.168.0.10:3667): ");
        char qr_text[BUF_SIZE];
        scanf("%s", qr_text);

        char *colon = strchr(qr_text, ':');
        if (!colon) {
            fprintf(stderr, "Fehler: Format muss IP:PORT sein.\n");
            return 1;
        }

        *colon = '\0';
        strncpy(ip, qr_text, BUF_SIZE);
        port = atoi(colon + 1);

    } else if (choice == 2) {
        printf("\nIP: ");
        scanf("%s", ip);

        printf("Port: ");
        scanf("%d", &port);

    } else {
        printf("Ungültige Auswahl.\n");
        return 1;
    }

    printf("\nEingelesen:\n");
    printf("IP: %s\n", ip);
    printf("Port: %d\n", port);

    printf("\nSSH-Verbindung wird später hier gestartet...\n");

    /* Frage, ob SSH gestartet werden soll */
    printf("SSH jetzt starten? (j/n): ");
    char answer[8];
    if (scanf("%7s", answer) != 1) {
        fprintf(stderr, "Fehler beim Lesen der Eingabe.\n");
        return 1;
    }

    if (answer[0] == 'j' || answer[0] == 'J') {
        /* Vor dem fgets eventuell verbliebene newline entfernen */
        int ch;
        while ((ch = getchar()) != '\n' && ch != EOF) { }

        char user[BUF_SIZE];
        printf("Benutzername (leer = aktueller Benutzer): ");
        if (!fgets(user, sizeof(user), stdin)) {
            fprintf(stderr, "Fehler beim Lesen des Benutzernamens.\n");
            return 1;
        }
        /* Entferne newline */
        size_t len = strlen(user);
        if (len > 0 && user[len-1] == '\n') user[len-1] = '\0';

        char target[BUF_SIZE * 2];
        if (user[0] == '\0') {
            /* Kein Nutzer angegeben */
            strncpy(target, ip, sizeof(target));
            target[sizeof(target)-1] = '\0';
        } else {
            snprintf(target, sizeof(target), "%s@%s", user, ip);
        }

        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port);

        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
            return 1;
        }

        if (pid == 0) {
            /* Kindprozess: exec ssh */
            execlp("ssh", "ssh", "-p", port_str, target, (char *)NULL);
            /* Wenn exec fehlschlägt */
            fprintf(stderr, "Fehler beim Ausführen von ssh: %s\n", strerror(errno));
            _exit(127);
        } else {
            /* Elternprozess: warte auf Kind */
            int status = 0;
            if (waitpid(pid, &status, 0) == -1) {
                perror("waitpid");
                return 1;
            }
            if (WIFEXITED(status)) {
                printf("SSH beendet mit Status %d\n", WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                printf("SSH wurde durch Signal %d beendet\n", WTERMSIG(status));
            }
        }
    } else {
        printf("SSH-Start übersprungen.\n");
    }

    return 0;
}
