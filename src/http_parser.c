#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "http_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int url_decode(const char *src, char *dst, size_t dst_len) {
    size_t si = 0, di = 0;
    if (!src || !dst || dst_len == 0) {
        return -1;
    }

    while (src[si] != '\0' && di + 1 < dst_len) {
        if (src[si] == '%') {
            if (src[si + 1] != '\0' && src[si + 2] != '\0') {
                int h1 = hex_val(src[si + 1]);
                int h2 = hex_val(src[si + 2]);
                if (h1 >= 0 && h2 >= 0) {
                    char decoded = (char)((h1 << 4) | h2);
                    /* Disallow null byte injection */
                    if (decoded == '\0') {
                        return -1;
                    }
                    dst[di++] = decoded;
                    si += 3;
                    continue;
                }
            }
            return -1; /* Malformed hex encoding */
        } else {
            dst[di++] = src[si++];
        }
    }

    if (src[si] != '\0') {
        return -1; /* Buffer overflow */
    }

    dst[di] = '\0';
    return 0;
}

int validate_uri_path(const char *path) {
    const char *p;
    if (!path || path[0] == '\0') {
        return 0;
    }

    /* Path must start with '/' or '~' (for user directory) */
    if (path[0] != '/' && path[0] != '~') {
        return 0;
    }

    p = path;
    while (*p != '\0') {
        /* Skip multiple consecutive slashes */
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        /* Check start of a path segment */
        if (p[0] == '.') {
            if (p[1] == '/' || p[1] == '\0') {
                /* Current directory '.' is harmless, skip */
                p++;
                continue;
            } else if (p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
                /* Directory traversal detected: '..' */
                return 0;
            }
        }

        /* Advance to next segment */
        while (*p != '/' && *p != '\0') {
            p++;
        }
    }

    return 1;
}

/* Custom timegm implementation for strict POSIX C99 compliance */
static time_t my_timegm(struct tm *tm) {
    static const int days_before_month[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };
    int year = tm->tm_year + 1900;
    int month = tm->tm_mon;
    int day = tm->tm_mday;
    int leap_days;
    long days;
    long seconds;

    if (month < 0 || month > 11) {
        return (time_t)-1;
    }

    /* Leap year calculation since 1970 */
    leap_days = (year - 1) / 4 - (year - 1) / 100 + (year - 1) / 400
                - (1969 / 4 - 1969 / 100 + 1969 / 400);

    days = (year - 1970) * 365L + leap_days + days_before_month[month] + (day - 1);
    if (month > 1 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) {
        days += 1;
    }

    seconds = days * 86400L + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec;
    return (time_t)seconds;
}

time_t parse_http_date(const char *date_str) {
    struct tm tm_val;
    char *res;

    if (!date_str) {
        return (time_t)-1;
    }

    /* Try RFC 1123 format: "Sun, 06 Nov 1994 08:49:37 GMT" */
    memset(&tm_val, 0, sizeof(tm_val));
    res = strptime(date_str, "%a, %d %b %Y %H:%M:%S GMT", &tm_val);
    if (res != NULL) {
        return my_timegm(&tm_val);
    }

    /* Try RFC 850 format: "Sunday, 06-Nov-94 08:49:37 GMT" */
    memset(&tm_val, 0, sizeof(tm_val));
    res = strptime(date_str, "%a, %d-%b-%y %H:%M:%S GMT", &tm_val);
    if (res != NULL) {
        return my_timegm(&tm_val);
    }

    /* Try ANSI C asctime format: "Sun Nov  6 08:49:37 1994" */
    memset(&tm_val, 0, sizeof(tm_val));
    res = strptime(date_str, "%a %b %d %H:%M:%S %Y", &tm_val);
    if (res != NULL) {
        return my_timegm(&tm_val);
    }

    return (time_t)-1;
}

int parse_http_request(int client_fd, http_request_t *req) {
    char buffer[MAX_REQUEST_SIZE];
    ssize_t bytes_read = 0;
    size_t total_bytes = 0;
    char *end_of_headers = NULL;
    char *line;
    char *saveptr = NULL;
    char *method_token;
    char *uri_token;
    char *version_token;
    char raw_path[MAX_PATH_LENGTH];
    char *query_start;

    if (!req) {
        return 400;
    }

    memset(req, 0, sizeof(*req));
    req->if_modified_since = (time_t)-1;
    req->has_if_modified_since = 0;

    /* Read HTTP request headers */
    while (total_bytes + 1 < sizeof(buffer)) {
        bytes_read = read(client_fd, buffer + total_bytes, sizeof(buffer) - 1 - total_bytes);
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 400;
        }
        if (bytes_read == 0) {
            break; /* Client closed connection */
        }
        total_bytes += (size_t)bytes_read;
        buffer[total_bytes] = '\0';

        /* Check for end of headers: CRLF CRLF or LF LF */
        end_of_headers = strstr(buffer, "\r\n\r\n");
        if (end_of_headers) {
            break;
        }
        end_of_headers = strstr(buffer, "\n\n");
        if (end_of_headers) {
            break;
        }
    }

    if (total_bytes == 0) {
        return 400; /* Empty request */
    }

    /* Parse Request Line */
    line = strtok_r(buffer, "\r\n", &saveptr);
    if (!line) {
        return 400;
    }

    /* Method */
    method_token = strtok(line, " ");
    if (!method_token) {
        return 400;
    }
    strncpy(req->method_str, method_token, sizeof(req->method_str) - 1);

    if (strcmp(method_token, "GET") == 0) {
        req->method = METHOD_GET;
    } else if (strcmp(method_token, "HEAD") == 0) {
        req->method = METHOD_HEAD;
    } else if (strcmp(method_token, "POST") == 0 ||
               strcmp(method_token, "PUT") == 0 ||
               strcmp(method_token, "DELETE") == 0 ||
               strcmp(method_token, "OPTIONS") == 0 ||
               strcmp(method_token, "TRACE") == 0 ||
               strcmp(method_token, "CONNECT") == 0 ||
               strcmp(method_token, "PATCH") == 0) {
        req->method = METHOD_UNSUPPORTED;
        return 501; /* 501 Not Implemented */
    } else {
        req->method = METHOD_UNKNOWN;
        return 400; /* Bad Request */
    }

    /* URI */
    uri_token = strtok(NULL, " ");
    if (!uri_token) {
        return 400;
    }
    if (strlen(uri_token) >= sizeof(req->raw_uri)) {
        return 400;
    }
    strncpy(req->raw_uri, uri_token, sizeof(req->raw_uri) - 1);

    /* Version */
    version_token = strtok(NULL, " ");
    if (version_token) {
        strncpy(req->version, version_token, sizeof(req->version) - 1);
        /* Check HTTP/1.0 or HTTP/1.1 */
        if (strncmp(req->version, "HTTP/1.", 7) != 0) {
            return 400;
        }
    } else {
        /* Simple Request (HTTP/0.9) - default to HTTP/1.0 */
        strncpy(req->version, "HTTP/1.0", sizeof(req->version) - 1);
    }

    /* Extract Query String from URI */
    query_start = strchr(req->raw_uri, '?');
    if (query_start) {
        size_t path_len = (size_t)(query_start - req->raw_uri);
        if (path_len >= sizeof(raw_path)) {
            return 400;
        }
        memcpy(raw_path, req->raw_uri, path_len);
        raw_path[path_len] = '\0';
        strncpy(req->query_string, query_start + 1, sizeof(req->query_string) - 1);
    } else {
        strncpy(raw_path, req->raw_uri, sizeof(raw_path) - 1);
        req->query_string[0] = '\0';
    }

    /* URL Decode the path */
    if (url_decode(raw_path, req->path, sizeof(req->path)) != 0) {
        return 400;
    }

    /* Security check: Directory Traversal */
    if (!validate_uri_path(req->path)) {
        return 403; /* 403 Forbidden */
    }

    /* Parse Headers (looking for If-Modified-Since) */
    while ((line = strtok_r(NULL, "\r\n", &saveptr)) != NULL) {
        if (line[0] == '\0') {
            break;
        }
        if (strncasecmp(line, "If-Modified-Since:", 18) == 0) {
            char *val = line + 18;
            while (*val == ' ' || *val == '\t') {
                val++;
            }
            req->if_modified_since = parse_http_date(val);
            if (req->if_modified_since != (time_t)-1) {
                req->has_if_modified_since = 1;
            }
        }
    }

    return 0;
}
