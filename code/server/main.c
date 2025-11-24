#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <linux/limits.h>

#define ACCOUNTS_DIR "/userdata/accounts"
#define VM_BASE_QCOW2 "/vm/base/arch.qcow2"
#define VM_BASE_ROOTFS "/vm/base/arch-rootfs.img"
#define KERNEL_PATH "/userdata/kernel/vmlinuz"
#define INITRD_PATH "/userdata/kernel/initrd.img"

// ----------------------------------
// Input flush
// ----------------------------------
void flushInput() {
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}

// ----------------------------------
// Ensure Base Image Exists
// ----------------------------------
void ensureBaseImage() {
    DIR *d = opendir("/vm/base");
    if (!d) system("mkdir -p /vm/base");
    else closedir(d);

    if (access(VM_BASE_QCOW2, F_OK) != 0) {
        printf("Base Arch RootFS not found. Downloading...\n");
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
                 "wget -O %s https://ftp.fau.de/archlinux/images/latest/Arch-Linux-x86_64-cloudimg.qcow2",
                 VM_BASE_QCOW2);
        if (system(cmd) != 0) {
            printf("Error: Failed to download Arch Base RootFS\n");
            exit(1);
        }
    }

    if (access(VM_BASE_ROOTFS, F_OK) != 0) {
        printf("Preparing Base RootFS...\n");
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "cp %s %s", VM_BASE_QCOW2, VM_BASE_ROOTFS);
        if (system(cmd) != 0) {
            printf("Error: Failed to prepare Arch Base RootFS\n");
            exit(1);
        }
    }
}

// ----------------------------------
// List Accounts
// ----------------------------------
void listAccounts() {
    struct dirent *entry;
    DIR *dp = opendir(ACCOUNTS_DIR);
    if (!dp) {
        printf("No accounts found.\n");
        return;
    }

    printf("Available accounts:\n");
    int index = 1;
    while ((entry = readdir(dp))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", ACCOUNTS_DIR, entry->d_name);

        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            printf("%d) %s\n", index++, entry->d_name);
    }

    closedir(dp);
}

// ----------------------------------
// Help
// ----------------------------------
void showHelp() {
    printf("\nAvailable commands:\n");
    printf("startvm       - Start a VM\n");
    printf("stopvm        - Stop all VMs\n");
    printf("listuser      - List accounts\n");
    printf("createuser    - Create new account\n");
    printf("removeuser    - Delete an account\n");
    printf("checkuser     - Check if an account exists\n");
    printf("userinfo      - Show info about a user\n");
    printf("cloneuser     - Clone an existing user\n");
    printf("resetuser     - Reset a user's rootfs\n");
    printf("rebuildbase   - Redownload the base rootfs\n");
    printf("help          - Show this help\n");
    printf("exit          - Exit terminal\n\n");
}

// ----------------------------------
// Create User
// ----------------------------------
void createUser() {
    char name[50];
    printf("Enter new account name: ");
    fgets(name, sizeof(name), stdin);
    name[strcspn(name, "\n")] = 0;

    char accountPath[256];
    snprintf(accountPath, sizeof(accountPath), "%s/%s", ACCOUNTS_DIR, name);

    DIR *dir = opendir(accountPath);
    if (dir) {
        closedir(dir);
        printf("Error: Account '%s' already exists!\n", name);
        return;
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", accountPath);

    if (system(cmd) != 0) {
        printf("Error: Failed to create account directory\n");
        return;
    }

    printf("Account '%s' created.\n", name);
}

// ----------------------------------
// Remove User
// ----------------------------------
void removeUser() {
    char name[50];
    printf("Enter account name to delete: ");
    scanf("%49s", name);
    flushInput();

    char accountPath[256];
    snprintf(accountPath, sizeof(accountPath), "%s/%s", ACCOUNTS_DIR, name);

    DIR *dir = opendir(accountPath);
    if (!dir) {
        printf("Error: Account '%s' does not exist!\n", name);
        return;
    }
    closedir(dir);

    char confirm[4];
    printf("Are you sure you want to delete '%s'? (y/n): ", name);
    scanf("%3s", confirm);
    flushInput();

    if (strcmp(confirm, "y") != 0) {
        printf("Aborted.\n");
        return;
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", accountPath);
    if (system(cmd) != 0) {
        printf("Error: Failed to delete account '%s'\n", name);
        return;
    }

    printf("Account '%s' deleted.\n", name);
}

// ----------------------------------
// Check User
// ----------------------------------
void checkUser() {
    char name[50];
    printf("Enter account name to check: ");
    scanf("%49s", name);
    flushInput();

    char accountPath[256];
    snprintf(accountPath, sizeof(accountPath), "%s/%s", ACCOUNTS_DIR, name);

    DIR *dir = opendir(accountPath);
    if (dir) {
        closedir(dir);
        printf("Account '%s' exists.\n", name);
    } else {
        printf("Account '%s' does not exist.\n", name);
    }
}

// ----------------------------------
// User Info
// ----------------------------------
void userInfo() {
    char name[50];
    printf("Enter account name: ");
    scanf("%49s", name);
    flushInput();

    char accountPath[PATH_MAX];
    snprintf(accountPath, sizeof(accountPath), "%s/%s", ACCOUNTS_DIR, name);

    DIR *dir = opendir(accountPath);
    if (!dir) {
        printf("Account '%s' does not exist.\n", name);
        return;
    }
    closedir(dir);

    char rootfs[PATH_MAX];
    snprintf(rootfs, sizeof(rootfs), "%s/rootfs.img", accountPath);

    int port = 2200;
    for (int i = 0; name[i]; i++) port += name[i];

    printf("User: %s\n", name);
    printf("RootFS: %s\n", rootfs);
    printf("SSH Port: %d\n", port);
    printf("RootFS exists: %s\n", access(rootfs, F_OK) == 0 ? "yes" : "no");
}

// ----------------------------------
// Clone User
// ----------------------------------
void cloneUser() {
    char src[50], dest[50];

    printf("Enter source user: ");
    scanf("%49s", src);
    flushInput();

    printf("Enter new user name: ");
    scanf("%49s", dest);
    flushInput();

    char srcPath[PATH_MAX], destPath[PATH_MAX];
    snprintf(srcPath, sizeof(srcPath), "%s/%s", ACCOUNTS_DIR, src);
    snprintf(destPath, sizeof(destPath), "%s/%s", ACCOUNTS_DIR, dest);

    DIR *dir = opendir(srcPath);
    if (!dir) {
        printf("Source user does not exist.\n");
        return;
    }
    closedir(dir);

    dir = opendir(destPath);
    if (dir) {
        closedir(dir);
        printf("Destination user already exists.\n");
        return;
    }

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "cp -r %s %s", srcPath, destPath);
    system(cmd);

    printf("User '%s' cloned to '%s'.\n", src, dest);
}

// ----------------------------------
// Reset User
// ----------------------------------
void resetUser() {
    char name[50];
    printf("Enter account to reset: ");
    scanf("%49s", name);
    flushInput();

    char rootfs[PATH_MAX];
    snprintf(rootfs, sizeof(rootfs), "%s/%s/rootfs.img", ACCOUNTS_DIR, name);

    if (access(rootfs, F_OK) == 0) {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "rm %s", rootfs);
        system(cmd);
    }

    printf("Copying fresh base rootfs...\n");
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cp %s %s", VM_BASE_ROOTFS, rootfs);
    system(cmd);

    printf("User '%s' was reset.\n", name);
}

// ----------------------------------
// Rebuild Base
// ----------------------------------
void rebuildBase() {
    printf("Rebuilding base image...\n");

    if (access(VM_BASE_QCOW2, F_OK) == 0) system("rm /vm/base/debian.qcow2");
    if (access(VM_BASE_ROOTFS, F_OK) == 0) system("rm /vm/base/debian-rootfs.img");

    ensureBaseImage();

    printf("Base image rebuilt.\n");
}

// ----------------------------------
// Select Account
// ----------------------------------
int selectAccount(char *accountName) {
    listAccounts();
    printf("Enter account name: ");
    scanf("%49s", accountName);
    flushInput();

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", ACCOUNTS_DIR, accountName);

    DIR *dir = opendir(path);
    if (!dir) {
        printf("Error: Account '%s' does not exist!\n", accountName);
        return 0;
    }
    closedir(dir);
    return 1;
}

// ----------------------------------
// Start VM
// ----------------------------------
void startVM() {
    ensureBaseImage();

    char accountName[50];
    if (!selectAccount(accountName)) return;

    char rootfsPath[PATH_MAX];
    snprintf(rootfsPath, sizeof(rootfsPath), "%s/%s/rootfs.img", ACCOUNTS_DIR, accountName);

    if (access(rootfsPath, F_OK) != 0) {
        printf("Account '%s' rootfs not found. Copying Base-Image...\n", accountName);
        char cmd[PATH_MAX * 2];
        snprintf(cmd, sizeof(cmd), "cp %s %s", VM_BASE_ROOTFS, rootfsPath);
        if (system(cmd) != 0) {
            printf("Error: Failed to copy base rootfs for '%s'\n", accountName);
            return;
        }
    }

    int port = 2200;
    for (int i = 0; accountName[i]; i++) port += accountName[i];

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "qemu-system-x86_64 "
             "-m 512M "
             "-boot c "
             "-nographic "
             "-net user,hostfwd=tcp::%d-:22 -net nic "
             "-drive file=%s,format=qcow2,if=virtio &",
             port, rootfsPath);

    if (system(cmd) != 0) {
        printf("Error: Failed to start VM for '%s'\n", accountName);
        return;
    }

    printf("VM for '%s' started on SSH port %d\n", accountName, port);
}

// ----------------------------------
// Stop VM
// ----------------------------------
void stopVM() {
    if (system("pkill qemu-system-x86_64") != 0) {
        printf("Error: Failed to stop VMs or none running.\n");
        return;
    }
    printf("All VMs stopped.\n");
}

// ----------------------------------
// Menu
// ----------------------------------
void menu() {
    char input[50];
    printf("For help type 'help'\n");

    while (1) {
        printf("\n> ");
        fgets(input, sizeof(input), stdin);
        input[strcspn(input, "\n")] = 0;

        if (!strcmp(input, "startvm")) startVM();
        else if (!strcmp(input, "stopvm")) stopVM();
        else if (!strcmp(input, "listuser")) listAccounts();
        else if (!strcmp(input, "createuser")) createUser();
        else if (!strcmp(input, "removeuser")) removeUser();
        else if (!strcmp(input, "checkuser")) checkUser();
        else if (!strcmp(input, "userinfo")) userInfo();
        else if (!strcmp(input, "cloneuser")) cloneUser();
        else if (!strcmp(input, "resetuser")) resetUser();
        else if (!strcmp(input, "rebuildbase")) rebuildBase();
        else if (!strcmp(input, "help")) showHelp();
        else if (!strcmp(input, "exit")) exit(0);
        else if (strlen(input) == 0) continue;
        else printf("Error: Unknown command '%s'. Type 'help' for available commands.\n", input);
    }
}

// ----------------------------------
// Main
// ----------------------------------
int main() {
    system("mkdir -p /userdata/accounts");
    menu();
    return 0;
}
