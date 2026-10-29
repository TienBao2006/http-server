#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <limits.h>
#include <sys/stat.h>

static void print_usage(const char *prog_name) {
    fprintf(stderr,
            "Usage: %s [OPTIONS]\n"
            "Options:\n"
            "  -p <port>     Port to listen on (default: 8080)\n"
            "  -d <docroot>  Document root directory (default: www)\n"
            "  -l <logfile>  Log file destination\n"
            "  -f            Run in foreground (do not daemonize)\n"
            "  -h            Show this help message\n",
            prog_name);
}

static int make_absolute_path(const char *rel_path, char *abs_path, size_t max_len) {
    char cwd[PATH_MAX];
    if (rel_path[0] == '/') {
        strncpy(abs_path, rel_path, max_len - 1);
        abs_path[max_len - 1] = '\0';
        return 0;
    }
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        return -1;
    }
    if (snprintf(abs_path, max_len, "%s/%s", cwd, rel_path) >= (int)max_len) {
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    server_config_t config;
    int opt;
    int listen_fd;
    char abs_docroot[PATH_MAX];
    char abs_logfile[PATH_MAX];
    char resolved_docroot[PATH_MAX];
    struct stat st;

    memset(&config, 0, sizeof(config));
    config.port = DEFAULT_PORT;
    strncpy(config.docroot, DEFAULT_DOCROOT, sizeof(config.docroot) - 1);
    config.logfile[0] = '\0';
    config.daemon_mode = 1; /* Default to daemonize as specified in requirements */

    while ((opt = getopt(argc, argv, "p:d:l:fh")) != -1) {
        switch (opt) {
            case 'p':
                config.port = atoi(optarg);
                if (config.port <= 0 || config.port > 65535) {
                    fprintf(stderr, "Error: Invalid port number: %s\n", optarg);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'd':
                strncpy(config.docroot, optarg, sizeof(config.docroot) - 1);
                config.docroot[sizeof(config.docroot) - 1] = '\0';
                break;
            case 'l':
                strncpy(config.logfile, optarg, sizeof(config.logfile) - 1);
                config.logfile[sizeof(config.logfile) - 1] = '\0';
                break;
            case 'f':
                config.daemon_mode = 0; /* Foreground mode */
                break;
            case 'h':
                print_usage(argv[0]);
                exit(EXIT_SUCCESS);
            default:
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    /* Convert docroot to canonical / absolute path before daemonize chdir("/") */
    if (realpath(config.docroot, resolved_docroot) != NULL) {
        strncpy(config.docroot, resolved_docroot, sizeof(config.docroot) - 1);
    } else {
        if (make_absolute_path(config.docroot, abs_docroot, sizeof(abs_docroot)) == 0) {
            strncpy(config.docroot, abs_docroot, sizeof(config.docroot) - 1);
        }
    }
    config.docroot[sizeof(config.docroot) - 1] = '\0';

    /* Verify docroot exists and is a directory */
    if (stat(config.docroot, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Warning: Document root '%s' does not exist or is not a directory.\n", config.docroot);
    }

    /* Convert logfile to absolute path if specified */
    if (config.logfile[0] != '\0') {
        if (make_absolute_path(config.logfile, abs_logfile, sizeof(abs_logfile)) == 0) {
            strncpy(config.logfile, abs_logfile, sizeof(config.logfile) - 1);
            config.logfile[sizeof(config.logfile) - 1] = '\0';
        }
    }

    /* Setup signal handlers (SIGCHLD zombie reaping, SIGPIPE ignore) */
    setup_signals();

    /* Initialize listening socket */
    listen_fd = server_init(&config);
    if (listen_fd < 0) {
        fprintf(stderr, "Fatal: Server initialization failed on port %d\n", config.port);
        exit(EXIT_FAILURE);
    }

    printf("Starting cs631-httpd on port %d [docroot: %s]\n", config.port, config.docroot);
    if (config.logfile[0] != '\0') {
        printf("Logging to: %s\n", config.logfile);
    }
    if (config.daemon_mode) {
        printf("Daemonizing process...\n");
        fflush(stdout);
        daemonize(config.logfile);
    } else {
        printf("Running in foreground mode.\n");
        fflush(stdout);
    }

    /* Run server main accept loop */
    server_run(listen_fd, &config);

    close(listen_fd);
    return EXIT_SUCCESS;
}
