#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stddef.h>
#include <time.h>

#define MAX_REQUEST_SIZE 8192
#define MAX_URI_LENGTH 2048
#define MAX_PATH_LENGTH 1024
#define MAX_QUERY_LENGTH 1024

typedef enum http_method {
    METHOD_UNKNOWN = 0,
    METHOD_GET,
    METHOD_HEAD,
    METHOD_UNSUPPORTED
} http_method_t;

typedef struct http_request {
    http_method_t method;
    char method_str[16];
    char raw_uri[MAX_URI_LENGTH];
    char path[MAX_PATH_LENGTH];
    char query_string[MAX_QUERY_LENGTH];
    char version[16];
    int has_if_modified_since;
    time_t if_modified_since;
} http_request_t;

/*
 * Returns 0 on success, or an HTTP error status code:
 * 400 (Bad Request), 501 (Not Implemented).
 */
int parse_http_request(int client_fd, http_request_t *req);

/*
 * URL decodes src into dst. Returns 0 on success, -1 on invalid hex.
 */
int url_decode(const char *src, char *dst, size_t dst_len);

/*
 * Validates path security (rejects '..' traversal segments).
 * Returns 1 if safe, 0 if path contains directory traversal.
 */
int validate_uri_path(const char *path);

/*
 * Parses RFC 1123, RFC 850, or asctime HTTP-date format into time_t (UTC).
 * Returns (time_t)-1 on parse failure.
 */
time_t parse_http_date(const char *date_str);

#endif /* HTTP_PARSER_H */
