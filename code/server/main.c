#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <libgen.h>
#include <linux/limits.h>
#include <errno.h>

#define ACCOUNTS_DIR "/userdata/accounts"

// Base RootFS im Projektordner
#define VM_BASE_DIR "/vm/base"
#define VM_BASE_IMG "/vm/base/debian-rootfs.img"
#define VM_BASE_QCOW "/vm/base/debian.qcow2"

#define KERNEL_PATH "/userdata/kernel/vmlinuz"
#define INITRD_PATH "/userdata/kernel/initrd.img"

// Debian Cloud-Image Downloadlink
#define DEBIAN_CLOUD_URL "https://cloudfront.debian.net/cdimage/cloud/bookworm/20250316-2053/debian-12-genericcloud-amd64-20250316-2053.qcow2"

// ------------------------------
// List all accounts
// ------------------------------
void listAccounts() {
    struct dirent *entry;
    DIR *dp = opendir(ACCOUNTS_DIR);
    if (!dp) {
        printf("No accounts found.\n");
        return;
    }

    int index = 1;
    printf("Available accounts:\n");

    while ((entry = readdir(dp))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", ACCOUNTS_DIR, entry->d_name);

        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            printf("%d) %s\n", index++, entry->d_name);
        }
    }

    closedir(dp);
}

// ------------------------------
// Show help
// ------------------------------
void showHelp() {
    printf("\nAvailable commands:\n");
    printf("startvm    - Start a VM\n");
    printf("stopvm     - Stop all VMs\n");
    printf("list       - List accounts\n");
    printf("createuser - Create new account\n");
    printf("help       - Show this help\n");
    printf("exit       - Exit terminal\n\n");
}

// ------------------------------
// Ensure Base RootFS exists
// ------------------------------
int ensureBaseImage() {
    struct stat st;
    if (stat(VM_BASE_IMG, &st) == 0) {
        return 1; // already exists
    }

    printf("Base RootFS not found. Downloading...\n");

    // VM_BASE_DIR erstellen falls nicht existiert
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", VM_BASE_DIR);
    system(cmd);

    // Download
    snprintf(cmd, sizeof(cmd), "wget -O %s %s", VM_BASE_QCOW, DEBIAN_CLOUD_URL);
    if (system(cmd) != 0) {
        printf("Error: Failed to download Base RootFS from '%s'\n", DEBIAN_CLOUD_URL);
        return 0;
    }

    // Konvertieren zu RAW
    snprintf(cmd, sizeof(cmd), "qemu-img convert -O raw %s %s", VM_BASE_QCOW, VM_BASE_IMG);
    if (system(cmd) != 0) {
        printf("Error: Failed to convert QCOW2 to RAW.\n");
        return 0;
    }

    printf("Base RootFS ready at '%s'\n", VM_BASE_IMG);
    return 1;
}

// ------------------------------
// Create new user account
// ------------------------------
void createUser() {
    if (!ensureBaseImage()) return;

    char name[50];
    printf("Enter new account name: ");
    fgets(name, sizeof(name), stdin);
    name[strcspn(name, "\n")] = 0;  // newline entfernen

    char accountPath[256];
    snprintf(accountPath, sizeof(accountPath), "%s/%s", ACCOUNTS_DIR, name);

    DIR *dir = opendir(accountPath);
    if (dir) {
        printf("Error: Account '%s' already exists!\n", name);
        closedir(dir);
        return;
    }

    // Ordner für VM rootfs erstellen
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s/rootfs", accountPath);
    if (system(cmd) != 0) {
        printf("Error: Failed to create directory for account '%s'\n", name);
        return;
    }

    // RootFS kopieren
    snprintf(cmd, sizeof(cmd), "cp %s %s/rootfs.img", VM_BASE_IMG, accountPath);
    if (system(cmd) != 0) {
        printf("Error: Failed to copy Base-Image for '%s'\n", name);
        return;
    }

    // Benutzer-Datei anlegen
    char userFile[512];
    snprintf(userFile, sizeof(userFile), "%s/%s.txt", ACCOUNTS_DIR, name);
    FILE *f = fopen(userFile, "w");
    if (f) {
        fprintf(f, "username=%s\n", name); // später kann hier mehr gespeichert werden
        fclose(f);
    } else {
        printf("Warning: Failed to create user info file for '%s'\n", name);
    }

    printf("Account '%s' created successfully!\n", name);
}

// ------------------------------
// Select an existing account
// ------------------------------
int selectAccount(char *accountName) {
    listAccounts();
    printf("Enter account name: ");
    scanf("%s", accountName);
    getchar(); // consume leftover newline

    char path[256];
    snprintf(path, sizeof(path), "%s/%s/rootfs.img", ACCOUNTS_DIR, accountName);

    if (access(path, F_OK) != 0) {
        printf("Error: Account '%s' does not exist!\n", accountName);
        return 0;
    }

    return 1;
}

// ------------------------------
// Start a VM for a given account
// ------------------------------
void startVM() {
    if (!ensureBaseImage()) return;

    char accountName[50];
    if (!selectAccount(accountName))
        return;

    int port = 2200;
    for (int i = 0; accountName[i] != '\0'; i++) {
        port += accountName[i];
    }

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "qemu-system-x86_64 "
             "-m 512M "
             "-kernel %s "
             "-initrd %s "
             "-append \"console=ttyS0 root=/dev/ram0 rw\" "
             "-nographic "
             "-net user,hostfwd=tcp::%d-:22 -net nic "
             "-drive file=%s/%s/rootfs.img,format=raw,if=virtio "
             "&",
             KERNEL_PATH, INITRD_PATH, port, ACCOUNTS_DIR, accountName);

    if (system(cmd) != 0) {
        printf("Error: Failed to start VM for '%s'\n", accountName);
        return;
    }

    printf("VM for '%s' started on SSH port %d\n", accountName, port);
}

// ------------------------------
// Stop all VMs
// ------------------------------
void stopVM() {
    if (system("pkill qemu-system-x86_64") != 0) {
        printf("Error: Failed to stop VMs or none running.\n");
        return;
    }
    printf("All VMs stopped.\n");
}

// ------------------------------
// Menu
// ------------------------------
void menu() {
    char input[50];

    printf("For help type 'help'\n");

    while (1) {
        printf("\n> ");
        fgets(input, sizeof(input), stdin);
        input[strcspn(input, "\n")] = 0;  // newline entfernen

        if (strcmp(input, "startvm") == 0) startVM();
        else if (strcmp(input, "stopvm") == 0) stopVM();
        else if (strcmp(input, "list") == 0) listAccounts();
        else if (strcmp(input, "createuser") == 0) createUser();
        else if (strcmp(input, "help") == 0) showHelp();
        else if (strcmp(input, "exit") == 0) exit(0);
        else printf("Error: Unknown command '%s'. Type 'help' for available commands.\n", input);
    }
}

// ------------------------------
int main() {
    // Ordner für Accounts anlegen, falls nicht existiert
    system("mkdir -p /userdata/accounts");
    system("mkdir -p " VM_BASE_DIR);

    menu();
    return 0;
}
