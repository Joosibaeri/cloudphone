#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#define VM_BASE "./vm/base/debian-rootfs"
#define ACCOUNTS_DIR "./vm/accounts"
#define KERNEL_PATH "./vm/kernel/vmlinuz"
#define INITRD_PATH "./vm/kernel/initrd.img"

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
        // skip . and ..
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
// Create new VM account
// ------------------------------
void createAccount() {
    char name[50];
    printf("Enter new account name: ");
    scanf("%s", name);

    char accountPath[256];
    snprintf(accountPath, sizeof(accountPath), "%s/%s", ACCOUNTS_DIR, name);

    // Check if account exists
    DIR *dir = opendir(accountPath);
    if (dir) {
        printf("Account '%s' already exists!\n", name);
        closedir(dir);
        return;
    }

    // Make directories
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s/rootfs", accountPath);
    system(cmd);

    // Copy base rootfs
    snprintf(cmd, sizeof(cmd), "cp -r %s %s/rootfs", VM_BASE, accountPath);
    system(cmd);

    printf("Account '%s' created successfully!\n", name);
}

// ------------------------------
// Select an existing account
// ------------------------------
int selectAccount(char *accountName) {
    listAccounts();
    printf("Enter account name: ");
    scanf("%s", accountName);

    char path[256];
    snprintf(path, sizeof(path), "%s/%s/rootfs", ACCOUNTS_DIR, accountName);

    DIR *dir = opendir(path);
    if (!dir) {
        printf("Account '%s' does not exist!\n", accountName);
        return 0;
    }
    closedir(dir);
    return 1;
}

// ------------------------------
// Start a VM for a given account
// ------------------------------
void startVM() {
    char accountName[50];
    if (!selectAccount(accountName))
        return;

    // Simple deterministic SSH port based on account name
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

    printf("Starting VM for '%s' on SSH port %d...\n", accountName, port);
    system(cmd);
}

// ------------------------------
// Stop all VMs
// ------------------------------
void stopVMs() {
    printf("Stopping all running VMs...\n");
    system("pkill qemu-system-x86_64");
}

// ------------------------------
// Menu
// ------------------------------
void menu() {
    int choice;
    while (1) {
        printf("\nCloudPhone Server Terminal\n");
        printf("==========================\n");
        printf("1) Start VM\n");
        printf("2) Stop all VMs\n");
        printf("3) List accounts\n");
        printf("4) Create new account\n");
        printf("5) Exit\n");
        printf("Enter choice: ");
        scanf("%d", &choice);

        switch (choice) {
            case 1: startVM(); break;
            case 2: stopVMs(); break;
            case 3: listAccounts(); break;
            case 4: createAccount(); break;
            case 5: exit(0);
            default: printf("Unknown command.\n");
        }
    }
}

// ------------------------------
int main() {
    // Ensure account directory exists
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", ACCOUNTS_DIR);
    system(cmd);

    menu();
    return 0;
}

