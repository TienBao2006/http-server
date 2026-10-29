#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "directory.h"
#include "file_handler.h"
#include "response.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>

typedef struct dir_entry_info {
    char name[256];
    int is_dir;
    off_t size;
    time_t mtime;
} dir_entry_info_t;

static int entry_cmp(const void *a, const void *b) {
    const dir_entry_info_t *ea = (const dir_entry_info_t *)a;
    const dir_entry_info_t *eb = (const dir_entry_info_t *)b;

    /* Directories first */
    if (ea->is_dir != eb->is_dir) {
        return eb->is_dir - ea->is_dir;
    }
    return strcmp(ea->name, eb->name);
}

typedef struct str_builder {
    char *data;
    size_t len;
    size_t cap;
} str_builder_t;

static int sb_init(str_builder_t *sb, size_t initial_cap) {
    sb->data = malloc(initial_cap);
    if (!sb->data) return -1;
    sb->len = 0;
    sb->cap = initial_cap;
    sb->data[0] = '\0';
    return 0;
}

static int sb_append(str_builder_t *sb, const char *str) {
    size_t slen = strlen(str);
    if (sb->len + slen + 1 > sb->cap) {
        size_t new_cap = (sb->cap + slen + 1024) * 2;
        char *new_data = realloc(sb->data, new_cap);
        if (!new_data) return -1;
        sb->data = new_data;
        sb->cap = new_cap;
    }
    memcpy(sb->data + sb->len, str, slen);
    sb->len += slen;
    sb->data[sb->len] = '\0';
    return 0;
}

static int sb_append_printf(str_builder_t *sb, const char *fmt, ...) {
    char tmp[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    return sb_append(sb, tmp);
}

static void sb_free(str_builder_t *sb) {
    if (sb->data) {
        free(sb->data);
        sb->data = NULL;
    }
    sb->len = 0;
    sb->cap = 0;
}

int serve_directory(int client_fd, const http_request_t *req,
                    const char *dir_path, const char *uri_path) {
    char index_path[1024];
    struct stat st;
    DIR *dir;
    struct dirent *de;
    dir_entry_info_t *entries = NULL;
    size_t entry_count = 0;
    size_t entry_cap = 64;
    str_builder_t sb;
    size_t i;
    char time_buf[64];
    char normalized_uri[1024];
    size_t ulen;

    /* 1. Check if index.html exists */
    if (snprintf(index_path, sizeof(index_path), "%s/index.html", dir_path) < (int)sizeof(index_path)) {
        if (stat(index_path, &st) == 0 && S_ISREG(st.st_mode)) {
            return serve_file(client_fd, req, index_path, &st);
        }
    }

    /* 2. Open directory */
    dir = opendir(dir_path);
    if (!dir) {
        if (errno == EACCES) {
            send_error_response(client_fd, 403, "Forbidden", "Cannot read directory.", req->method == METHOD_HEAD);
        } else {
            send_error_response(client_fd, 404, "Not Found", "Directory not found.", req->method == METHOD_HEAD);
        }
        return 0;
    }

    entries = malloc(entry_cap * sizeof(dir_entry_info_t));
    if (!entries) {
        closedir(dir);
        send_error_response(client_fd, 500, "Internal Server Error", "Memory allocation failed.", req->method == METHOD_HEAD);
        return -1;
    }

    while ((de = readdir(dir)) != NULL) {
        char full_entry_path[1024];
        struct stat entry_st;

        /* Skip '.' */
        if (strcmp(de->d_name, ".") == 0) {
            continue;
        }

        if (snprintf(full_entry_path, sizeof(full_entry_path), "%s/%s", dir_path, de->d_name) >= (int)sizeof(full_entry_path)) {
            continue;
        }

        if (stat(full_entry_path, &entry_st) != 0) {
            continue;
        }

        if (entry_count >= entry_cap) {
            entry_cap *= 2;
            dir_entry_info_t *new_entries = realloc(entries, entry_cap * sizeof(dir_entry_info_t));
            if (!new_entries) {
                break;
            }
            entries = new_entries;
        }

        strncpy(entries[entry_count].name, de->d_name, sizeof(entries[entry_count].name) - 1);
        entries[entry_count].name[sizeof(entries[entry_count].name) - 1] = '\0';
        entries[entry_count].is_dir = S_ISDIR(entry_st.st_mode);
        entries[entry_count].size = entry_st.st_size;
        entries[entry_count].mtime = entry_st.st_mtime;
        entry_count++;
    }
    closedir(dir);

    /* Sort entries */
    if (entry_count > 1) {
        qsort(entries, entry_count, sizeof(dir_entry_info_t), entry_cmp);
    }

    /* Build HTML page */
    if (sb_init(&sb, 4096) != 0) {
        free(entries);
        send_error_response(client_fd, 500, "Internal Server Error", "Memory error.", req->method == METHOD_HEAD);
        return -1;
    }

    /* Ensure normalized_uri ends with slash */
    strncpy(normalized_uri, uri_path, sizeof(normalized_uri) - 2);
    normalized_uri[sizeof(normalized_uri) - 2] = '\0';
    ulen = strlen(normalized_uri);
    if (ulen == 0 || normalized_uri[ulen - 1] != '/') {
        normalized_uri[ulen] = '/';
        normalized_uri[ulen + 1] = '\0';
    }

    sb_append_printf(&sb,
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "  <meta charset=\"utf-8\">\n"
        "  <title>Index of %s</title>\n"
        "  <style>\n"
        "    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; background: #0f172a; color: #f8fafc; margin: 0; padding: 40px; }\n"
        "    h1 { font-size: 24px; font-weight: 600; color: #38bdf8; border-bottom: 1px solid #334155; padding-bottom: 12px; }\n"
        "    table { width: 100%%; border-collapse: collapse; margin-top: 20px; font-size: 14px; }\n"
        "    th { text-align: left; padding: 12px; border-bottom: 2px solid #334155; color: #94a3b8; text-transform: uppercase; font-size: 12px; letter-spacing: 0.05em; }\n"
        "    td { padding: 12px; border-bottom: 1px solid #1e293b; }\n"
        "    tr:hover td { background-color: #1e293b; }\n"
        "    a { color: #60a5fa; text-decoration: none; font-weight: 500; }\n"
        "    a:hover { text-decoration: underline; color: #93c5fd; }\n"
        "    .size, .date { color: #94a3b8; font-family: monospace; font-size: 13px; }\n"
        "    .icon { margin-right: 8px; }\n"
        "    .footer { margin-top: 30px; font-size: 12px; color: #64748b; border-top: 1px solid #334155; padding-top: 10px; }\n"
        "  </style>\n"
        "</head>\n"
        "<body>\n"
        "  <h1>Index of %s</h1>\n"
        "  <table>\n"
        "    <tr><th>Name</th><th>Last Modified</th><th>Size</th></tr>\n",
        normalized_uri, normalized_uri);

    /* Parent directory link if not root */
    if (strcmp(normalized_uri, "/") != 0) {
        sb_append(&sb, "    <tr><td><span class=\"icon\">📁</span><a href=\"..\">[Parent Directory]</a></td><td>-</td><td class=\"size\">-</td></tr>\n");
    }

    for (i = 0; i < entry_count; i++) {
        struct tm tm_info;
        if (strcmp(entries[i].name, "..") == 0) {
            continue; /* Handled above */
        }

        if (gmtime_r(&entries[i].mtime, &tm_info)) {
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S GMT", &tm_info);
        } else {
            strcpy(time_buf, "-");
        }

        if (entries[i].is_dir) {
            sb_append_printf(&sb,
                "    <tr><td><span class=\"icon\">📁</span><a href=\"%s%s/\">%s/</a></td><td class=\"date\">%s</td><td class=\"size\">-</td></tr>\n",
                normalized_uri, entries[i].name, entries[i].name, time_buf);
        } else {
            sb_append_printf(&sb,
                "    <tr><td><span class=\"icon\">📄</span><a href=\"%s%s\">%s</a></td><td class=\"date\">%s</td><td class=\"size\">%ld B</td></tr>\n",
                normalized_uri, entries[i].name, entries[i].name, time_buf, (long)entries[i].size);
        }
    }

    sb_append_printf(&sb,
        "  </table>\n"
        "  <div class=\"footer\">%s HTTP/1.0 Server</div>\n"
        "</body>\n"
        "</html>\n",
        SERVER_NAME);

    free(entries);

    /* Send response */
    send_headers(client_fd, 200, "OK", "text/html; charset=utf-8", (off_t)sb.len, 0);

    if (req->method != METHOD_HEAD && sb.len > 0) {
        ssize_t written = 0;
        size_t total = 0;
        while (total < sb.len) {
            written = write(client_fd, sb.data + total, sb.len - total);
            if (written <= 0) {
                if (written < 0 && errno == EINTR) {
                    continue;
                }
                break;
            }
            total += (size_t)written;
        }
    }

    sb_free(&sb);
    return 0;
}
