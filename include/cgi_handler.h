#ifndef CGI_HANDLER_H
#define CGI_HANDLER_H

#include "http_parser.h"

/*
 * Returns 1 if uri_path represents a CGI script (e.g. starts with /cgi-bin/), 0 otherwise.
 */
int is_cgi_request(const char *uri_path);

/*
 * Executes CGI script via pipe(), fork(), dup2(), and execve().
 * Sets up CGI environment variables.
 * Sends HTTP/1.0 response with CGI output to client_fd.
 * Returns 0 on success, -1 on error.
 */
int handle_cgi(int client_fd, const http_request_t *req,
               const char *script_path, const char *client_ip);

#endif /* CGI_HANDLER_H */
