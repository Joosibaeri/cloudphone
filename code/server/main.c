#include <stdio.h>
#include <stdlib.h>

void startServer() {
    printf("Server wird gestartet...\n");
    // hier später dein Start-Code
}

void stopServer() {
    printf("Server wird gestoppt...\n");
    // hier später dein Stop-Code
}

void showStatus() {
    printf("Server-Status: (hier später echter Status)\n");
}

int main() {
    int choice;

    while (1) {
        printf("\n=== Server Control Terminal ===\n");
        printf("1) Server starten\n");
        printf("2) Server stoppen\n");
        printf("3) Status anzeigen\n");
        printf("4) Beenden\n");
        printf("Auswahl: ");

        scanf("%d", &choice);

        switch (choice) {
            case 1:
                startServer();
                break;
            case 2:
                stopServer();
                break;
            case 3:
                showStatus();
                break;
            case 4:
                printf("Programm beendet.\n");
                return 0;
            default:
                printf("Ungültige Eingabe.\n");
        }
    }

    return 0;
}
