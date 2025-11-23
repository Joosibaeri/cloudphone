#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <linux/limits.h>

#define ACCOUNTS_DIR "/userdata/accounts"
#define VM_BASE_QCOW2 "/vm/base/debian.qcow2"
#define VM_BASE_ROOTFS "/vm/base/debian-rootfs.img"
#define KERNEL_PATH "/userdata/kernel/vmlinuz"
#define INITRD_PATH "/userdata/kernel/initrd.img"

void flushInput() {
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}

void ensureBaseImage() {
    DIR *d = opendir("/vm/base");
    if (!d) system("mkdir -p /vm/base");
    else closedir(d);

    if (access(VM_BASE_QCOW2, F_OK) != 0) {
        printf("Base RootFS not found. Downloading...\n");
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
                 "wget -O %s https://cdimage.debian.org/cdimage/cloud/bookworm/latest/debian-12-genericcloud-amd64.qcow2",
                 VM_BASE_QCOW2);
        if (system(cmd) != 0) {
            printf("Error: Failed to download Base RootFS\n");
            exit(1);
        }
    }

    if (access(VM_BASE_ROOTFS, F_OK) != 0) {
        printf("Preparing Base RootFS...\n");
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "cp %s %s", VM_BASE_QCOW2, VM_BASE_ROOTFS);
        if (system(cmd) != 0) {
            printf("Error: Failed to prepare Base RootFS\n");
            exit(1);
        }
    }
}

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

void showHelp() {
    printf("\nAvailable commands:\n");
    printf("startvm     - Start a VM\n");
    printf("stopvm      - Stop all VMs\n");
    printf("listuser    - List accounts\n");
    printf("createuser  - Create new account\n");
    printf("removeuser  - Delete an account\n");
    printf("help        - Show this help\n");
    printf("exit        - Exit terminal\n\n");
    printf("checkuser   - Check if an account exists\n");
}

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
             "-kernel %s "
             "-initrd %s "
             "-append \"console=ttyS0 root=/dev/ram0 rw\" "
             "-nographic "
             "-net user,hostfwd=tcp::%d-:22 -net nic "
             "-drive file=%s,format=raw,if=virtio &",
             KERNEL_PATH, INITRD_PATH, port, rootfsPath);

    if (system(cmd) != 0) {
        printf("Error: Failed to start VM for '%s'\n", accountName);
        return;
    }

    printf("VM for '%s' started on SSH port %d\n", accountName, port);
}

void stopVM() {
    if (system("pkill qemu-system-x86_64") != 0) {
        printf("Error: Failed to stop VMs or none running.\n");
        return;
    }
    printf("All VMs stopped.\n");
}

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
        else if (!strcmp(input, "help")) showHelp();
        else if (!strcmp(input, "exit")) exit(0);
        else if (!strcmp(input, "checkuser")) checkUser();
        
        else if (strlen(input) == 0) continue;
        else printf("Error: Unknown command '%s'. Type 'help' for available commands.\n", input);
    }
}

int main() {
    system("mkdir -p /userdata/accounts");
    menu();
    return 0;
}
