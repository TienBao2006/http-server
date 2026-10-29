#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "cgi_handler.h"
#include "response.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

int is_cgi_request(const char *uri_path) {
    if (!uri_path) return 0;
    /* Path starts with /cgi-bin/ or is /cgi-bin */
    if (strncmp(uri_path, "/cgi-bin/", 9) == 0 || strcmp(uri_path, "/cgi-bin") == 0) {
        return 1;
    }
    return 0;
}

int handle_cgi(int client_fd, const http_request_t *req,
               const char *script_path, const char *client_ip) {
    struct stat st;
    int pipefd[2];
    pid_t pid;
    char *envp[32];
    int env_idx = 0;
    char env_method[64];
    char env_query[1100];
    char env_protocol[32];
    char env_software[64];
    char env_script[1100];
    char env_pathinfo[1100];
    char env_remote[64];
    char env_gateway[32];
    char env_uri[2100];
    char *output_buf = NULL;
    size_t output_len = 0;
    size_t output_cap = 8192;
    char chunk[4096];
    ssize_t n_read;
    int child_status = 0;
    char date_buf[64];
    time_t now;

    /* 1. Check if file exists and is executable */
    if (stat(script_path, &st) != 0) {
        send_error_response(client_fd, 404, "Not Found", "CGI script not found.", req->method == METHOD_HEAD);
        return 0;
    }

    if (!S_ISREG(st.st_mode) || access(script_path, X_OK) != 0) {
        send_error_response(client_fd, 403, "Forbidden", "CGI script is not executable.", req->method == METHOD_HEAD);
        return 0;
    }

    /* 2. Create pipe */
    if (pipe(pipefd) < 0) {
        send_error_response(client_fd, 500, "Internal Server Error", "Failed to create CGI pipe.", req->method == METHOD_HEAD);
        return -1;
    }

    /* 3. Prepare Environment Variables */
    snprintf(env_method, sizeof(env_method), "REQUEST_METHOD=%s", req->method_str);
    snprintf(env_query, sizeof(env_query), "QUERY_STRING=%s", req->query_string);
    snprintf(env_protocol, sizeof(env_protocol), "SERVER_PROTOCOL=%s", req->version[0] ? req->version : "HTTP/1.0");
    snprintf(env_software, sizeof(env_software), "SERVER_SOFTWARE=%s", SERVER_NAME);
    snprintf(env_script, sizeof(env_script), "SCRIPT_NAME=%s", req->path);
    snprintf(env_pathinfo, sizeof(env_pathinfo), "PATH_INFO=%s", req->path);
    snprintf(env_remote, sizeof(env_remote), "REMOTE_ADDR=%s", client_ip ? client_ip : "127.0.0.1");
    snprintf(env_gateway, sizeof(env_gateway), "GATEWAY_INTERFACE=CGI/1.1");
    snprintf(env_uri, sizeof(env_uri), "REQUEST_URI=%s", req->raw_uri);

    envp[env_idx++] = env_method;
    envp[env_idx++] = env_query;
    envp[env_idx++] = env_protocol;
    envp[env_idx++] = env_software;
    envp[env_idx++] = env_script;
    envp[env_idx++] = env_pathinfo;
    envp[env_idx++] = env_remote;
    envp[env_idx++] = env_gateway;
    envp[env_idx++] = env_uri;
    envp[env_idx++] = "GATEWAY_INTERFACE=CGI/1.1";
    envp[env_idx] = NULL;

    /* 4. Fork child process */
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        send_error_response(client_fd, 500, "Internal Server Error", "Failed to fork CGI process.", req->method == METHOD_HEAD);
        return -1;
    }

    if (pid == 0) {
        /* Child Process */
        char *argv[2];
        int devnull;

        close(pipefd[0]); /* Close read end */

        /* Redirect stdout to pipe write end */
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(pipefd[1]);

        /* Redirect stdin from /dev/null */
        devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }

        argv[0] = (char *)script_path;
        argv[1] = NULL;

        execve(script_path, argv, envp);

        /* If execve fails with ENOEXEC, try running via /bin/sh */
        if (errno == ENOEXEC) {
            char *sh_argv[3];
            sh_argv[0] = (char *)"/bin/sh";
            sh_argv[1] = (char *)script_path;
            sh_argv[2] = NULL;
            execve("/bin/sh", sh_argv, envp);
        }

        /* If execve fails */
        _exit(127);
    }

    /* Parent Process */
    close(pipefd[1]); /* Close write end */

    output_buf = malloc(output_cap);
    if (!output_buf) {
        close(pipefd[0]);
        waitpid(pid, &child_status, 0);
        send_error_response(client_fd, 500, "Internal Server Error", "CGI memory error.", req->method == METHOD_HEAD);
        return -1;
    }

    /* Read CGI output from pipe */
    while ((n_read = read(pipefd[0], chunk, sizeof(chunk))) > 0) {
        if (output_len + (size_t)n_read >= output_cap) {
            output_cap = (output_cap + (size_t)n_read + 4096) * 2;
            char *new_buf = realloc(output_buf, output_cap);
            if (!new_buf) {
                break;
            }
            output_buf = new_buf;
        }
        memcpy(output_buf + output_len, chunk, (size_t)n_read);
        output_len += (size_t)n_read;
    }
    close(pipefd[0]);

    waitpid(pid, &child_status, 0);

    now = time(NULL);
    format_rfc1123_date(now, date_buf, sizeof(date_buf));

    if (output_len == 0 && WIFEXITED(child_status) && WEXITSTATUS(child_status) != 0) {
        free(output_buf);
        send_error_response(client_fd, 500, "Internal Server Error", "CGI execution failed.", req->method == METHOD_HEAD);
        return 0;
    }

    /* Check if output contains CGI headers */
    output_buf[output_len] = '\0';
    char *header_end = strstr(output_buf, "\r\n\r\n");
    size_t header_len = 0;
    int has_crlf = 1;
    if (!header_end) {
        header_end = strstr(output_buf, "\n\n");
        has_crlf = 0;
    }

    if (header_end) {
        header_len = (size_t)(header_end - output_buf) + (has_crlf ? 4 : 2);
    }

    /* Send HTTP Status line and default server headers */
    char prefix[256];
    int plen = snprintf(prefix, sizeof(prefix),
                        "HTTP/1.0 200 OK\r\n"
                        "Date: %s\r\n"
                        "Server: %s\r\n"
                        "Connection: close\r\n",
                        date_buf, SERVER_NAME);
    if (plen > 0) {
        write(client_fd, prefix, (size_t)plen);
    }

    /* If script did not provide headers, provide default Content-Type header */
    if (!header_end) {
        char def_ct[] = "Content-Type: text/html; charset=utf-8\r\n\r\n";
        write(client_fd, def_ct, sizeof(def_ct) - 1);
    }

    /* Send output */
    if (req->method == METHOD_HEAD) {
        if (header_end && header_len > 0) {
            write(client_fd, output_buf, header_len);
        }
    } else {
        if (output_len > 0) {
            write(client_fd, output_buf, output_len);
        }
    }

    free(output_buf);
    return 0;
}
