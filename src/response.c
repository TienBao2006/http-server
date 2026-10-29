#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "response.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>

void format_rfc1123_date(time_t t, char *buf, size_t len) {
    struct tm tm_utc;
    if (gmtime_r(&t, &tm_utc) != NULL) {
        strftime(buf, len, "%a, %d %b %Y %H:%M:%S GMT", &tm_utc);
    } else {
        if (len > 0) {
            buf[0] = '\0';
        }
    }
}

const char *get_status_reason(int status_code) {
    switch (status_code) {
        case 200: return "OK";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        default:  return "Unknown";
    }
}

const char *get_mime_type(const char *path) {
    const char *ext;
    if (!path) return "application/octet-stream";

    ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0) {
        return "text/html; charset=utf-8";
    } else if (strcasecmp(ext, ".css") == 0) {
        return "text/css";
    } else if (strcasecmp(ext, ".js") == 0) {
        return "application/javascript";
    } else if (strcasecmp(ext, ".json") == 0) {
        return "application/json";
    } else if (strcasecmp(ext, ".txt") == 0 || strcasecmp(ext, ".c") == 0 ||
               strcasecmp(ext, ".h") == 0 || strcasecmp(ext, ".log") == 0) {
        return "text/plain; charset=utf-8";
    } else if (strcasecmp(ext, ".png") == 0) {
        return "image/png";
    } else if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) {
        return "image/jpeg";
    } else if (strcasecmp(ext, ".gif") == 0) {
        return "image/gif";
    } else if (strcasecmp(ext, ".svg") == 0) {
        return "image/svg+xml";
    } else if (strcasecmp(ext, ".pdf") == 0) {
        return "application/pdf";
    } else if (strcasecmp(ext, ".ico") == 0) {
        return "image/x-icon";
    }

    return "application/octet-stream";
}

void send_headers(int client_fd, int status_code, const char *reason,
                  const char *content_type, off_t content_length,
                  time_t last_modified) {
    char header_buf[1024];
    char date_buf[64];
    char lm_buf[64];
    int len;
    time_t now = time(NULL);

    format_rfc1123_date(now, date_buf, sizeof(date_buf));

    if (last_modified > 0) {
        format_rfc1123_date(last_modified, lm_buf, sizeof(lm_buf));
        len = snprintf(header_buf, sizeof(header_buf),
                       "HTTP/1.0 %d %s\r\n"
                       "Date: %s\r\n"
                       "Server: %s\r\n"
                       "Last-Modified: %s\r\n"
                       "Content-Type: %s\r\n"
                       "Content-Length: %ld\r\n"
                       "Connection: close\r\n\r\n",
                       status_code, reason, date_buf, SERVER_NAME,
                       lm_buf, content_type, (long)content_length);
    } else {
        len = snprintf(header_buf, sizeof(header_buf),
                       "HTTP/1.0 %d %s\r\n"
                       "Date: %s\r\n"
                       "Server: %s\r\n"
                       "Content-Type: %s\r\n"
                       "Content-Length: %ld\r\n"
                       "Connection: close\r\n\r\n",
                       status_code, reason, date_buf, SERVER_NAME,
                       content_type, (long)content_length);
    }

    if (len > 0) {
        write(client_fd, header_buf, (size_t)len);
    }
}

void send_304_response(int client_fd, time_t last_modified) {
    char header_buf[512];
    char date_buf[64];
    char lm_buf[64];
    int len;
    time_t now = time(NULL);

    format_rfc1123_date(now, date_buf, sizeof(date_buf));
    format_rfc1123_date(last_modified, lm_buf, sizeof(lm_buf));

    len = snprintf(header_buf, sizeof(header_buf),
                   "HTTP/1.0 304 Not Modified\r\n"
                   "Date: %s\r\n"
                   "Server: %s\r\n"
                   "Last-Modified: %s\r\n"
                   "Connection: close\r\n\r\n",
                   date_buf, SERVER_NAME, lm_buf);

    if (len > 0) {
        write(client_fd, header_buf, (size_t)len);
    }
}

void send_error_response(int client_fd, int status_code, const char *reason,
                         const char *detail, int head_only) {
    char body[1024];
    int body_len;

    if (!reason) {
        reason = get_status_reason(status_code);
    }

    body_len = snprintf(body, sizeof(body),
                        "<!DOCTYPE html>\n"
                        "<html>\n"
                        "<head><title>%d %s</title></head>\n"
                        "<body style=\"font-family:sans-serif;text-align:center;padding:50px;\">\n"
                        "<h1>%d %s</h1>\n"
                        "<p>%s</p>\n"
                        "<hr>\n"
                        "<small>%s</small>\n"
                        "</body>\n"
                        "</html>\n",
                        status_code, reason, status_code, reason,
                        detail ? detail : reason, SERVER_NAME);

    send_headers(client_fd, status_code, reason,
                 "text/html; charset=utf-8",
                 body_len > 0 ? (off_t)body_len : 0, 0);

    if (!head_only && body_len > 0) {
        write(client_fd, body, (size_t)body_len);
    }
}
