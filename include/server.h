#ifndef SERVER_H
#define SERVER_H

#include <sys/types.h>

#define DEFAULT_PORT 8080
#define DEFAULT_DOCROOT "www"
#define SERVER_NAME "cs631-httpd/1.0"
#define BACKLOG 128

typedef struct server_config {
    int port;
    char docroot[1024];
    char logfile[1024];
    int daemon_mode;
} server_config_t;

int server_init(const server_config_t *config);
void server_run(int listen_fd, const server_config_t *config);
void daemonize(const char *logfile);
void setup_signals(void);

#endif /* SERVER_H */
