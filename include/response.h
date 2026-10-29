#ifndef RESPONSE_H
#define RESPONSE_H

#include <sys/types.h>
#include <time.h>

void format_rfc1123_date(time_t t, char *buf, size_t len);
const char *get_mime_type(const char *path);
const char *get_status_reason(int status_code);

void send_headers(int client_fd, int status_code, const char *reason,
                  const char *content_type, off_t content_length,
                  time_t last_modified);

void send_error_response(int client_fd, int status_code, const char *reason,
                         const char *detail, int head_only);

void send_304_response(int client_fd, time_t last_modified);

#endif /* RESPONSE_H */
