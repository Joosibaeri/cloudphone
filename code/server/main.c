#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void startServer() {
    printf("Starting server...\n");
    // Führt die Datei "start" aus, die sich im gleichen Ordner befindet
    int ret = system("./start");
    if(ret == -1) {
        printf("Failed to execute './start'\n");
    }
}

void stopServer() {
    printf("Stopping server...\n");
    // Hier später der Stop-Code
}

void showStatus() {
    printf("Server status: (placeholder)\n");
}

int main() {
    char input[100];

    while (1) {
        printf("\n=== Server Control Terminal ===\n");
        printf("Commands:\n");
        printf("start  - Start the server\n");
        printf("stop   - Stop the server\n");
        printf("status - Show server status\n");
        printf("exit   - Exit the program\n");
        printf("Enter command: ");

        if(scanf("%99s", input) != 1) continue;

        if(strcmp(input, "start") == 0) {
            startServer();
        } else if(strcmp(input, "stop") == 0) {
            stopServer();
        } else if(strcmp(input, "status") == 0) {
            showStatus();
        } else if(strcmp(input, "exit") == 0) {
            printf("Exiting program.\n");
            break;
        } else {
            printf("Unknown command: '%s'\n", input);
        }
    }

    return 0;
}
