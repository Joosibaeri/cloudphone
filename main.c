#include <stdio.h>
#include <stdlib.h>

int main() {
    printf("CLOUDPHONE OS v0.1\n");
    printf("Booting... Terminal style!\n\n");
    printf("1) QR-Code scan\n2) Manual IP/Port\n");
    printf("Selection: ");
    int choice;
    scanf("%d", &choice);
    if(choice == 1) {
        printf("QR-Code scanning placeholder...\n");
    } else {
        printf("Manual input placeholder...\n");
    }
    // Hier später SSH-Verbindung starten
    return 0;
}
