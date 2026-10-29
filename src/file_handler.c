#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "file_handler.h"
#include "response.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>
#include <errno.h>
#include <sys/stat.h>
#include <limits.h>

int resolve_path(const char *docroot, const char *uri_path, char *resolved_path, size_t max_len) {
    char base_dir[PATH_MAX];
    char rel_path[PATH_MAX];
    char temp_path[PATH_MAX];
    char canonical_base[PATH_MAX];
    char canonical_resolved[PATH_MAX];
    const char *p = uri_path;

    if (!docroot || !uri_path || !resolved_path || max_len == 0) {
        return -1;
    }

    /* Skip leading slash */
    while (*p == '/') {
        p++;
    }

    /* Check for User Directory: ~username */
    if (*p == '~') {
        char username[128];
        const char *slash;
        struct passwd *pw;
        size_t user_len;

        p++; /* Skip '~' */
        slash = strchr(p, '/');
        if (slash) {
            user_len = (size_t)(slash - p);
            if (user_len >= sizeof(username)) {
                return -1;
            }
            memcpy(username, p, user_len);
            username[user_len] = '\0';
            p = slash + 1;
        } else {
            strncpy(username, p, sizeof(username) - 1);
            username[sizeof(username) - 1] = '\0';
            p = "";
        }

        /* Look up user account */
        pw = getpwnam(username);
        if (!pw) {
            return -1; /* User does not exist */
        }

        /* Map to <pw_dir>/public_html */
        if (snprintf(base_dir, sizeof(base_dir), "%s/public_html", pw->pw_dir) >= (int)sizeof(base_dir)) {
            return -1;
        }
        strncpy(rel_path, p, sizeof(rel_path) - 1);
        rel_path[sizeof(rel_path) - 1] = '\0';
    } else {
        /* Standard docroot mapping */
        strncpy(base_dir, docroot, sizeof(base_dir) - 1);
        base_dir[sizeof(base_dir) - 1] = '\0';

        strncpy(rel_path, p, sizeof(rel_path) - 1);
        rel_path[sizeof(rel_path) - 1] = '\0';
    }

    /* Combine base_dir and rel_path */
    if (rel_path[0] != '\0') {
        if (snprintf(temp_path, sizeof(temp_path), "%s/%s", base_dir, rel_path) >= (int)sizeof(temp_path)) {
            return -1;
        }
    } else {
        strncpy(temp_path, base_dir, sizeof(temp_path) - 1);
        temp_path[sizeof(temp_path) - 1] = '\0';
    }

    /* Ensure canonical base_dir */
    if (realpath(base_dir, canonical_base) == NULL) {
        /* If base_dir does not exist, copy temp_path and let caller stat fail with 404 */
        strncpy(resolved_path, temp_path, max_len - 1);
        resolved_path[max_len - 1] = '\0';
        return 0;
    }

    /* If the target exists, verify canonical path starts with canonical_base */
    if (realpath(temp_path, canonical_resolved) != NULL) {
        size_t base_len = strlen(canonical_base);
        if (strncmp(canonical_resolved, canonical_base, base_len) != 0 ||
            (canonical_resolved[base_len] != '\0' && canonical_resolved[base_len] != '/')) {
            /* Directory traversal detected */
            return -1;
        }
        strncpy(resolved_path, canonical_resolved, max_len - 1);
        resolved_path[max_len - 1] = '\0';
    } else {
        /* Target may not exist yet, pass temp_path */
        strncpy(resolved_path, temp_path, max_len - 1);
        resolved_path[max_len - 1] = '\0';
    }

    return 0;
}

int serve_file(int client_fd, const http_request_t *req, const char *file_path, const struct stat *st) {
    const char *mime_type;
    int file_fd;
    char buffer[8192];
    ssize_t n_read;
    ssize_t n_written;
    ssize_t total_written;

    if (!req || !file_path || !st) {
        send_error_response(client_fd, 500, "Internal Server Error", "Invalid arguments to serve_file", 0);
        return -1;
    }

    /* Check read permission */
    if (access(file_path, R_OK) != 0) {
        if (errno == EACCES) {
            send_error_response(client_fd, 403, "Forbidden", "You do not have permission to access this resource.", req->method == METHOD_HEAD);
            return 0;
        } else {
            send_error_response(client_fd, 404, "Not Found", "The requested resource was not found.", req->method == METHOD_HEAD);
            return 0;
        }
    }

    /* Check If-Modified-Since */
    if (req->has_if_modified_since) {
        if (st->st_mtime <= req->if_modified_since) {
            send_304_response(client_fd, st->st_mtime);
            return 0;
        }
    }

    file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        if (errno == EACCES) {
            send_error_response(client_fd, 403, "Forbidden", "Permission denied.", req->method == METHOD_HEAD);
        } else if (errno == ENOENT) {
            send_error_response(client_fd, 404, "Not Found", "File not found.", req->method == METHOD_HEAD);
        } else {
            send_error_response(client_fd, 500, "Internal Server Error", "Failed to open requested file.", req->method == METHOD_HEAD);
        }
        return 0;
    }

    mime_type = get_mime_type(file_path);
    send_headers(client_fd, 200, "OK", mime_type, st->st_size, st->st_mtime);

    /* For HEAD requests, headers only */
    if (req->method == METHOD_HEAD) {
        close(file_fd);
        return 0;
    }

    /* Stream file contents for GET */
    while ((n_read = read(file_fd, buffer, sizeof(buffer))) > 0) {
        total_written = 0;
        while (total_written < n_read) {
            n_written = write(client_fd, buffer + total_written, (size_t)(n_read - total_written));
            if (n_written <= 0) {
                if (n_written < 0 && errno == EINTR) {
                    continue;
                }
                /* Broken connection */
                close(file_fd);
                return -1;
            }
            total_written += n_written;
        }
    }

    close(file_fd);
    return 0;
}
