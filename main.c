#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <limits.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/un.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <ctype.h>
#include <strings.h>
#include <stdint.h>
#include <time.h>

/*
 * Small CLI to manage per-user QEMU VMs and their camera bridge.
 * Responsibilities:
 * - Prepare a shared base qcow2 image (download + provisioning stamp)
 * - Manage per-account data in ./vm/userdata/accounts/<name>
 * - Start/stop VMs with SSH port forwarding and camera MJPEG bridge
 * - Provide a REPL with helper commands (create, clone, reset, delete)
 */

#define ACCOUNTS_DIR "./vm/userdata/accounts"   /* per-account folders */
#define BASE_DIR "./vm"                          /* shared VM assets */
#define VM_BASE_QCOW2 "./vm/base.qcow2"         /* base qcow2 image */
#define VM_LAUNCH_BIN "./vm/launch"             /* helper binary to copy */
#define VM_PROVISIONER "./vm/provision_base.sh"  /* script that provisions base */
#define SSH_PORT_FILE "ssh.port"
#define SSH_V6_PID_FILE "sshv6.pid"
#define SSH_V6_LOG_NAME "sshv6.log"
#define CAMERA_OUT_NAME "camera.mjpg"
#define CAMERA_LOG_NAME "cam.log"
#define CAMERA_PID_NAME "cam.pid"
#define CAMERA_PORT_FILE "camera.port"
#define CONFIG_PATH "./config.cfg"

#define DEFAULT_BASE_IMAGE_URL "https://dl.rockylinux.org/pub/rocky/9/images/x86_64/Rocky-9-GenericCloud.latest.x86_64.qcow2"
#define DEFAULT_IP_MODE "ipv6"
#define DEFAULT_NETWORK_MODE "bridge"
#define DEFAULT_BRIDGE_NAME "br0"

enum ip_mode { IP_MODE_IPV6 = 0, IP_MODE_IPV4 = 1 };
enum network_mode { NET_MODE_USER = 0, NET_MODE_BRIDGE = 1 };

struct Config {
    char base_image_url[PATH_MAX];
    enum ip_mode ip_mode;
    enum network_mode network_mode;
    char bridge_name[128];
};

static struct Config g_cfg = { DEFAULT_BASE_IMAGE_URL, IP_MODE_IPV6, NET_MODE_BRIDGE, DEFAULT_BRIDGE_NAME };

/* core commands */
int selectAccount(char *accountName);
int ensureBaseImage(void);
int ensureBaseProvisioned(void);
int ensureAccountsFolder(void);
void listAccounts(void);
void createUser(void);
void removeUser(void);
void checkUser(void);
void userInfo(void);
void cloneUser(void);
void resetUser(void);
void rebuildBase(void);
pid_t startCameraBridge(const char *accountDir, int preferredStartPort, int *outPort);
pid_t startIPv6Forward(const char *accountDir, int port);
void startVM(void);
void stopVM(void);
 
int deployLaunchBinary(const char *accountDir);
void stopCameraBridge(const char *accountDir);
void stopIPv6Forward(const char *accountDir);

/* helpers */
int findFreePortFrom(int startPort);
int fetchPublicIPIfconfigMe(char *out, size_t outSize);
int buildVmMacForAccount(const char *accountName, char *out, size_t outSize);
int findVmIpByMacOnBridge(const char *bridgeName, const char *mac, char *outIp, size_t outIpSize, int retries, int delayMs);
int findVmIpViaQga(const char *qgaSockPath, char *outIp, size_t outIpSize, int retries, int delayMs);
static void primeBridgeNeighborTable(const char *bridgeName);
static int bridge_exists(const char *bridgeName);
static int qemu_bridge_allowed(const char *bridgeName);
static int find_first_bridge(char *out, size_t outSize);
 
void showHelp(void);
void menu(void);
void loadConfig(void);

/* ask confirmation from stdin; empty input counts as YES */
static int ask_yes_default_yes(const char *prompt) {
    char buf[32];
    printf("%s", prompt);
    if (!fgets(buf, sizeof(buf), stdin)) return 0;
    buf[strcspn(buf, "\n")] = '\0';
    if (buf[0] == '\0') return 1; /* default yes */
    if (buf[0] == 'y' || buf[0] == 'Y') return 1;
    return 0;
}

void showServerIP(void);

/* pidfile helpers */
static pid_t pidfile_read(const char *path);
static int process_is_running(pid_t pid);
static void trim_trailing_ws(char *s);
static void trim_leading_ws(char **p);
static void sleep_ms(int ms);

/* recursively create a directory path (mkdir -p behavior) */
static int ensure_dir(const char *path) {
    if (!path || !*path) { errno = EINVAL; return -1; }
    char tmp[PATH_MAX];
    size_t len = snprintf(tmp, sizeof(tmp), "%s", path);
    if (len == 0 || len >= sizeof(tmp)) { errno = ENAMETOOLONG; return -1; }
    while (len > 1 && tmp[len-1] == '/') { tmp[len-1] = '\0'; --len; }
    for (char *p = tmp + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0) {
            if (errno != EEXIST) return -1;
        }
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0) {
        if (errno != EEXIST) return -1;
    }
    return 0;
}

/* drop trailing whitespace (including newlines) in-place */
static void trim_trailing_ws(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        --len;
    }
}

/* advance pointer past leading whitespace */
static void trim_leading_ws(char **p) {
    if (!p || !*p) return;
    while (**p && isspace((unsigned char)**p)) (*p)++;
}

/* sleep helper in milliseconds (POSIX-safe with nanosleep) */
static void sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/* read pid from a pidfile; returns 0 on error */
static pid_t pidfile_read(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    long p = 0;
    if (fscanf(f, "%ld", &p) != 1) p = 0;
    fclose(f);
    if (p <= 0) return 0;
    return (pid_t)p;
}

/* check if a pid is alive, treating EPERM as alive */
static int process_is_running(pid_t pid) {
    if (pid <= 0) return 0;
    if (kill(pid, 0) == 0) return 1;
    if (errno == EPERM) return 1;
    return 0;
}

/* prompt user for an account name and verify it exists */
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
    if (!dir) { printf("Error: account '%s' does not exist\n", accountName); return 0; }
    closedir(dir);
    return 1;
}

 
/*
 * Ensure a base qcow2 exists at VM_BASE_QCOW2.
 * - mkdir -p ./vm
 * - download the current cloud image if missing
 * - run provisioning once (stamp file prevents re-run)
 */
int ensureBaseImage(void) {
    if (ensure_dir(BASE_DIR) != 0) { perror("mkdir base"); return -1; }

    if (access(VM_BASE_QCOW2, F_OK) != 0) {
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s/base-cloudimg.qcow2", BASE_DIR);
        fprintf(stderr, "Base qcow2 not found at %s. Attempting download...\n", VM_BASE_QCOW2);
        const char *url = g_cfg.base_image_url[0] ? g_cfg.base_image_url : DEFAULT_BASE_IMAGE_URL;
        char cmd[PATH_MAX * 2];
        int r = snprintf(cmd, sizeof(cmd), "wget -q -O '%s' -o /dev/null '%s'", tmp, url);
        if (r < 0 || r >= (int)sizeof(cmd)) { fprintf(stderr, "URL too long for command buffer\n"); return -1; }
        if (system(cmd) != 0) { fprintf(stderr, "Failed to download base image\n"); return -1; }
        if (rename(tmp, VM_BASE_QCOW2) != 0) perror("rename base image");
    }
    return ensureBaseProvisioned();
}

/*
 * Run provisioning script against the base image exactly once.
 * A success stamp is written to BASE_PROVISION_STAMP to skip future runs.
 */
int ensureBaseProvisioned(void) {
    static int already_ran = 0;
    if (already_ran) return 0;

    if (access(VM_PROVISIONER, X_OK) != 0) {
        fprintf(stderr, "Provisioner script missing or not executable: %s\n", VM_PROVISIONER);
        return -1;
    }

    char cmd[PATH_MAX * 2];
    int r = snprintf(cmd, sizeof(cmd), "%s '%s'", VM_PROVISIONER, VM_BASE_QCOW2);
    if (r < 0 || r >= (int)sizeof(cmd)) {
        fprintf(stderr, "Provision command too long\n");
        return -1;
    }
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "Provisioner failed with code %d\n", rc);
        return -1;
    }
    already_ran = 1;
    return 0;
}

 
/* list existing account directories */
void listAccounts(void) {
    struct dirent *entry;
    DIR *dp = opendir(ACCOUNTS_DIR);
    if (!dp) { printf("No accounts found\n"); return; }
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

 
/* Create ./vm/userdata/accounts if needed (mkdir -p semantics). */
int ensureAccountsFolder(void) {
    if (ensure_dir(ACCOUNTS_DIR) != 0) { perror("mkdir accounts"); return -1; }
    return 0;
}

 
/* allow only safe account names (alnum + -_. and no slashes) */
static int validateName(const char *name) {
    if (!name || !*name) return 0;
    if (strchr(name, '/')) return 0;
    for (const char *p = name; *p; ++p) {
        unsigned char c = *p;
        if (!(c == '-' || c == '_' || c == '.' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return 0;
    }
    return 1;
}

 
/* copy a single file, overwriting destination */
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

 
/* rm -rf equivalent for files, dirs, and symlinks */
static int remove_recursive(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) { perror("lstat"); return -1; }
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

 
/* cp -a equivalent: recurse, preserve symlinks and modes */
static int copy_recursive(const char *src, const char *dst) {
    struct stat st;
    if (lstat(src, &st) != 0) { perror("lstat src"); return -1; }
    
    if (S_ISLNK(st.st_mode)) {
        char buf[PATH_MAX]; ssize_t r = readlink(src, buf, sizeof(buf)-1);
        if (r < 0) { perror("readlink"); return -1; }
        buf[r] = '\0';
        if (symlink(buf, dst) != 0) { perror("symlink"); return -1; }
        return 0;
    }
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, 0755) != 0 && errno != EEXIST) { perror("mkdir dst"); return -1; }
        DIR *d = opendir(src);
        if (!d) { perror("opendir src"); return -1; }
        struct dirent *entry;
        int rc = 0;
        while ((entry = readdir(d))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            char child_src[PATH_MAX * 2];
            char child_dst[PATH_MAX * 2];
            if (snprintf(child_src, sizeof(child_src), "%s/%s", src, entry->d_name) >= (int)sizeof(child_src)) { rc = -1; break; }
            if (snprintf(child_dst, sizeof(child_dst), "%s/%s", dst, entry->d_name) >= (int)sizeof(child_dst)) { rc = -1; break; }
            if (copy_recursive(child_src, child_dst) != 0) rc = -1;
        }
        closedir(d);
        return rc;
    }
    
    int in = open(src, O_RDONLY);
    if (in < 0) { perror("open src"); return -1; }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (out < 0) { perror("open dst"); close(in); return -1; }
    char buf[8192]; ssize_t r;
    while ((r = read(in, buf, sizeof(buf))) > 0) {
        ssize_t w = write(out, buf, (size_t)r);
        if (w != r) { perror("write"); close(in); close(out); return -1; }
    }
    close(in); close(out);
    return (r == 0) ? 0 : -1;
}

/* write an integer with trailing newline to a file */
static int read_int_file(const char *path, int *out) {
    if (!out) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int v = 0; int rc = fscanf(f, "%d", &v);
    fclose(f);
    if (rc == 1) { *out = v; return 0; }
    return -1;
}

static int write_int_file(const char *path, int value) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", value);
    ssize_t w = write(fd, buf, (size_t)len);
    close(fd);
    return (w == len) ? 0 : -1;
}

/* copy the launch helper binary into a user directory */
int deployLaunchBinary(const char *accountDir) {
    char dst[PATH_MAX];
    if (snprintf(dst, sizeof(dst), "%s/launch", accountDir) >= (int)sizeof(dst)) return -1;
    if (access(VM_LAUNCH_BIN, X_OK) != 0) {
        fprintf(stderr, "launch helper missing at %s\n", VM_LAUNCH_BIN);
        return -1;
    }
    if (copyFile(VM_LAUNCH_BIN, dst) != 0) {
        perror("copy launch helper");
        return -1;
    }
    chmod(dst, 0755);
    return 0;
}

/*
 * Start the per-account camera bridge via the bundled launch helper.
 * - Reuses an already-running bridge if pidfile is alive
 * - Picks a free TCP port (preferring preferredStartPort)
 * - Persists port/pid files next to the account
 * - Returns the child pid or -1 on failure
 */
pid_t startCameraBridge(const char *accountDir, int preferredStartPort, int *outPort) {
    char bin[PATH_MAX], out[PATH_MAX], logp[PATH_MAX], pidp[PATH_MAX], portfile[PATH_MAX];
    if (snprintf(bin, sizeof(bin), "%s/launch", accountDir) >= (int)sizeof(bin)) return -1;
    if (snprintf(out, sizeof(out), "%s/%s", accountDir, CAMERA_OUT_NAME) >= (int)sizeof(out)) return -1;
    if (snprintf(logp, sizeof(logp), "%s/%s", accountDir, CAMERA_LOG_NAME) >= (int)sizeof(logp)) return -1;
    if (snprintf(pidp, sizeof(pidp), "%s/%s", accountDir, CAMERA_PID_NAME) >= (int)sizeof(pidp)) return -1;
    if (snprintf(portfile, sizeof(portfile), "%s/%s", accountDir, CAMERA_PORT_FILE) >= (int)sizeof(portfile)) return -1;

    pid_t existing = pidfile_read(pidp);
    if (existing && process_is_running(existing)) {
        fprintf(stderr, "Camera bridge already running (pid=%d).\n", (int)existing);
        if (outPort) *outPort = -1;
        return existing;
    }
    unlink(pidp);

    if (deployLaunchBinary(accountDir) != 0) {
        fprintf(stderr, "Failed to deploy launch helper into %s\n", accountDir);
        return -1;
    }

    int port = findFreePortFrom(preferredStartPort);
    if (port <= 0) {
        fprintf(stderr, "No free port for camera bridge\n");
        return -1;
    }
    if (write_int_file(portfile, port) != 0) {
        fprintf(stderr, "Warning: failed to write camera port file\n");
    }
    if (outPort) *outPort = port;

    int pw[2];
    if (pipe(pw) != 0) { perror("pipe"); return -1; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); close(pw[0]); close(pw[1]); return -1; }

    if (pid == 0) {
        close(pw[0]);
        int flags = fcntl(pw[1], F_GETFD);
        if (flags != -1) fcntl(pw[1], F_SETFD, flags | FD_CLOEXEC);

        int fd = open(logp, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }

        char portStr[32]; snprintf(portStr, sizeof(portStr), "%d", port);
        char *const argv[] = { bin, "--camera-port", portStr, "--out", out, "--log", logp, "--pid-file", pidp, NULL };
        execv(bin, argv);

        int save_errno = errno;
        (void)write(pw[1], &save_errno, sizeof(save_errno));
        close(pw[1]);
        _exit(127);
    }

    close(pw[1]);
    int child_errno = 0;
    ssize_t r = read(pw[0], &child_errno, sizeof(child_errno));
    close(pw[0]);

    if (r == 0) {
        return pid;
    }
    if (r > 0) {
        int status = 0; waitpid(pid, &status, 0);
        fprintf(stderr, "Failed to start camera bridge: errno=%d\n", child_errno);
    } else {
        int saved = errno; fprintf(stderr, "Failed to start camera bridge: pipe read error: %s\n", strerror(saved));
        waitpid(pid, NULL, 0);
    }
    return -1;
}

/* stop camera bridge if running and clean pid/port files */
void stopCameraBridge(const char *accountDir) {
    char pidp[PATH_MAX], portfile[PATH_MAX];
    if (snprintf(pidp, sizeof(pidp), "%s/%s", accountDir, CAMERA_PID_NAME) >= (int)sizeof(pidp)) return;
    if (snprintf(portfile, sizeof(portfile), "%s/%s", accountDir, CAMERA_PORT_FILE) >= (int)sizeof(portfile)) return;
    pid_t pid = pidfile_read(pidp);
    if (pid && process_is_running(pid)) {
        kill(pid, SIGTERM);
    }
    unlink(pidp);
    unlink(portfile);
}

/* start IPv6 -> IPv4 SSH forwarder using socat (per-account) */
pid_t startIPv6Forward(const char *accountDir, int port) {
    if (g_cfg.ip_mode != IP_MODE_IPV6) return 0;
    char pidp[PATH_MAX], logp[PATH_MAX];
    if (snprintf(pidp, sizeof(pidp), "%s/%s", accountDir, SSH_V6_PID_FILE) >= (int)sizeof(pidp)) return -1;
    if (snprintf(logp, sizeof(logp), "%s/%s", accountDir, SSH_V6_LOG_NAME) >= (int)sizeof(logp)) return -1;

    pid_t existing = pidfile_read(pidp);
    if (existing && process_is_running(existing)) return existing;
    unlink(pidp);

    const char *socat_bin = NULL;
    if (access("/usr/bin/socat", X_OK) == 0) socat_bin = "/usr/bin/socat";
    else if (access("/usr/sbin/socat", X_OK) == 0) socat_bin = "/usr/sbin/socat";
    if (!socat_bin) {
        fprintf(stderr, "Warning: socat not found; IPv6 SSH forward not started\n");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        int fd = open(logp, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }

        char lspec[128];
        char rspec[128];
        snprintf(lspec, sizeof(lspec), "TCP6-LISTEN:%d,bind=[::],fork,reuseaddr,ipv6only=1", port);
        snprintf(rspec, sizeof(rspec), "TCP4:127.0.0.1:%d", port);

        char *const argv[] = { (char *)socat_bin, lspec, rspec, NULL };
        execv(socat_bin, argv);
        _exit(127);
    }

    FILE *f = fopen(pidp, "w");
    if (f) { fprintf(f, "%d\n", (int)pid); fclose(f); }
    return pid;
}

/* stop IPv6 forwarder if running */
void stopIPv6Forward(const char *accountDir) {
    char pidp[PATH_MAX];
    if (snprintf(pidp, sizeof(pidp), "%s/%s", accountDir, SSH_V6_PID_FILE) >= (int)sizeof(pidp)) return;
    pid_t pid = pidfile_read(pidp);
    if (pid && process_is_running(pid)) {
        kill(pid, SIGTERM);
    }
    unlink(pidp);
}

 
/* bind-scan for a free TCP port on all interfaces starting at startPort */
int findFreePortFrom(int startPort) {
    int s = socket(AF_INET6, SOCK_STREAM, 0);
    if (s < 0) return -1;

    int v6only = 0;
    (void)setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;

    if (startPort < 1) startPort = 1;
    for (int p = startPort; p <= 65535; ++p) {
        addr.sin6_port = htons(p);
        int b = bind(s, (struct sockaddr*)&addr, sizeof(addr));
        if (b == 0) {
            close(s);
            return p;
        }
    }
    close(s);
    return -1;
}

/* fetch public IP via plain HTTP from ifconfig.me/ip */
int fetchPublicIPIfconfigMe(char *out, size_t outSize) {
    if (!out || outSize < 8) return -1;
    out[0] = '\0';

    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo("ifconfig.me", "80", &hints, &res) != 0) return -1;

    int sock = -1;
    for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) continue;
        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    if (sock < 0) return -1;

    const char *req =
        "GET /ip HTTP/1.1\r\n"
        "Host: ifconfig.me\r\n"
        "User-Agent: cloudphone-server\r\n"
        "Connection: close\r\n\r\n";
    size_t reqLen = strlen(req);
    if (send(sock, req, reqLen, 0) != (ssize_t)reqLen) {
        close(sock);
        return -1;
    }

    char buf[4096];
    size_t total = 0;
    for (;;) {
        ssize_t n = recv(sock, buf + total, sizeof(buf) - 1 - total, 0);
        if (n < 0) {
            close(sock);
            return -1;
        }
        if (n == 0) break;
        total += (size_t)n;
        if (total >= sizeof(buf) - 1) break;
    }
    close(sock);
    buf[total] = '\0';

    char *body = strstr(buf, "\r\n\r\n");
    if (!body) return -1;
    body += 4;

    while (*body && isspace((unsigned char)*body)) body++;
    size_t len = strcspn(body, "\r\n");
    while (len > 0 && isspace((unsigned char)body[len - 1])) len--;
    if (len == 0 || len >= outSize) return -1;

    memcpy(out, body, len);
    out[len] = '\0';
    return 0;
}

/* build stable QEMU MAC from account name: 52:54:00:xx:xx:xx */
int buildVmMacForAccount(const char *accountName, char *out, size_t outSize) {
    if (!accountName || !*accountName || !out || outSize < 18) return -1;
    uint32_t hash = 2166136261u; /* FNV-1a */
    for (const unsigned char *p = (const unsigned char *)accountName; *p; ++p) {
        hash ^= (uint32_t)(*p);
        hash *= 16777619u;
    }
    int n = snprintf(
        out,
        outSize,
        "52:54:00:%02x:%02x:%02x",
        (unsigned int)((hash >> 16) & 0xff),
        (unsigned int)((hash >> 8) & 0xff),
        (unsigned int)(hash & 0xff)
    );
    return (n > 0 && (size_t)n < outSize) ? 0 : -1;
}

static int valid_ifname(const char *s) {
    if (!s || !*s) return 0;
    size_t len = strlen(s);
    if (len >= 64) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) {
            return 0;
        }
    }
    return 1;
}

static int bridge_exists(const char *bridgeName) {
    if (!valid_ifname(bridgeName)) return 0;
    return if_nametoindex(bridgeName) != 0;
}

static int qemu_bridge_allowed(const char *bridgeName) {
    if (!valid_ifname(bridgeName)) return 0;
    FILE *f = fopen("/etc/qemu/bridge.conf", "r");
    if (!f) return 0;
    char line[256];
    int allowed = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        trim_leading_ws(&p);
        trim_trailing_ws(p);
        if (*p == '\0' || *p == '#') continue;
        if (strncasecmp(p, "allow", 5) != 0) continue;
        p += 5;
        trim_leading_ws(&p);
        if (strcasecmp(p, "all") == 0 || strcmp(p, bridgeName) == 0) {
            allowed = 1;
            break;
        }
    }
    fclose(f);
    return allowed;
}

static int find_first_bridge(char *out, size_t outSize) {
    if (!out || outSize < 2) return -1;
    out[0] = '\0';

    DIR *d = opendir("/sys/class/net");
    if (!d) return -1;

    struct dirent *entry;
    while ((entry = readdir(d))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (!valid_ifname(entry->d_name)) continue;

        char marker[PATH_MAX];
        if (snprintf(marker, sizeof(marker), "/sys/class/net/%s/bridge", entry->d_name) >= (int)sizeof(marker)) continue;
        if (access(marker, F_OK) == 0) {
            int n = snprintf(out, outSize, "%s", entry->d_name);
            closedir(d);
            return (n > 0 && (size_t)n < outSize) ? 0 : -1;
        }
    }
    closedir(d);
    return -1;
}

static int probe_ipv4_host_port(const char *ip, int port, int timeoutMs) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc == 0) {
        close(fd);
        return 0;
    }
    if (errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    (void)select(fd + 1, NULL, &wfds, NULL, &tv);
    close(fd);
    return 0;
}

static void primeBridgeNeighborTable(const char *bridgeName) {
    if (!valid_ifname(bridgeName)) return;

    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) != 0) return;

    uint32_t host = 0;
    uint32_t mask = 0;
    int found = 0;
    for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || !ifa->ifa_netmask) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!ifa->ifa_name || strcmp(ifa->ifa_name, bridgeName) != 0) continue;

        struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
        struct sockaddr_in *msk = (struct sockaddr_in *)ifa->ifa_netmask;
        host = ntohl(sin->sin_addr.s_addr);
        mask = ntohl(msk->sin_addr.s_addr);
        found = 1;
        break;
    }
    freeifaddrs(ifaddr);
    if (!found) return;

    uint32_t network = host & mask;
    uint32_t broadcast = network | (~mask);
    if (broadcast <= network + 1) return;

    uint32_t hostCount = broadcast - network - 1;
    if (hostCount == 0 || hostCount > 1024) return;

    char ipbuf[32];
    for (uint32_t ip = network + 1; ip < broadcast; ++ip) {
        if (ip == host) continue;
        struct in_addr ia;
        ia.s_addr = htonl(ip);
        if (!inet_ntop(AF_INET, &ia, ipbuf, sizeof(ipbuf))) continue;
        (void)probe_ipv4_host_port(ipbuf, 22, 20);
    }
}

static int extract_non_loopback_ipv4(const char *text, char *outIp, size_t outIpSize) {
    if (!text || !outIp || outIpSize < 16) return -1;
    const char *p = text;
    while (*p) {
        if (!isdigit((unsigned char)*p)) { p++; continue; }

        unsigned int a, b, c, d;
        int consumed = 0;
        if (sscanf(p, "%3u.%3u.%3u.%3u%n", &a, &b, &c, &d, &consumed) == 4) {
            if (a <= 255 && b <= 255 && c <= 255 && d <= 255) {
                if (a != 127 && !(a == 169 && b == 254)) {
                    int n = snprintf(outIp, outIpSize, "%u.%u.%u.%u", a, b, c, d);
                    if (n > 0 && (size_t)n < outIpSize) return 0;
                    return -1;
                }
            }
            p += (consumed > 0 ? consumed : 1);
            continue;
        }
        p++;
    }
    return -1;
}

static ssize_t recv_once_with_timeout(int fd, char *buf, size_t cap, int timeoutMs) {
    if (!buf || cap == 0) return -1;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int s = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (s <= 0) return -1;
    return recv(fd, buf, cap, 0);
}

int findVmIpViaQga(const char *qgaSockPath, char *outIp, size_t outIpSize, int retries, int delayMs) {
    if (!qgaSockPath || !*qgaSockPath || !outIp || outIpSize < 16) return -1;
    outIp[0] = '\0';
    if (retries < 1) retries = 1;
    if (delayMs < 100) delayMs = 100;

    const char *cmd = "{\"execute\":\"guest-network-get-interfaces\"}\n";

    for (int attempt = 0; attempt < retries; ++attempt) {
        if (access(qgaSockPath, F_OK) != 0) {
            if (attempt + 1 < retries) sleep_ms(delayMs);
            continue;
        }

        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            if (attempt + 1 < retries) sleep_ms(delayMs);
            continue;
        }

        struct sockaddr_un sun;
        memset(&sun, 0, sizeof(sun));
        sun.sun_family = AF_UNIX;
        if (snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", qgaSockPath) >= (int)sizeof(sun.sun_path)) {
            close(fd);
            return -1;
        }

        if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) != 0) {
            close(fd);
            if (attempt + 1 < retries) sleep_ms(delayMs);
            continue;
        }

        char scratch[1024];
        (void)recv_once_with_timeout(fd, scratch, sizeof(scratch), 100);

        size_t cmdLen = strlen(cmd);
        if (send(fd, cmd, cmdLen, 0) != (ssize_t)cmdLen) {
            close(fd);
            if (attempt + 1 < retries) sleep_ms(delayMs);
            continue;
        }

        char resp[16384];
        size_t total = 0;
        for (int i = 0; i < 8 && total < sizeof(resp) - 1; ++i) {
            ssize_t n = recv_once_with_timeout(fd, resp + total, sizeof(resp) - 1 - total, 700);
            if (n <= 0) break;
            total += (size_t)n;
            if (strstr(resp, "\"return\"") && strchr(resp, ']')) break;
        }
        close(fd);
        resp[total] = '\0';

        if (total > 0 && extract_non_loopback_ipv4(resp, outIp, outIpSize) == 0) {
            return 0;
        }

        if (attempt + 1 < retries) sleep_ms(delayMs);
    }

    return -1;
}

/* try to resolve VM IP (IPv4 preferred, then IPv6) by MAC using neighbor table */
int findVmIpByMacOnBridge(const char *bridgeName, const char *mac, char *outIp, size_t outIpSize, int retries, int delayMs) {
    if (!bridgeName || !mac || !outIp || outIpSize < 8) return -1;
    if (!valid_ifname(bridgeName)) return -1;
    outIp[0] = '\0';
    if (retries < 1) retries = 1;
    if (delayMs < 50) delayMs = 50;

    char cmd4dev[256];
    char cmd4all[256];
    char cmd6dev[256];
    int c1 = snprintf(cmd4dev, sizeof(cmd4dev), "ip -4 neigh show dev %s 2>/dev/null", bridgeName);
    int c2 = snprintf(cmd4all, sizeof(cmd4all), "ip -4 neigh show 2>/dev/null");
    int c3 = snprintf(cmd6dev, sizeof(cmd6dev), "ip -6 neigh show dev %s 2>/dev/null", bridgeName);
    if (c1 <= 0 || c1 >= (int)sizeof(cmd4dev)) return -1;
    if (c2 <= 0 || c2 >= (int)sizeof(cmd4all)) return -1;
    if (c3 <= 0 || c3 >= (int)sizeof(cmd6dev)) return -1;

    for (int attempt = 0; attempt < retries; ++attempt) {
        const char *commands[] = { cmd4dev, cmd4all, cmd6dev };
        for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
            FILE *fp = popen(commands[i], "r");
            if (!fp) continue;

            char line[512];
            while (fgets(line, sizeof(line), fp)) {
                char ip[96] = {0};
                char dev[64] = {0};
                char lladdr[64] = {0};
                int m = sscanf(line, "%95s dev %63s lladdr %63s", ip, dev, lladdr);
                if (m >= 3 && strcasecmp(lladdr, mac) == 0) {
                    size_t len = strlen(ip);
                    if (len == 0) continue;

                    if (strchr(ip, ':') != NULL) {
                        if (strncasecmp(ip, "fe80", 4) == 0) {
                            int n = snprintf(outIp, outIpSize, "%s%%%s", ip, bridgeName);
                            if (n > 0 && (size_t)n < outIpSize) {
                                pclose(fp);
                                return 0;
                            }
                        } else if (len < outIpSize) {
                            memcpy(outIp, ip, len + 1);
                            pclose(fp);
                            return 0;
                        }
                    } else {
                        if (len < outIpSize) {
                            memcpy(outIp, ip, len + 1);
                            pclose(fp);
                            return 0;
                        }
                    }
                }
            }
            pclose(fp);
        }
        if (attempt == 2) {
            primeBridgeNeighborTable(bridgeName);
        }
        if (attempt + 1 < retries) sleep_ms(delayMs);
    }
    return -1;
}
/*
 * Create a new account directory with a fresh disk copy and helper binary.
 * The launch helper is copied so per-account processes can run locally.
 */
void createUser(void) {
    if (ensureAccountsFolder() != 0) { printf("accounts folder missing and cannot be created\n"); return; }
    char name[128];
    printf("Enter new account name: ");
    if (!fgets(name, sizeof(name), stdin)) {
        return;
    }
    name[strcspn(name, "\n")] = 0;
    int only_ws = 1;
    for (char *p = name; *p; ++p) {
        if (!isspace((unsigned char)*p)) { only_ws = 0; break; }
    }
    if (name[0] == '\0' || only_ws) { printf("No account name provided\n"); return; }
    if (!validateName(name)) { printf("Invalid account name\n"); return; }
    char accountPath[PATH_MAX];
    if (snprintf(accountPath, sizeof(accountPath), "%s/%s", ACCOUNTS_DIR, name) >= (int)sizeof(accountPath)) { printf("Name too long\n"); return; }
    DIR *d = opendir(accountPath); if (d) { closedir(d); printf("Error: Account '%s' already exists!\n", name); return; }
    if (mkdir(accountPath, 0755) != 0 && errno != EEXIST) { perror("mkdir user"); return; }
    char diskPath[PATH_MAX]; if (snprintf(diskPath, sizeof(diskPath), "%s/disk.qcow2", accountPath) >= (int)sizeof(diskPath)) return;
    if (access(VM_BASE_QCOW2, F_OK) == 0) {
        if (copyFile(VM_BASE_QCOW2, diskPath) != 0) fprintf(stderr, "Warning: failed to copy base to %s\n", diskPath);
    }
    if (deployLaunchBinary(accountPath) != 0) {
        fprintf(stderr, "Warning: launch helper not deployed to %s\n", accountPath);
    }
    printf("Account '%s' created at %s\n", name, accountPath);
}

/*
 * Delete an account after confirmation and only if its VM is not running.
 * Also removes per-account camera artifacts.
 */
void removeUser(void) {
    char name[50];
    printf("Enter account name to delete: ");
    if (!fgets(name, sizeof(name), stdin)) return;
    name[strcspn(name, "\n")] = 0;

    
    if (!validateName(name)) { printf("Invalid account name\n"); return; }

    char accountPath[PATH_MAX]; snprintf(accountPath, sizeof(accountPath), "%s/%s", ACCOUNTS_DIR, name);
    DIR *d = opendir(accountPath);
    if (!d) { printf("Error: Account '%s' does not exist!\n", name); return; }
    closedir(d);

    /* refuse to delete while VM is running */
    char userPid[PATH_MAX]; snprintf(userPid, sizeof(userPid), "%s/%s/vm.pid", ACCOUNTS_DIR, name);
    pid_t existing = pidfile_read(userPid);
    if (existing && process_is_running(existing)) {
        printf("Error: VM for '%s' appears to be running (pid=%d). Stop it before deleting.\n", name, (int)existing);
        return;
    }

    char prompt[256];
    snprintf(prompt, sizeof(prompt), "Are you sure you want to delete '%s'? [Y/n]: ", name);
    if (!ask_yes_default_yes(prompt)) { printf("Aborted.\n"); return; }

    if (remove_recursive(accountPath) != 0) { printf("Error: Failed to delete account '%s'\n", name); return; }
    printf("Account '%s' deleted.\n", name);
}

/* quick existence check for an account */
void checkUser(void) {
    char name[128]; printf("Enter account name to check: ");
    if (!fgets(name, sizeof(name), stdin)) return;
    name[strcspn(name, "\n")] = 0;
    char accountPath[PATH_MAX]; snprintf(accountPath, sizeof(accountPath), "%s/%s", ACCOUNTS_DIR, name);
    DIR *d = opendir(accountPath); if (d) { closedir(d); printf("Account '%s' exists.\n", name); } else printf("Account '%s' does not exist.\n", name);
}

/* print account disk path and (persisted) SSH port if known */
void userInfo(void) {
    char name[128]; printf("Enter account name: ");
    if (!fgets(name, sizeof(name), stdin)) return;
    name[strcspn(name, "\n")] = 0;
    char accountPath[PATH_MAX]; snprintf(accountPath, sizeof(accountPath), "%s/%s", ACCOUNTS_DIR, name);
    DIR *d = opendir(accountPath); if (!d) { printf("Account '%s' does not exist.\n", name); return; } closedir(d);
    char disk[PATH_MAX * 2];
    if (snprintf(disk, sizeof(disk), "%s/disk.qcow2", accountPath) >= (int)sizeof(disk)) { printf("Internal path too long\n"); return; }
    int port = -1;
    char sshportpath[PATH_MAX];
    if (snprintf(sshportpath, sizeof(sshportpath), "%s/%s", accountPath, SSH_PORT_FILE) < (int)sizeof(sshportpath)) {
        (void)read_int_file(sshportpath, &port);
    }
    if (port <= 0) {
        port = 22;
    }
    printf("User: %s\nDisk: %s\nSSH Port: %d\nDisk exists: %s\n", name, disk, port, access(disk, F_OK) == 0 ? "yes" : "no");
}

/* Deep copy an existing account directory to a new account name. */
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
    
    if (copy_recursive(srcPath, destPath) != 0) { printf("Error: Failed to clone user data\n"); return; }
    if (deployLaunchBinary(destPath) != 0) {
        fprintf(stderr, "Warning: launch helper not deployed to %s\n", destPath);
    }
    printf("User '%s' cloned to '%s'.\n", src, dest);
}

/* Replace an account's disk with a fresh base image copy (keeps other files). */
void resetUser(void) {
    char name[128];
    printf("Enter account to reset: ");
    if (!fgets(name, sizeof(name), stdin)) return;
    name[strcspn(name, "\n")] = 0;
    if (!validateName(name)) { printf("Invalid account name\n"); return; }
    char accountPath[PATH_MAX], disk[PATH_MAX];
    if (snprintf(accountPath, sizeof(accountPath), "%s/%s", ACCOUNTS_DIR, name) >= (int)sizeof(accountPath)) return;
    if (snprintf(disk, sizeof(disk), "%s/disk.qcow2", accountPath) >= (int)sizeof(disk)) return;
    DIR *d = opendir(accountPath);
    if (!d) { printf("Account '%s' does not exist\n", name); return; }
    closedir(d);
    if (ensureBaseImage() != 0) { printf("Base image not available\n"); return; }
    if (access(disk, F_OK) == 0) {
        char bak[PATH_MAX + 8];
        snprintf(bak, sizeof(bak), "%s.bak", disk);
        if (rename(disk, bak) != 0) { perror("rename backup"); return; }
    }
    printf("Copying fresh base qcow2...\n");
    if (copyFile(VM_BASE_QCOW2, disk) != 0) { fprintf(stderr, "Failed to copy base -> %s\n", disk); return; }
    if (deployLaunchBinary(accountPath) != 0) {
        fprintf(stderr, "Warning: launch helper not deployed to %s\n", accountPath);
    }
    printf("User '%s' was reset. (disk=%s)\n", name, disk);
}


/* Redownload and re-provision the shared base image unconditionally. */
void rebuildBase(void) {
    printf("Rebuilding base image...\n");
    if (access(VM_BASE_QCOW2, F_OK) == 0) { if (unlink(VM_BASE_QCOW2) != 0) perror("unlink base"); }
    ensureBaseImage();
    printf("Base image rebuilt.\n");
}

/* find a non-loopback IPv6/IPv4 address to advertise to users */
void showServerIP(void) {
    struct ifaddrs *ifaddr, *ifa;
    char found6[INET6_ADDRSTRLEN] = "";
    char found4[INET_ADDRSTRLEN] = "";

    if (getifaddrs(&ifaddr) == -1) {
        printf("Server IP: ::1 (fallback)\n");
        return;
    }

    for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET6 && !found6[0]) {
            struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)ifa->ifa_addr;
            if (IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr)) continue;
            if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) continue;
            char host[INET6_ADDRSTRLEN];
            if (inet_ntop(AF_INET6, &sin6->sin6_addr, host, sizeof(host)) == NULL) continue;
            strncpy(found6, host, sizeof(found6));
            found6[sizeof(found6) - 1] = '\0';
        } else if (ifa->ifa_addr->sa_family == AF_INET && !found4[0]) {
            struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
            char host[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &sin->sin_addr, host, sizeof(host)) == NULL) continue;
            if (strncmp(host, "127.", 4) == 0) continue;
            strncpy(found4, host, sizeof(found4));
            found4[sizeof(found4) - 1] = '\0';
        }
    }

    freeifaddrs(ifaddr);

    if (g_cfg.ip_mode == IP_MODE_IPV4) {
        if (found4[0]) {
            printf("Server IP (ipv4): %s\n", found4);
            return;
        }
        if (found6[0]) {
            printf("Server IP (fallback ipv6): %s\n", found6);
            return;
        }
        printf("Server IP: 127.0.0.1 (fallback)\n");
    } else {
        if (found6[0]) {
            printf("Server IP (ipv6): %s\n", found6);
            return;
        }
        if (found4[0]) {
            printf("Server IP (fallback ipv4): %s\n", found4);
            return;
        }
        printf("Server IP: ::1 (fallback)\n");
    }
}
 

/*
 * Start a VM for a chosen account with SSH port forwarding and camera bridge.
 * Steps:
 * - Validate base image presence and copy per-account disk if missing
 * - Reserve/persist a free SSH port (ipv4/ipv6 listen)
 * - Start camera bridge on the next port
 * - Spawn qemu headless with virtio disk and shared folder
 * - Write pid/log files to the account directory
 */
void startVM(void) {
    const char *qemu_bin = NULL;
    if (access("/usr/bin/qemu-system-x86_64", X_OK) == 0) {
        qemu_bin = "/usr/bin/qemu-system-x86_64";
    } else if (access("/usr/libexec/qemu-kvm", X_OK) == 0) {
        qemu_bin = "/usr/libexec/qemu-kvm";
    }
    if (!qemu_bin) {
        fprintf(stderr, "QEMU is not installed or not in PATH\n");
        return;
    }

    if (ensureBaseImage() != 0) { printf("Base image missing and cannot be prepared\n"); return; }
    char accountName[128]; if (!selectAccount(accountName)) return;
    char diskPath[PATH_MAX]; snprintf(diskPath, sizeof(diskPath), "%s/%s/disk.qcow2", ACCOUNTS_DIR, accountName);

    /* check for existing running VM for this account */
    char pidpath[PATH_MAX]; snprintf(pidpath, sizeof(pidpath), "%s/%s/vm.pid", ACCOUNTS_DIR, accountName);
    pid_t existing = pidfile_read(pidpath);
    if (existing) {
        if (process_is_running(existing)) {
            printf("Error: VM for '%s' already running (pid=%d).\n", accountName, (int)existing);
            return;
        } else {
            /* remove stale pidfile */
            unlink(pidpath);
        }
    }

    char accountDir[PATH_MAX * 2]; if (snprintf(accountDir, sizeof(accountDir), "%s/%s", ACCOUNTS_DIR, accountName) >= (int)sizeof(accountDir)) { printf("Internal path too long\n"); return; }
    char userLog[PATH_MAX * 2]; if (snprintf(userLog, sizeof(userLog), "%s/vm.log", accountDir) >= (int)sizeof(userLog)) { printf("Internal path too long\n"); return; }
    char userPid[PATH_MAX * 2]; if (snprintf(userPid, sizeof(userPid), "%s/vm.pid", accountDir) >= (int)sizeof(userPid)) { printf("Internal path too long\n"); return; }
    char qgaSockPath[PATH_MAX];
    if (snprintf(qgaSockPath, sizeof(qgaSockPath), "%s/%s/qga.sock", ACCOUNTS_DIR, accountName) >= (int)sizeof(qgaSockPath)) {
        printf("Internal path too long\n");
        return;
    }
    unlink(qgaSockPath);

    if (access(diskPath, F_OK) != 0) {
        printf("Account '%s' disk.qcow2 not found. Copying base image...\n", accountName);
        if (copyFile(VM_BASE_QCOW2, diskPath) != 0) { perror("copyFile"); return; }
    }

    if (deployLaunchBinary(accountDir) != 0) {
        fprintf(stderr, "Warning: launch helper not deployed to %s\n", accountDir);
    }

    int sshPort = -1;
    char sshportpath[PATH_MAX];
    if (snprintf(sshportpath, sizeof(sshportpath), "%s/%s", accountDir, SSH_PORT_FILE) >= (int)sizeof(sshportpath)) { printf("Internal path too long\n"); return; }

    sshPort = 22;

    char activeBridge[128];
    activeBridge[0] = '\0';
    if (g_cfg.network_mode == NET_MODE_BRIDGE) {
        if (bridge_exists(g_cfg.bridge_name)) {
            snprintf(activeBridge, sizeof(activeBridge), "%s", g_cfg.bridge_name);
        } else if (find_first_bridge(activeBridge, sizeof(activeBridge)) == 0) {
            fprintf(stderr, "Configured bridge '%s' not found. Using detected bridge '%s'.\n", g_cfg.bridge_name, activeBridge);
        } else {
            fprintf(stderr, "Bridge mode requested but no Linux bridge interface found.\n");
            fprintf(stderr, "Create a bridge (e.g. br0) or set network_mode=user in config.cfg.\n");
            return;
        }

        if (!qemu_bridge_allowed(activeBridge)) {
            fprintf(stderr, "QEMU bridge helper is not allowed to use '%s'.\n", activeBridge);
            fprintf(stderr, "Add 'allow %s' to /etc/qemu/bridge.conf (or 'allow all').\n", activeBridge);
            return;
        }
    }

    char vmMac[32];
    if (buildVmMacForAccount(accountName, vmMac, sizeof(vmMac)) != 0) {
        fprintf(stderr, "Failed to build VM MAC for account '%s'\n", accountName);
        return;
    }

    if (write_int_file(sshportpath, sshPort) != 0) {
        fprintf(stderr, "Warning: could not persist ssh port to %s\n", sshportpath);
    }

    int cameraPort = -1;
    pid_t camPid = startCameraBridge(accountDir, sshPort + 1, &cameraPort);
    if (camPid <= 0) {
        fprintf(stderr, "Warning: camera bridge not started for '%s'\n", accountName);
    }

    int pw[2];
    if (pipe(pw) != 0) { perror("pipe"); return; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); close(pw[0]); close(pw[1]); return; }

    if (pid == 0) {
        close(pw[0]);
        int flags = fcntl(pw[1], F_GETFD);
        if (flags != -1) fcntl(pw[1], F_SETFD, flags | FD_CLOEXEC);

          int fd = open(userLog, O_CREAT | O_WRONLY | O_APPEND, 0644);
          if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
          
          int nullfd = open("/dev/null", O_RDONLY);
          if (nullfd >= 0) { dup2(nullfd, STDIN_FILENO); if (nullfd != STDIN_FILENO) close(nullfd); }
                char netdevarg[256];
                char devicearg[256];
                if (g_cfg.network_mode == NET_MODE_BRIDGE) {
                    snprintf(netdevarg, sizeof(netdevarg), "bridge,id=net0,br=%s", activeBridge);
                } else {
                    snprintf(netdevarg, sizeof(netdevarg), "user,id=net0,ipv6=on,hostfwd=tcp:0.0.0.0:%d-:22", sshPort);
                }
                snprintf(devicearg, sizeof(devicearg), "virtio-net-pci,netdev=net0,mac=%s", vmMac);
                char drivearg[PATH_MAX + 64]; snprintf(drivearg, sizeof(drivearg), "file=%s,format=qcow2,if=virtio", diskPath);
                char qgaCharDevArg[PATH_MAX + 128];
                if (snprintf(qgaCharDevArg, sizeof(qgaCharDevArg), "socket,id=qga0,path=%s,server=on,wait=off", qgaSockPath) >= (int)sizeof(qgaCharDevArg)) {
                    _exit(127);
                }
                char *const argv[] = {
                    (char *)qemu_bin,
                    "-m", "512M",
                    "-cpu", "host",
                    "-nographic",
                    "-device", "virtio-rng-pci", /* provide entropy so sshd banner is fast */
                    "-netdev", netdevarg,
                    "-device", devicearg,
                    "-chardev", qgaCharDevArg,
                    "-device", "virtio-serial-pci",
                    "-device", "virtserialport,chardev=qga0,name=org.qemu.guest_agent.0",
                    "-drive", drivearg,
                    NULL
                };
        execv(qemu_bin, argv);

        
        int save_errno = errno;
        (void)write(pw[1], &save_errno, sizeof(save_errno));
        close(pw[1]);
        _exit(127);
    }

    close(pw[1]);
    int child_errno = 0;
    ssize_t r = read(pw[0], &child_errno, sizeof(child_errno));
    close(pw[0]);

    if (r == 0) {
        sleep_ms(700);
        int quick_status = 0;
        pid_t quick = waitpid(pid, &quick_status, WNOHANG);
        if (quick == pid) {
            fprintf(stderr, "QEMU exited shortly after start. Check log: %s\n", userLog);
            return;
        }

        FILE *f = fopen(userPid, "w"); if (f) { fprintf(f, "%d\n", pid); fclose(f); }
        int fd = open(userLog, O_CREAT | O_WRONLY | O_APPEND, 0644); if (fd >= 0) close(fd);
        if (cameraPort > 0) {
            printf("VM started: ssh=%d camera=%d pid=%d disk=%s log=%s pidfile=%s (cam pid may be %d)\n", sshPort, cameraPort, pid, diskPath, userLog, userPid, (int)camPid);
        } else {
            printf("VM started: ssh=%d pid=%d disk=%s log=%s pidfile=%s\n", sshPort, pid, diskPath, userLog, userPid);
        }
        if (g_cfg.network_mode == NET_MODE_BRIDGE) {
            char vmIp[64];
            printf("Network mode: bridge (%s)\n", activeBridge);
            printf("Each VM gets its own IP from your network DHCP.\n");
            printf("VM MAC: %s\n", vmMac);
            if (findVmIpViaQga(qgaSockPath, vmIp, sizeof(vmIp), 45, 1000) == 0 ||
                findVmIpByMacOnBridge(activeBridge, vmMac, vmIp, sizeof(vmIp), 30, 1000) == 0) {
                printf("VM IP: %s\n", vmIp);
                printf("SSH connect: ssh cloud@%s\n", vmIp);
            } else {
                printf("VM IP: unresolved (guest agent/neighbor not ready yet)\n");
                printf("SSH connect: ssh cloud@<vm-ip> (port 22)\n");
            }
        } else {
            char publicIp[128];
            if (fetchPublicIPIfconfigMe(publicIp, sizeof(publicIp)) == 0) {
                printf("VM SSH via QEMU hostfwd is active on port %d\n", sshPort);
                printf("Public IP (ifconfig.me): %s\n", publicIp);
                printf("SSH connect: ssh -p %d cloud@%s\n", sshPort, publicIp);
            } else {
                printf("VM SSH via QEMU hostfwd is active on port %d\n", sshPort);
                printf("Public IP (ifconfig.me): unavailable\n");
                printf("SSH connect (local): ssh -p %d cloud@127.0.0.1\n", sshPort);
            }
        }
    } else if (r > 0) {
        int status = 0; waitpid(pid, &status, 0);
        fprintf(stderr, "Failed to start qemu: exec failed (errno=%d)\n", child_errno);
        return;
    } else {
        int saved = errno; fprintf(stderr, "Failed to start qemu: pipe read error: %s\n", strerror(saved));
        waitpid(pid, NULL, 0);
        return;
    }
    int fd = open(userLog, O_CREAT | O_WRONLY | O_APPEND, 0644); if (fd >= 0) close(fd);
}

/* Stop VM and camera bridge for a selected account, cleaning pid files. */
void stopVM(void) {
    char accountName[128];
    if (!selectAccount(accountName)) return;
    char accountDir[PATH_MAX * 2];
    if (snprintf(accountDir, sizeof(accountDir), "%s/%s", ACCOUNTS_DIR, accountName) >= (int)sizeof(accountDir)) {
        printf("Internal path too long\n");
        return;
    }
    char userPid[PATH_MAX * 2];
    if (snprintf(userPid, sizeof(userPid), "%s/vm.pid", accountDir) >= (int)sizeof(userPid)) {
        printf("Internal path too long\n");
        return;
    }
    pid_t pid = pidfile_read(userPid);
    if (!pid) { printf("No pidfile found for '%s'. Is the VM running?\n", accountName); return; }
    if (!process_is_running(pid)) {
        printf("Stale pidfile found (pid=%d). Removing pidfile.\n", (int)pid);
        if (unlink(userPid) != 0) perror("unlink pidfile");
        stopIPv6Forward(accountDir);
        stopCameraBridge(accountDir);
        return;
    }
    if (kill((pid_t)pid, SIGTERM) != 0) { perror("kill"); return; }
    if (unlink(userPid) != 0) perror("unlink pidfile");
    stopIPv6Forward(accountDir);
    stopCameraBridge(accountDir);
    printf("Sent SIGTERM to pid %d for account '%s' and stopped camera bridge\n", (int)pid, accountName);
}

 
/* print the interactive command list */
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
    printf("serverip      - Show server IP address\n");
    printf("userinfo      - Show info about a user\n\n");
}

/* REPL-style command loop */
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
        else if (!strcmp(input, "serverip")) showServerIP();
        else if (!strcmp(input, "exit")) {
            if (ask_yes_default_yes("Are you sure you want to exit? [Y/n]: ")) exit(0);
            continue;
        }
        else if (strlen(input) == 0) continue;
        else printf("Error: Unknown command '%s'. Type 'help' for available commands.\n", input);
    }
}

/* entry point: ensure folders/base exist, then enter menu loop */
int main(void) {
    loadConfig();
    if (ensureAccountsFolder() != 0) {
        fprintf(stderr, "Failed to create accounts directory\n");
        return 1;
    }
    if (ensureBaseImage() != 0) fprintf(stderr, "Warning: base image not available\n");
    menu();
    
    return 0;
}

/* Parse config.cfg for base_image_url and ip_mode. Missing file keeps defaults. */
void loadConfig(void) {
    /* reset defaults */
    snprintf(g_cfg.base_image_url, sizeof(g_cfg.base_image_url), "%s", DEFAULT_BASE_IMAGE_URL);
    g_cfg.ip_mode = IP_MODE_IPV6;
    g_cfg.network_mode = NET_MODE_BRIDGE;
    snprintf(g_cfg.bridge_name, sizeof(g_cfg.bridge_name), "%s", DEFAULT_BRIDGE_NAME);

    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return;

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        trim_leading_ws(&p);
        if (*p == '#' || *p == ';' || *p == '\0') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;
        trim_trailing_ws(p);
        trim_trailing_ws(val);
        trim_leading_ws(&val);

        if (strcasecmp(p, "base_image_url") == 0) {
            if (*val) {
                snprintf(g_cfg.base_image_url, sizeof(g_cfg.base_image_url), "%s", val);
            }
        } else if (strcasecmp(p, "ip_mode") == 0) {
            if (strcasecmp(val, "ipv4") == 0) g_cfg.ip_mode = IP_MODE_IPV4;
            else if (strcasecmp(val, "ipv6") == 0) g_cfg.ip_mode = IP_MODE_IPV6;
        } else if (strcasecmp(p, "network_mode") == 0) {
            if (strcasecmp(val, "bridge") == 0) g_cfg.network_mode = NET_MODE_BRIDGE;
            else if (strcasecmp(val, "user") == 0) g_cfg.network_mode = NET_MODE_USER;
        } else if (strcasecmp(p, "bridge_name") == 0) {
            if (*val) {
                snprintf(g_cfg.bridge_name, sizeof(g_cfg.bridge_name), "%s", val);
            }
        }
    }

    fclose(f);
}
