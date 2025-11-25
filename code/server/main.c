#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <linux/limits.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

extern int kill(pid_t pid, int sig);

#define ACCOUNTS_DIR "/userdata/accounts"
#define BASE_DIR        "/userdata/base"
#define VM_BASE_QCOW2   "/userdata/base/base.qcow2"

/* Prototypes */
void flushInput(void);
int selectAccount(char *accountName);
int ensureBaseImage(void);
int ensureAccountsFolder(void);
void listAccounts(void);
void createUser(void);
void removeUser(void);
void checkUser(void);
void userInfo(void);
void cloneUser(void);
void resetUser(void);
void rebuildBase(void);
void startVM(void);
void stopVM(void);
/* changeimg: download/change base qcow2 image */
void changeimg(void);
/* programmatic action prototypes used by API server */

/* API server state */
/* no API server state (removed automation API) */

/* parse a single-line command and write a reply into out */
/* automation API removed */
void showHelp(void);
void menu(void);

/* --- Helpers --- */
void flushInput(void) {
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}

int selectAccount(char *accountName) {
    listAccounts();
    printf("Enter account name: ");
    if (!fgets(accountName, PATH_MAX, stdin)) {
        return 0;
    }
    accountName[strcspn(accountName, "\n")] = 0;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", ACCOUNTS_DIR, accountName);
    DIR *dir = opendir(path);
    if (!dir) { printf("Error: Account '%s' does not exist!\n", accountName); return 0; }
    closedir(dir);
    return 1;
}

/* Ensure base directory and base image exist. Return 0 on success, -1 on error */
int ensureBaseImage(void) {
    DIR *d = opendir(BASE_DIR);
    if (!d) {
        if (mkdir(BASE_DIR, 0755) != 0 && errno != EEXIST) {
            perror("mkdir base");
            return -1;
        }
    } else closedir(d);

    if (access(VM_BASE_QCOW2, F_OK) != 0) {
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s/arch-cloudimg.qcow2", BASE_DIR);
        fprintf(stderr, "Base qcow2 not found at %s. Attempting download...\n", VM_BASE_QCOW2);
        char cmd[PATH_MAX * 2];
        int r = snprintf(cmd, sizeof(cmd), "wget -O '%s' https://ftp.fau.de/archlinux/images/latest/Arch-Linux-x86_64-cloudimg.qcow2", tmp);
        if (r < 0 || r >= (int)sizeof(cmd)) { fprintf(stderr, "URL too long for command buffer\n"); return -1; }
        if (system(cmd) != 0) { fprintf(stderr, "Failed to download base image\n"); return -1; }
        if (rename(tmp, VM_BASE_QCOW2) != 0) perror("rename base image");
    }
    return 0;
}

/* --- Account operations --- */
void listAccounts(void) {
    struct dirent *entry;
    DIR *dp = opendir(ACCOUNTS_DIR);
    if (!dp) { printf("No accounts found.\n"); return; }

    printf("Available accounts:\n");
    int idx = 1;
    while ((entry = readdir(dp))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", ACCOUNTS_DIR, entry->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) printf("%d) %s\n", idx++, entry->d_name);
    }
    closedir(dp);
}

/* Ensure accounts folder exists. Return 0 on success */
int ensureAccountsFolder(void) {
    DIR *d = opendir(ACCOUNTS_DIR);
    if (d) { closedir(d); return 0; }
    if (mkdir(ACCOUNTS_DIR, 0755) != 0 && errno != EEXIST) { perror("mkdir accounts"); return -1; }
    return 0;
}

/* minimal name validation: no slash and only safe characters */
static int validateName(const char *name) {
    if (!name || !*name) return 0;
    if (strchr(name, '/')) return 0;
    for (const char *p = name; *p; ++p) {
        unsigned char c = *p;
        if (!(c == '-' || c == '_' || c == '.' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return 0;
    }
    return 1;
}

/* copy file content; returns 0 on success */
static int copyFile(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); return -1; }
    char buf[8192];
    ssize_t r;
    while ((r = read(in, buf, sizeof(buf))) > 0) {
        ssize_t w = write(out, buf, (size_t)r);
        if (w != r) { close(in); close(out); return -1; }
    }
    close(in); close(out);
    return (r == 0) ? 0 : -1;
}

/* nftw callback to remove files/directories */
/* recursively remove a file/directory (depth first). returns 0 on success */
static int remove_recursive(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) { perror("stat"); return -1; }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) { perror("opendir"); return -1; }
        struct dirent *entry;
        int rc = 0;
        while ((entry = readdir(d))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            char child[PATH_MAX * 2];
            if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child)) { rc = -1; break; }
            if (remove_recursive(child) != 0) rc = -1;
        }
        closedir(d);
        if (rmdir(path) != 0) { perror("rmdir"); return -1; }
        return rc;
    }
    if (unlink(path) != 0) { perror("unlink"); return -1; }
    return 0;
}

void createUser(void) {
    if (ensureAccountsFolder() != 0) { printf("accounts folder missing and cannot be created\n"); return; }
    char name[128];
    printf("Enter new account name: ");
    if (!fgets(name, sizeof(name), stdin)) {
        return;
    }
    name[strcspn(name, "\n")] = 0;
    if (!validateName(name)) { printf("Invalid account name\n"); return; }
    char accountPath[PATH_MAX];
    if (snprintf(accountPath, sizeof(accountPath), "%s/%s", ACCOUNTS_DIR, name) >= (int)sizeof(accountPath)) { printf("Name too long\n"); return; }
    DIR *d = opendir(accountPath); if (d) { closedir(d); printf("Error: Account '%s' already exists!\n", name); return; }
    if (mkdir(accountPath, 0755) != 0 && errno != EEXIST) { perror("mkdir user"); return; }
    char diskPath[PATH_MAX]; if (snprintf(diskPath, sizeof(diskPath), "%s/disk.qcow2", accountPath) >= (int)sizeof(diskPath)) return;
    if (access(VM_BASE_QCOW2, F_OK) == 0) {
        if (copyFile(VM_BASE_QCOW2, diskPath) != 0) fprintf(stderr, "Warning: failed to copy base to %s\n", diskPath);
    }
    printf("Account '%s' created at %s\n", name, accountPath);
}

    

void removeUser(void) {
    char name[50];
    printf("Enter account name to delete: ");
    fgets(name, sizeof(name), stdin);
    name[strcspn(name, "\n")] = 0;

    char accountPath[PATH_MAX]; snprintf(accountPath, sizeof(accountPath), "%s/%s", ACCOUNTS_DIR, name);
    DIR *d = opendir(accountPath);
    if (!d) { printf("Error: Account '%s' does not exist!\n", name); return; }
    closedir(d);

    char confirm[8]; printf("Are you sure you want to delete '%s'? (y/n): ", name);
    fgets(confirm, sizeof(confirm), stdin); confirm[strcspn(confirm, "\n")] = 0;
    if (strcmp(confirm, "y") != 0 && strcmp(confirm, "Y") != 0) { printf("Aborted.\n"); return; }

    if (!validateName(name)) { printf("Invalid account name\n"); return; }
    if (remove_recursive(accountPath) != 0) { printf("Error: Failed to delete account '%s'\n", name); return; }
    printf("Account '%s' deleted.\n", name);
}

void checkUser(void) {
    char name[128]; printf("Enter account name to check: ");
    if (!fgets(name, sizeof(name), stdin)) return;
    name[strcspn(name, "\n")] = 0;
    char accountPath[PATH_MAX]; snprintf(accountPath, sizeof(accountPath), "%s/%s", ACCOUNTS_DIR, name);
    DIR *d = opendir(accountPath); if (d) { closedir(d); printf("Account '%s' exists.\n", name); } else printf("Account '%s' does not exist.\n", name);
}

void userInfo(void) {
    char name[128]; printf("Enter account name: ");
    if (!fgets(name, sizeof(name), stdin)) return;
    name[strcspn(name, "\n")] = 0;
    char accountPath[PATH_MAX]; snprintf(accountPath, sizeof(accountPath), "%s/%s", ACCOUNTS_DIR, name);
    DIR *d = opendir(accountPath); if (!d) { printf("Account '%s' does not exist.\n", name); return; } closedir(d);
    char disk[PATH_MAX * 2];
    if (snprintf(disk, sizeof(disk), "%s/disk.qcow2", accountPath) >= (int)sizeof(disk)) { printf("Internal path too long\n"); return; }
    int port = 2200; for (int i = 0; name[i]; i++) port += (unsigned char)name[i];
    printf("User: %s\nDisk: %s\nSSH Port: %d\nDisk exists: %s\n", name, disk, port, access(disk, F_OK) == 0 ? "yes" : "no");
}

void cloneUser(void) {
    char src[128], dest[128]; printf("Enter source user: ");
    if (!fgets(src, sizeof(src), stdin)) return;
    src[strcspn(src, "\n")] = 0;
    printf("Enter new user name: ");
    if (!fgets(dest, sizeof(dest), stdin)) return;
    dest[strcspn(dest, "\n")] = 0;
    if (!validateName(dest) || !validateName(src)) { printf("Invalid user name\n"); return; }
    char srcPath[PATH_MAX], destPath[PATH_MAX]; snprintf(srcPath, sizeof(srcPath), "%s/%s", ACCOUNTS_DIR, src); snprintf(destPath, sizeof(destPath), "%s/%s", ACCOUNTS_DIR, dest);
    DIR *d = opendir(srcPath); if (!d) { printf("Source user does not exist.\n"); return; } closedir(d);
    d = opendir(destPath); if (d) { closedir(d); printf("Destination user already exists.\n"); return; }
    char cmd[PATH_MAX * 2];
    if (snprintf(cmd, sizeof(cmd), "cp -a -- '%s' '%s'", srcPath, destPath) >= (int)sizeof(cmd)) { printf("Static buffer overflow\n"); return; }
    system(cmd);
    printf("User '%s' cloned to '%s'.\n", src, dest);
}

void resetUser(void) {
    char name[128]; printf("Enter account to reset: ");
    if (!fgets(name, sizeof(name), stdin)) return;
    name[strcspn(name, "\n")] = 0;
    char disk[PATH_MAX]; snprintf(disk, sizeof(disk), "%s/%s/disk.qcow2", ACCOUNTS_DIR, name);
    if (access(disk, F_OK) == 0) { if (unlink(disk) != 0) perror("unlink"); }
    printf("Copying fresh base qcow2...\n"); if (copyFile(VM_BASE_QCOW2, disk) != 0) fprintf(stderr, "Failed to copy base -> %s\n", disk);
    printf("User '%s' was reset.\n", name);
}

void rebuildBase(void) {
    printf("Rebuilding base image...\n");
    if (access(VM_BASE_QCOW2, F_OK) == 0) { if (unlink(VM_BASE_QCOW2) != 0) perror("unlink base"); }
    ensureBaseImage();
    printf("Base image rebuilt.\n");
}

/* changeimg - prompt for a URL and download a new base qcow2 into BASE_DIR/base.qcow2
   Simple safety checks: only accepts http(s) URLs and rejects single-quote characters
 */
void changeimg(void) {
    char url[2048];
    printf("Enter image URL (http(s)://...): ");
    if (!fgets(url, sizeof(url), stdin)) return;
    url[strcspn(url, "\n")] = 0;
    if (!url[0]) { printf("No URL provided\n"); return; }
    if (!(strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0)) {
        printf("Only http:// or https:// URLs are supported\n");
        return;
    }
    if (strchr(url, '\'') != NULL) { printf("URL contains invalid character '\''\n"); return; }

    /* ensure base dir exists */
    if (mkdir(BASE_DIR, 0755) != 0 && errno != EEXIST) { perror("mkdir base"); return; }

    /* create a temp file and download into it */
    char tmpfile[PATH_MAX];
    if (snprintf(tmpfile, sizeof(tmpfile), "%s/baseimg.%d.tmp", BASE_DIR, (int)getpid()) >= (int)sizeof(tmpfile)) { printf("path too long\n"); return; }
    /* ensure no conflicting file; try to remove any stale file first */
    unlink(tmpfile);

    char cmd[PATH_MAX * 2];
    if (snprintf(cmd, sizeof(cmd), "wget -O '%s' '%s'", tmpfile, url) < 0) { fprintf(stderr, "command build failed\n"); unlink(tmpfile); return; }
    if (system(cmd) != 0) { fprintf(stderr, "Failed to download URL to %s\n", tmpfile); unlink(tmpfile); return; }

    struct stat st;
    if (stat(tmpfile, &st) != 0 || st.st_size == 0) { fprintf(stderr, "Downloaded file missing/empty\n"); unlink(tmpfile); return; }

    /* replace the canonical base image */
    if (access(VM_BASE_QCOW2, F_OK) == 0) {
        if (unlink(VM_BASE_QCOW2) != 0) perror("unlink old base");
    }
    if (rename(tmpfile, VM_BASE_QCOW2) != 0) { perror("rename base"); unlink(tmpfile); return; }
    printf("Base image updated to %s\n", VM_BASE_QCOW2);
}

/* --- VM operations --- */
void startVM(void) {
    if (ensureBaseImage() != 0) { printf("Base image missing and cannot be prepared\n"); return; }
    char accountName[128]; if (!selectAccount(accountName)) return;
    char diskPath[PATH_MAX]; snprintf(diskPath, sizeof(diskPath), "%s/%s/disk.qcow2", ACCOUNTS_DIR, accountName);

    if (access(diskPath, F_OK) != 0) {
        printf("Account '%s' disk.qcow2 not found. Copying base image...\n", accountName);
        if (copyFile(VM_BASE_QCOW2, diskPath) != 0) { perror("copyFile"); return; }
    }

    int port = 2200; for (int i = 0; accountName[i]; i++) port += (unsigned char)accountName[i];
    port = (port % 64512) + 1024;

    /* prepare logging and pid files inside account directory */
    char accountDir[PATH_MAX * 2]; if (snprintf(accountDir, sizeof(accountDir), "%s/%s", ACCOUNTS_DIR, accountName) >= (int)sizeof(accountDir)) { printf("Internal path too long\n"); return; }
    char userLog[PATH_MAX * 2]; if (snprintf(userLog, sizeof(userLog), "%s/vm.log", accountDir) >= (int)sizeof(userLog)) { printf("Internal path too long\n"); return; }
    char userPid[PATH_MAX * 2]; if (snprintf(userPid, sizeof(userPid), "%s/vm.pid", accountDir) >= (int)sizeof(userPid)) { printf("Internal path too long\n"); return; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return; }
    if (pid == 0) {
        /* child: run qemu */
        int fd = open(userLog, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
        char portarg[64]; snprintf(portarg, sizeof(portarg), "user,hostfwd=tcp::%d-:22", port);
        char drivearg[PATH_MAX + 64]; snprintf(drivearg, sizeof(drivearg), "file=%s,format=qcow2,if=virtio", diskPath);
        char *const argv[] = { "qemu-system-x86_64", "-m", "512M", "-nographic", "-net", portarg, "-net", "nic", "-drive", drivearg, NULL };
        execvp("qemu-system-x86_64", argv);
        perror("exec qemu"); _exit(127);
    }
    /* parent: write pid file */
    FILE *f = fopen(userPid, "w"); if (f) { fprintf(f, "%d\n", pid); fclose(f); }
    /* make sure log exists */
    int fd = open(userLog, O_CREAT | O_WRONLY | O_APPEND, 0644); if (fd >= 0) close(fd);
    printf("VM started: port=%d pid=%d disk=%s log=%s pidfile=%s\n", port, pid, diskPath, userLog, userPid);
}

void stopVM(void) {
    char accountName[128];
    if (!selectAccount(accountName)) return;
    char userPid[PATH_MAX]; snprintf(userPid, sizeof(userPid), "%s/%s/vm.pid", ACCOUNTS_DIR, accountName);
    FILE *f = fopen(userPid, "r");
    if (!f) { printf("No pidfile found for '%s'. Is VM running?\n", accountName); return; }
    int pid = 0; if (fscanf(f, "%d", &pid) != 1) { fclose(f); printf("Failed to read pidfile\n"); return; } fclose(f);
    if (kill((pid_t)pid, SIGTERM) != 0) { perror("kill"); return; }
    /* remove pidfile after stopping */
    if (unlink(userPid) != 0) perror("unlink pidfile");
    printf("Sent SIGTERM to pid %d for account '%s'\n", pid, accountName);
}

/* showHelp alphabetical */
void showHelp(void) {
    printf("\nAvailable commands:\n");
    printf("checkuser     - Check if an account exists\n");
    printf("cloneuser     - Clone an existing user\n");
    printf("createuser    - Create new account\n");
    printf("exit          - Exit terminal\n");
    printf("help          - Show this help\n");
    printf("listuser      - List accounts\n");
    printf("rebuildbase   - Redownload the base qcow2 image\n");
    printf("removeuser    - Delete an account\n");
    printf("resetuser     - Reset a user's disk.qcow2 from base\n");
    printf("startvm       - Start a VM\n");
    printf("stopvm        - Stop all VMs\n");
    printf("changeimg     - Download a new base QCOW2 from a URL (replace /userdata/base/base.qcow2)\n");
    printf("userinfo      - Show info about a user\n\n");
}

void menu(void) {
    char input[64];
    printf("For help type 'help'\n");
    while (1) {
        printf("\n> ");
        if (!fgets(input, sizeof(input), stdin)) break;
        input[strcspn(input, "\n")] = '\0';

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
        else if (!strcmp(input, "changeimg")) changeimg();
        else if (!strcmp(input, "exit")) exit(0);
        else if (strlen(input) == 0) continue;
        else printf("Error: Unknown command '%s'. Type 'help' for available commands.\n", input);
    }
}

int main(void) {
    /* automation API removed; starting main CLI only */
    if (ensureAccountsFolder() != 0) {
        fprintf(stderr, "Failed to create accounts directory\n");
        return 1;
    }
    if (ensureBaseImage() != 0) fprintf(stderr, "Warning: base image not available\n");
    menu();
    /* exiting */
    return 0;
}
