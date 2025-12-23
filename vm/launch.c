#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int sig) {
	(void)sig;
	g_stop = 1;
}

static void usage(const char *prog) {
	fprintf(stderr,
			"Usage: %s --camera-port <port> --out <file> [--log <file>] [--pid-file <file>] [--max-bytes <N>]\n"
			"  --camera-port   TCP port to listen on for camera stream\n"
			"  --out           Output file path (written/overwritten per connection)\n"
			"  --log           Optional log file path (append)\n"
			"  --pid-file      Optional pid file to write own PID\n"
			"  --max-bytes     Optional limit per connection (bytes, default 50MB)\n",
			prog);
}

static int write_pidfile(const char *pidfile) {
	if (!pidfile) return 0;
	int fd = open(pidfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) return -1;
	char buf[32];
	int len = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
	ssize_t w = write(fd, buf, (size_t)len);
	close(fd);
	return (w == len) ? 0 : -1;
}

static int listen_port(int port) {
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) return -1;
	int one = 1;
	(void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((uint16_t)port);
	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(s);
		return -1;
	}
	if (listen(s, 1) != 0) {
		close(s);
		return -1;
	}
	return s;
}

static void log_line(FILE *logf, const char *msg) {
	if (!logf) return;
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char ts[32];
	strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
	fprintf(logf, "%s %s\n", ts, msg);
	fflush(logf);
}

static int copy_stream(int client_fd, const char *out_path, size_t max_bytes, FILE *logf) {
	int out = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) {
		log_line(logf, "open output failed");
		return -1;
	}
	char buf[16 * 1024];
	size_t total = 0;
	while (!g_stop) {
		ssize_t r = read(client_fd, buf, sizeof(buf));
		if (r == 0) break; /* client closed */
		if (r < 0) {
			if (errno == EINTR) continue;
			log_line(logf, "read error");
			close(out);
			return -1;
		}
		if (total + (size_t)r > max_bytes) {
			size_t allowed = max_bytes > total ? max_bytes - total : 0;
			if (allowed > 0) {
				ssize_t w = write(out, buf, allowed);
				(void)w;
				total += allowed;
			}
			log_line(logf, "max bytes reached, dropping rest");
			break;
		}
		ssize_t w = write(out, buf, (size_t)r);
		if (w != r) {
			log_line(logf, "write error");
			close(out);
			return -1;
		}
		total += (size_t)r;
	}
	close(out);
	char msg[128];
	snprintf(msg, sizeof(msg), "connection closed, wrote %zu bytes", total);
	log_line(logf, msg);
	return 0;
}

int main(int argc, char **argv) {
	int cam_port = 0;
	const char *out_path = NULL;
	const char *log_path = NULL;
	const char *pid_path = NULL;
	size_t max_bytes = 50 * 1024 * 1024; /* default 50MB per connection */

	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--camera-port") && i + 1 < argc) {
			cam_port = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
			out_path = argv[++i];
		} else if (!strcmp(argv[i], "--log") && i + 1 < argc) {
			log_path = argv[++i];
		} else if (!strcmp(argv[i], "--pid-file") && i + 1 < argc) {
			pid_path = argv[++i];
		} else if (!strcmp(argv[i], "--max-bytes") && i + 1 < argc) {
			long long v = atoll(argv[++i]);
			if (v > 0) max_bytes = (size_t)v;
		} else {
			usage(argv[0]);
			return 1;
		}
	}

	if (cam_port <= 0 || !out_path) {
		usage(argv[0]);
		return 1;
	}

	FILE *logf = stderr;
	if (log_path) {
		logf = fopen(log_path, "a");
		if (!logf) {
			perror("fopen log");
			return 1;
		}
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	if (write_pidfile(pid_path) != 0) {
		log_line(logf, "failed to write pid file");
	}

	int s = listen_port(cam_port);
	if (s < 0) {
		log_line(logf, "failed to bind camera port");
		if (logf && logf != stderr) fclose(logf);
		return 1;
	}

	log_line(logf, "camera bridge ready");

	while (!g_stop) {
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(s, &rfds);
		struct timeval tv = {1, 0};
		int r = select(s + 1, &rfds, NULL, NULL, &tv);
		if (r < 0) {
			if (errno == EINTR) continue;
			log_line(logf, "select error");
			break;
		}
		if (r == 0) continue; /* timeout */

		if (FD_ISSET(s, &rfds)) {
			struct sockaddr_in peer;
			socklen_t plen = sizeof(peer);
			int c = accept(s, (struct sockaddr *)&peer, &plen);
			if (c < 0) {
				if (errno == EINTR) continue;
				log_line(logf, "accept error");
				continue;
			}
			log_line(logf, "connection accepted");
			copy_stream(c, out_path, max_bytes, logf);
			close(c);
		}
	}

	close(s);
	log_line(logf, "camera bridge stopped");
	if (logf && logf != stderr) fclose(logf);
	return 0;
}
