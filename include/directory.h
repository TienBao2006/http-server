#ifndef DIRECTORY_H
#define DIRECTORY_H

#include "http_parser.h"

/*
 * Checks if index.html exists in dir_path.
 * If yes, serves index.html.
 * Otherwise, scans directory with opendir/readdir and generates an HTML index.
 * Handles HEAD and GET methods.
 */
int serve_directory(int client_fd, const http_request_t *req,
                    const char *dir_path, const char *uri_path);

#endif /* DIRECTORY_H */
