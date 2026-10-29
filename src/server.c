#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "server.h"
#include "http_parser.h"
#include "response.h"
#include "file_handler.h"
#include "directory.h"
#include "cgi_handler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <limits.h>

static void sigchld_handler(int signo) {
    int saved_errno = errno;
    (void)signo;
    /* Reap all terminated children without blocking */
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        ;
    }
    errno = saved_errno;
}

void setup_signals(void) {
    struct sigaction sa;

    /* Handle SIGCHLD to reap zombies */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction(SIGCHLD)");
        exit(EXIT_FAILURE);
    }

    /* Ignore SIGPIPE so writing to closed sockets does not kill the server */
    signal(SIGPIPE, SIG_IGN);
}

void daemonize(const char *logfile) {
    pid_t pid;
    int fd;

    /* 1. Reset file creation mask */
    umask(0);

    /* 2. Fork and terminate parent */
    pid = fork();
    if (pid < 0) {
        perror("fork(daemonize 1)");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        _exit(EXIT_SUCCESS); /* Parent exits */
    }

    /* 3. Become session leader */
    if (setsid() < 0) {
        perror("setsid");
        exit(EXIT_FAILURE);
    }

    /* 4. Second fork to ensure daemon cannot acquire a controlling terminal */
    pid = fork();
    if (pid < 0) {
        perror("fork(daemonize 2)");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        _exit(EXIT_SUCCESS);
    }

    /* 5. Change working directory to root to unmount filesystems cleanly */
    if (chdir("/") < 0) {
        perror("chdir(/)");
        exit(EXIT_FAILURE);
    }

    /* 6. Close standard file descriptors and redirect */
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    /* Redirect stdin to /dev/null */
    fd = open("/dev/null", O_RDONLY);
    if (fd != STDIN_FILENO) {
        if (fd >= 0) {
            dup2(fd, STDIN_FILENO);
            close(fd);
        }
    }

    /* Redirect stdout and stderr */
    if (logfile && logfile[0] != '\0') {
        int log_fd = open(logfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            if (log_fd > STDERR_FILENO) {
                close(log_fd);
            }
        } else {
            /* Fallback to /dev/null if opening logfile fails */
            int null_out = open("/dev/null", O_WRONLY);
            if (null_out >= 0) {
                dup2(null_out, STDOUT_FILENO);
                dup2(null_out, STDERR_FILENO);
                close(null_out);
            }
        }
    } else {
        int null_out = open("/dev/null", O_WRONLY);
        if (null_out >= 0) {
            dup2(null_out, STDOUT_FILENO);
            dup2(null_out, STDERR_FILENO);
            close(null_out);
        }
    }
}

int server_init(const server_config_t *config) {
    int listen_fd;
    int optval = 1;
    struct sockaddr_in serv_addr;

    if (!config) {
        return -1;
    }

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }

    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(listen_fd);
        return -1;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons((uint16_t)config->port);

    if (bind(listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, BACKLOG) < 0) {
        perror("listen");
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

static void handle_client(int client_fd, const server_config_t *config, const char *client_ip) {
    http_request_t req;
    char resolved_path[PATH_MAX];
    struct stat st;
    int parse_status;

    parse_status = parse_http_request(client_fd, &req);
    if (parse_status != 0) {
        if (parse_status == 501) {
            send_error_response(client_fd, 501, "Not Implemented",
                                "The requested method is not implemented. Only GET and HEAD are supported.", 0);
        } else if (parse_status == 403) {
            send_error_response(client_fd, 403, "Forbidden",
                                "Directory traversal or restricted path detected.", 0);
        } else {
            send_error_response(client_fd, 400, "Bad Request",
                                "The server could not understand your request.", 0);
        }
        return;
    }

    /* Check for CGI request */
    if (is_cgi_request(req.path)) {
        char script_path[PATH_MAX];
        int found = 0;

        /* Try relative to docroot */
        if (snprintf(script_path, sizeof(script_path), "%s%s", config->docroot, req.path) < (int)sizeof(script_path)) {
            if (stat(script_path, &st) == 0) {
                found = 1;
            }
        }

        /* Try relative to docroot/.. */
        if (!found) {
            if (snprintf(script_path, sizeof(script_path), "%s/..%s", config->docroot, req.path) < (int)sizeof(script_path)) {
                if (stat(script_path, &st) == 0) {
                    found = 1;
                }
            }
        }

        /* Try directly as relative path (e.g. ./cgi-bin/hello.cgi) */
        if (!found) {
            if (req.path[0] == '/') {
                if (snprintf(script_path, sizeof(script_path), ".%s", req.path) < (int)sizeof(script_path)) {
                    if (stat(script_path, &st) == 0) {
                        found = 1;
                    }
                }
            }
        }

        if (found) {
            handle_cgi(client_fd, &req, script_path, client_ip);
        } else {
            send_error_response(client_fd, 404, "Not Found", "CGI script not found.", req.method == METHOD_HEAD);
        }
        return;
    }

    /* Regular file or directory request */
    if (resolve_path(config->docroot, req.path, resolved_path, sizeof(resolved_path)) != 0) {
        send_error_response(client_fd, 403, "Forbidden", "Access denied.", req.method == METHOD_HEAD);
        return;
    }

    if (stat(resolved_path, &st) != 0) {
        if (errno == ENOENT) {
            send_error_response(client_fd, 404, "Not Found", "Resource not found.", req.method == METHOD_HEAD);
        } else if (errno == EACCES) {
            send_error_response(client_fd, 403, "Forbidden", "Permission denied.", req.method == METHOD_HEAD);
        } else {
            send_error_response(client_fd, 500, "Internal Server Error", "Stat failed.", req.method == METHOD_HEAD);
        }
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        serve_directory(client_fd, &req, resolved_path, req.path);
    } else if (S_ISREG(st.st_mode)) {
        serve_file(client_fd, &req, resolved_path, &st);
    } else {
        send_error_response(client_fd, 403, "Forbidden", "Special files cannot be served.", req.method == METHOD_HEAD);
    }
}

void server_run(int listen_fd, const server_config_t *config) {
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        pid_t pid;
        char client_ip[INET_ADDRSTRLEN];

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue; /* Interrupted by signal, retry accept */
            }
            perror("accept");
            continue;
        }

        if (inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip)) == NULL) {
            strncpy(client_ip, "Unknown", sizeof(client_ip) - 1);
            client_ip[sizeof(client_ip) - 1] = '\0';
        }

        /* Fork process for each incoming connection */
        pid = fork();
        if (pid < 0) {
            perror("fork");
            close(client_fd);
            continue;
        }

        if (pid == 0) {
            /* Child process */
            close(listen_fd); /* Close listening socket in child */
            handle_client(client_fd, config, client_ip);
            close(client_fd);
            _exit(EXIT_SUCCESS);
        }

        /* Parent process */
        close(client_fd); /* Close client socket in parent */
    }
}
