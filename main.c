#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <ctype.h>

#define BUF_SIZE 256
#define DEFAULT_USER "cloud"
#define BASE_PORT 2200
#define CONNECT_TIMEOUT_SEC 2
#define DEFAULT_CAMERA_DEV "/dev/video0"

/* Prüft, ob eine TCP-Verbindung zu host:port innerhalb timeout möglich ist. */
static int try_connect(const char *host, int port, int timeout_sec) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(s);
        return 0;
    }

    /* Non-blocking connect mit select timeout */
    int flags = fcntl(s, F_GETFL, 0);
    if (flags == -1) { close(s); return 0; }
    if (fcntl(s, F_SETFL, flags | O_NONBLOCK) == -1) { close(s); return 0; }

    int res = connect(s, (struct sockaddr *)&addr, sizeof(addr));
    if (res == 0) {
        /* Sofort verbunden */
        fcntl(s, F_SETFL, flags);
        close(s);
        return 1;
    }

    if (errno != EINPROGRESS) {
        close(s);
        return 0;
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(s, &wfds);
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;

    res = select(s + 1, NULL, &wfds, NULL, &tv);
    if (res <= 0) {
        /* Timeout oder Fehler */
        close(s);
        return 0;
    }

    if (FD_ISSET(s, &wfds)) {
        int sockerr = 0;
        socklen_t len = sizeof(sockerr);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, &sockerr, &len) < 0) { close(s); return 0; }
        if (sockerr == 0) {
            fcntl(s, F_SETFL, flags);
            close(s);
            return 1;
        }
    }

    close(s);
    return 0;
}

/* einfache Port-Berechnung kompatibel zum server-seitigen userInfo-Ansatz */
static int compute_port_for_user(const char *user) {
    int port = BASE_PORT;
    for (const unsigned char *p = (const unsigned char *)user; *p; ++p) port += *p;
    if (port < 1024) port = BASE_PORT; /* Sicherheitsnetz */
    return port;
}

static pid_t start_camera_stream(const char *dev, const char *ip, int cam_port) {
    if (!dev || !*dev || !ip || cam_port <= 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork camera");
        return -1;
    }
    if (pid == 0) {
        char url[BUF_SIZE];
        if (snprintf(url, sizeof(url), "tcp://%s:%d", ip, cam_port) >= (int)sizeof(url)) {
            _exit(127);
        }
        /* ffmpeg command: v4l2 -> mjpeg -> tcp */
        execlp("ffmpeg", "ffmpeg",
               "-f", "v4l2",
               "-i", dev,
               "-f", "mjpeg",
               "-q:v", "5",
               "-vf", "format=yuvj422p",
               "-loglevel", "error",
               "-nostdin",
               url,
               (char *)NULL);
        perror("ffmpeg");
        _exit(127);
    }
    printf("Kamera-Stream gestartet (PID=%d) -> %s:%d\n", (int)pid, ip, cam_port);
    return pid;
}

int main(void) {
    char ip[BUF_SIZE];
    int port = 0;
    int cam_port = 0;
    char cam_dev[BUF_SIZE];
    cam_dev[0] = '\0';
    const char *user = DEFAULT_USER;

    printf("CLOUDPHONE OS - Server kompatibler SSH-Client\n");
    printf("Verbinde als Benutzer: %s\n\n", user);

    printf("Bitte Server-IP eingeben (IPv4): ");
    if (scanf("%255s", ip) != 1) {
        fprintf(stderr, "Fehler: Keine IP gelesen.\n");
        return 1;
    }

    /* Port automatisch berechnen (kompatibel mit server userInfo-Berechnung) */
    port = compute_port_for_user(user);
    printf("Berechneter Standard-Port für '%s': %d\n", user, port);

    printf("Kamera-Port (leer = überspringen): ");
    if (scanf("%d", &cam_port) != 1) {
        cam_port = 0;
    }
    if (cam_port > 0) {
        printf("Kamera-Device (Standard %s): ", DEFAULT_CAMERA_DEV);
        if (scanf("%255s", cam_dev) != 1) {
            strncpy(cam_dev, DEFAULT_CAMERA_DEV, sizeof(cam_dev));
            cam_dev[sizeof(cam_dev)-1] = '\0';
        }
    }

    printf("Prüfe Verbindung zu %s:%d ...\n", ip, port);
    if (try_connect(ip, port, CONNECT_TIMEOUT_SEC)) {
        printf("Port %d erreichbar. Starte SSH...\n", port);
    } else {
        printf("Port %d nicht erreichbar.\n", port);
        /* Fallback: Benutzer nach Port fragen */
        printf("Gib alternativen Port ein oder 0 zum Abbrechen: ");
        if (scanf("%d", &port) != 1) {
            fprintf(stderr, "Fehler beim Lesen des Ports.\n");
            return 1;
        }
        if (port <= 0) {
            printf("Abbruch durch Benutzer.\n");
            return 0;
        }
        if (!try_connect(ip, port, CONNECT_TIMEOUT_SEC)) {
            printf("Port %d ebenfalls nicht erreichbar. Abbruch.\n", port);
            return 1;
        }
    }

    /* Baue Ziel-String user@ip */
    char target[BUF_SIZE * 2];
    if (snprintf(target, sizeof(target), "%s@%s", user, ip) >= (int)sizeof(target)) {
        fprintf(stderr, "Interner Fehler: Zielpfad zu lang.\n");
        return 1;
    }

    char port_str[32];
    snprintf(port_str, sizeof(port_str), "%d", port);

    /* Kamera-Stream starten, falls gewünscht */
    pid_t cam_pid = -1;
    if (cam_port > 0) {
        cam_pid = start_camera_stream(cam_dev[0] ? cam_dev : DEFAULT_CAMERA_DEV, ip, cam_port);
    }

    /* Fork & exec ssh */
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* Kindprozess: ersetze durch ssh */
        execlp("ssh", "ssh", "-p", port_str, target, (char *)NULL);
        /* Falls exec fehlschlägt */
        fprintf(stderr, "Fehler beim Ausführen von ssh: %s\n", strerror(errno));
        _exit(127);
    } else {
        /* Elternprozess wartet auf Ende von ssh */
        int status = 0;
        if (waitpid(pid, &status, 0) == -1) {
            perror("waitpid");
            return 1;
        }
        if (WIFEXITED(status)) {
            printf("SSH beendet mit Status %d\n", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            printf("SSH wurde durch Signal %d beendet\n", WTERMSIG(status));
        } else {
            printf("SSH beendet (unbekannter Status).\n");
        }
    }

    if (cam_pid > 0) {
        printf("Beende Kamera-Stream (PID=%d)\n", (int)cam_pid);
        kill(cam_pid, SIGTERM);
        waitpid(cam_pid, NULL, 0);
    }

    return 0;
}
