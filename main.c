#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <gtk/gtk.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define BUF_SIZE 256
#define DEFAULT_USER "cloud"
#define BASE_PORT 2200
#define CONNECT_TIMEOUT_SEC 2
#define DEFAULT_CAMERA_DEV "/dev/video0"

typedef struct {
    GtkWidget *entry_ip;
    GtkWidget *entry_user;
    GtkWidget *entry_port;
    GtkWidget *entry_cam_port;
    GtkWidget *entry_cam_dev;
    GtkWidget *check_camera;
    GtkWidget *log_view;
    GtkWidget *btn_connect;
    GtkWidget *btn_stop;
    GPid ssh_pid;
    GPid cam_pid;
    GIOChannel *ssh_out;
    GIOChannel *ssh_err;
} AppState;

static void append_log(AppState *app, const char *fmt, ...) {
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->log_view));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);

    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    gtk_text_buffer_insert(buf, &end, line, -1);
    gtk_text_buffer_insert(buf, &end, "\n", 1);
    gtk_text_buffer_get_end_iter(buf, &end);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(app->log_view), &end, 0.0, FALSE, 0, 0);
}

static int compute_port_for_user(const char *user) {
    int port = BASE_PORT;
    for (const unsigned char *p = (const unsigned char *)user; *p; ++p) port += *p;
    if (port < 1024) port = BASE_PORT;
    return port;
}

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

    int flags = fcntl(s, F_GETFL, 0);
    if (flags == -1) { close(s); return 0; }
    if (fcntl(s, F_SETFL, flags | O_NONBLOCK) == -1) { close(s); return 0; }

    int res = connect(s, (struct sockaddr *)&addr, sizeof(addr));
    if (res == 0) {
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

static void stop_process(GPid *pid, const char *label, AppState *app, bool has_watch) {
    if (*pid <= 0) return;
    append_log(app, "%s wird beendet (PID=%d)...", label, (int)*pid);
    kill(*pid, SIGTERM);
    if (!has_watch) {
        waitpid(*pid, NULL, WNOHANG);
        g_spawn_close_pid(*pid);
        *pid = 0;
    }
}

static gboolean handle_child_output(GIOChannel *source, GIOCondition cond, gpointer data) {
    AppState *app = data;
    if (cond & (G_IO_HUP | G_IO_ERR)) {
        return FALSE;
    }

    char buf[256];
    gsize read_bytes = 0;
    GIOStatus st = g_io_channel_read_chars(source, buf, sizeof(buf) - 1, &read_bytes, NULL);
    if (st == G_IO_STATUS_NORMAL && read_bytes > 0) {
        buf[read_bytes] = '\0';
        char *line = buf;
        while (*line) {
            char *nl = strchr(line, '\n');
            if (nl) {
                *nl = '\0';
                append_log(app, "%s", line);
                line = nl + 1;
            } else {
                append_log(app, "%s", line);
                break;
            }
        }
    }
    return TRUE;
}

static void reset_channels(AppState *app) {
    if (app->ssh_out) { g_io_channel_unref(app->ssh_out); app->ssh_out = NULL; }
    if (app->ssh_err) { g_io_channel_unref(app->ssh_err); app->ssh_err = NULL; }
}

static void on_ssh_exit(GPid pid, gint status, gpointer data) {
    AppState *app = data;
    if (WIFEXITED(status)) {
        append_log(app, "SSH beendet (Status %d)", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        append_log(app, "SSH durch Signal %d beendet", WTERMSIG(status));
    } else {
        append_log(app, "SSH beendet (unbekannter Status)");
    }
    g_spawn_close_pid(pid);
    app->ssh_pid = 0;
    reset_channels(app);
    gtk_widget_set_sensitive(app->btn_connect, TRUE);
    gtk_widget_set_sensitive(app->btn_stop, FALSE);
}

static bool start_camera(AppState *app, const char *ip, int cam_port, const char *dev) {
    if (cam_port <= 0) return true;

    char url[BUF_SIZE];
    if (snprintf(url, sizeof(url), "tcp://%s:%d", ip, cam_port) >= (int)sizeof(url)) {
        append_log(app, "Kamera-URL zu lang.");
        return false;
    }

    gchar *argv[] = {
        "ffmpeg",
        "-f", "v4l2",
        "-i", (gchar *)dev,
        "-f", "mjpeg",
        "-q:v", "5",
        "-vf", "format=yuvj422p",
        "-loglevel", "error",
        "-nostdin",
        url,
        NULL
    };

    GError *err = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &app->cam_pid, &err)) {
        append_log(app, "Kamera-Start fehlgeschlagen: %s", err->message);
        g_clear_error(&err);
        app->cam_pid = 0;
        return false;
    }

    append_log(app, "Kamera-Stream läuft (PID=%d) -> %s:%d", (int)app->cam_pid, ip, cam_port);
    return true;
}

static bool start_ssh(AppState *app, const char *user, const char *ip, int port) {
    char target[BUF_SIZE * 2];
    if (snprintf(target, sizeof(target), "%s@%s", user, ip) >= (int)sizeof(target)) {
        append_log(app, "Zielangabe zu lang.");
        return false;
    }

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    gchar *argv[] = {
        "ssh",
        "-T",
        "-o", "BatchMode=yes",
        "-o", "StrictHostKeyChecking=accept-new",
        "-p", port_str,
        target,
        NULL
    };

    GError *err = NULL;
    gint out_fd = -1;
    gint err_fd = -1;
    if (!g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                                  &app->ssh_pid, NULL, &out_fd, &err_fd, &err)) {
        append_log(app, "SSH-Start fehlgeschlagen: %s", err->message);
        g_clear_error(&err);
        app->ssh_pid = 0;
        return false;
    }

    app->ssh_out = g_io_channel_unix_new(out_fd);
    g_io_channel_set_encoding(app->ssh_out, NULL, NULL);
    g_io_channel_set_flags(app->ssh_out, g_io_channel_get_flags(app->ssh_out) | G_IO_FLAG_NONBLOCK, NULL);
    g_io_add_watch(app->ssh_out, G_IO_IN | G_IO_HUP | G_IO_ERR, handle_child_output, app);

    app->ssh_err = g_io_channel_unix_new(err_fd);
    g_io_channel_set_encoding(app->ssh_err, NULL, NULL);
    g_io_channel_set_flags(app->ssh_err, g_io_channel_get_flags(app->ssh_err) | G_IO_FLAG_NONBLOCK, NULL);
    g_io_add_watch(app->ssh_err, G_IO_IN | G_IO_HUP | G_IO_ERR, handle_child_output, app);

    g_child_watch_add(app->ssh_pid, on_ssh_exit, app);
    append_log(app, "SSH gestartet (PID=%d) -> %s:%d", (int)app->ssh_pid, ip, port);
    return true;
}

static void on_connect(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppState *app = user_data;

    const char *ip = gtk_entry_get_text(GTK_ENTRY(app->entry_ip));
    const char *user = gtk_entry_get_text(GTK_ENTRY(app->entry_user));
    const char *port_txt = gtk_entry_get_text(GTK_ENTRY(app->entry_port));
    const char *cam_port_txt = gtk_entry_get_text(GTK_ENTRY(app->entry_cam_port));
    const char *cam_dev = gtk_entry_get_text(GTK_ENTRY(app->entry_cam_dev));

    if (!ip || !*ip) {
        append_log(app, "Bitte Server-IP eintragen.");
        return;
    }
    if (!user || !*user) {
        append_log(app, "Bitte Benutzer festlegen.");
        return;
    }

    int port = (*port_txt) ? atoi(port_txt) : compute_port_for_user(user);
    if (port <= 0 || port > 65535) {
        append_log(app, "Ungültiger Port.");
        return;
    }

    int cam_port = (*cam_port_txt) ? atoi(cam_port_txt) : 0;
    bool camera_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->check_camera));
    if (camera_enabled && (cam_port <= 0 || cam_port > 65535)) {
        append_log(app, "Ungültiger Kamera-Port.");
        return;
    }

    append_log(app, "Prüfe %s:%d ...", ip, port);
    if (!try_connect(ip, port, CONNECT_TIMEOUT_SEC)) {
        append_log(app, "Port %d nicht erreichbar.", port);
        return;
    }

    gtk_widget_set_sensitive(app->btn_connect, FALSE);
    gtk_widget_set_sensitive(app->btn_stop, TRUE);

    if (camera_enabled && !start_camera(app, ip, cam_port, (*cam_dev) ? cam_dev : DEFAULT_CAMERA_DEV)) {
        gtk_widget_set_sensitive(app->btn_connect, TRUE);
        gtk_widget_set_sensitive(app->btn_stop, FALSE);
        return;
    }

    if (!start_ssh(app, user, ip, port)) {
        stop_process(&app->cam_pid, "Kamera", app, false);
        gtk_widget_set_sensitive(app->btn_connect, TRUE);
        gtk_widget_set_sensitive(app->btn_stop, FALSE);
    }
}

static void on_stop(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppState *app = user_data;
    stop_process(&app->ssh_pid, "SSH", app, true);
    stop_process(&app->cam_pid, "Kamera", app, false);
    reset_channels(app);
    gtk_widget_set_sensitive(app->btn_connect, TRUE);
    gtk_widget_set_sensitive(app->btn_stop, FALSE);
}

static void on_destroy(GtkWidget *w, gpointer user_data) {
    (void)w;
    AppState *app = user_data;
    stop_process(&app->ssh_pid, "SSH", app, true);
    stop_process(&app->cam_pid, "Kamera", app, false);
    reset_channels(app);
    gtk_main_quit();
}

static GtkWidget *build_entry(const char *placeholder, const char *text) {
    GtkWidget *entry = gtk_entry_new();
    if (placeholder) gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
    if (text) gtk_entry_set_text(GTK_ENTRY(entry), text);
    return entry;
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    AppState app = {0};

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "CloudPhone UI");
    gtk_window_set_default_size(GTK_WINDOW(win), 520, 520);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 10);
    gtk_container_add(GTK_CONTAINER(win), outer);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);

    GtkWidget *lbl_ip = gtk_label_new("Server-IP");
    app.entry_ip = build_entry("z.B. 192.168.1.10", "");
    gtk_grid_attach(GTK_GRID(grid), lbl_ip, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app.entry_ip, 1, 0, 2, 1);

    GtkWidget *lbl_user = gtk_label_new("Benutzer");
    app.entry_user = build_entry(NULL, DEFAULT_USER);
    gtk_grid_attach(GTK_GRID(grid), lbl_user, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app.entry_user, 1, 1, 2, 1);

    GtkWidget *lbl_port = gtk_label_new("SSH-Port");
    int default_port = compute_port_for_user(DEFAULT_USER);
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%d", default_port);
    app.entry_port = build_entry(NULL, port_buf);
    gtk_grid_attach(GTK_GRID(grid), lbl_port, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app.entry_port, 1, 2, 2, 1);

    app.check_camera = gtk_check_button_new_with_label("Kamera-Stream aktivieren");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.check_camera), FALSE);
    gtk_grid_attach(GTK_GRID(grid), app.check_camera, 0, 3, 3, 1);

    GtkWidget *lbl_cam_port = gtk_label_new("Kamera-Port");
    app.entry_cam_port = build_entry("z.B. 9000", "");
    gtk_grid_attach(GTK_GRID(grid), lbl_cam_port, 0, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app.entry_cam_port, 1, 4, 2, 1);

    GtkWidget *lbl_cam_dev = gtk_label_new("Kamera-Device");
    app.entry_cam_dev = build_entry(NULL, DEFAULT_CAMERA_DEV);
    gtk_grid_attach(GTK_GRID(grid), lbl_cam_dev, 0, 5, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app.entry_cam_dev, 1, 5, 2, 1);

    gtk_box_pack_start(GTK_BOX(outer), grid, FALSE, FALSE, 4);

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app.btn_connect = gtk_button_new_with_label("Verbinden");
    app.btn_stop = gtk_button_new_with_label("Stop");
    gtk_widget_set_sensitive(app.btn_stop, FALSE);
    gtk_box_pack_start(GTK_BOX(btn_box), app.btn_connect, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btn_box), app.btn_stop, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), btn_box, FALSE, FALSE, 0);

    GtkWidget *log_frame = gtk_frame_new("Ausgabe");
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    app.log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(app.log_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(app.log_view), FALSE);
    gtk_container_add(GTK_CONTAINER(scroll), app.log_view);
    gtk_container_add(GTK_CONTAINER(log_frame), scroll);
    gtk_box_pack_start(GTK_BOX(outer), log_frame, TRUE, TRUE, 4);

    g_signal_connect(app.btn_connect, "clicked", G_CALLBACK(on_connect), &app);
    g_signal_connect(app.btn_stop, "clicked", G_CALLBACK(on_stop), &app);
    g_signal_connect(win, "destroy", G_CALLBACK(on_destroy), &app);

    gtk_widget_show_all(win);
    gtk_main();
    return 0;
}
