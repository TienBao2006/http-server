#ifndef FILE_HANDLER_H
#define FILE_HANDLER_H

#include <sys/types.h>
#include <sys/stat.h>
#include "http_parser.h"

/*
 * Resolves URI path to physical file path.
 * Supports ~user/ mappings via getpwnam() to <pw_dir>/public_html/.
 * Performs directory traversal validation.
 * Returns 0 on success, -1 on traversal or resolution failure.
 */
int resolve_path(const char *docroot, const char *uri_path, char *resolved_path, size_t max_len);

/*
 * Handles file request (HEAD or GET).
 * Checks If-Modified-Since for 304 Not Modified.
 * Sends headers and (for GET) file body.
 */
int serve_file(int client_fd, const http_request_t *req, const char *file_path, const struct stat *st);

#endif /* FILE_HANDLER_H */
