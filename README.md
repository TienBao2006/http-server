# CS631 Advanced Programming in the UNIX Environment: HTTP/1.0 Web Server

An RFC 1945-compliant HTTP/1.0 web server developed in C (C99 standard, strict POSIX compliance). This project demonstrates core UNIX systems programming concepts including process management, socket programming, signal handling, I/O redirection, dæmonization, dynamic CGI execution, and robust security against directory traversal attacks.

---

## Table of Contents
1. [Architectural Overview](#architectural-overview)
2. [Project Structure](#project-structure)
3. [Key Features & Standards Compliance](#key-features--standards-compliance)
   - [Dæmonization & Server Startup](#dæmonization--server-startup)
   - [Multi-Process Concurrency & Zombie Reaping](#multi-process-concurrency--zombie-reaping)
   - [HTTP/1.0 Request Parsing & Methods](#http10-request-parsing--methods)
   - [Response Generation & RFC 1123 Headers](#response-generation--rfc-1123-headers)
   - [HTTP Caching via `If-Modified-Since`](#http-caching-via-if-modified-since)
   - [Security: Directory Traversal Prevention](#security-directory-traversal-prevention)
   - [User Directories (`~username/`)](#user-directories-username)
   - [Dynamic Directory Indexing](#dynamic-directory-indexing)
   - [CGI 1.1 Execution Model](#cgi-11-execution-model)
4. [Building the Server](#building-the-server)
5. [Command-Line Options](#command-line-options)
6. [Testing & Verification Guide](#testing--verification-guide)

---

## Architectural Overview

The server follows the classic UNIX multi-process server design:
```
                    +-----------------------+
                    |  Parent Master Server |
                    |  (Listening Socket)   |
                    +-----------+-----------+
                                |
                   accept() incoming connection
                                |
                             fork()
                                |
            +-------------------+-------------------+
            |                                       |
    [Parent Process]                         [Child Worker]
- Closes client socket                   - Closes listening socket
- Loops back to accept()                 - Parses HTTP/1.0 request
- Reaps zombies asynchronously           - Verifies path security
  via SIGCHLD & waitpid(WNOHANG)         - Serves file / directory / CGI
                                         - Closes client socket & exits
```

---

## Project Structure

```
http-server/
├── Makefile                     # Build automation with strict C99/POSIX flags
├── README.md                    # Comprehensive documentation and manual
├── cgi-bin/
│   └── hello.cgi                # Executable CGI test script
├── www/
│   ├── index.html               # Default landing page
│   └── about.html               # Documentation and project details page
├── include/
│   ├── cgi_handler.h            # CGI execution interfaces
│   ├── directory.h              # Directory traversal & auto-indexing headers
│   ├── file_handler.h           # Path resolution & static file serving
│   ├── http_parser.h            # HTTP request parsing & date parsing
│   ├── response.h               # HTTP/1.0 headers, status codes, and MIME types
│   └── server.h                 # Daemonization, signal handling, and network loop
└── src/
    ├── main.c                   # CLI argument parsing and lifecycle entry point
    ├── server.c                 # Socket setup, fork accept loop, daemonize
    ├── http_parser.c            # Request-Line, headers, and security validation
    ├── response.c               # Response headers, status lines, RFC 1123 dates
    ├── file_handler.c           # Path resolution with ~user & realpath checks
    ├── directory.c              # Auto-indexing with opendir() and readdir()
    └── cgi_handler.c            # CGI process isolation with pipe(), fork(), dup2()
```

---

## Key Features & Standards Compliance

### Dæmonization & Server Startup
Implements standard UNIX dæmonization (Stevens APUE Chapter 13):
1. `umask(0)`: Resets file creation mask so file modes are not inherited from the invoking environment.
2. First `fork()`: Parent exits, placing the child in background.
3. `setsid()`: Detaches from controlling terminal and creates a new session.
4. Second `fork()`: Guarantees that the daemon is not a session leader and can never re-acquire a controlling terminal.
5. `chdir("/")`: Releases any mounted filesystem locks.
6. Absolute Path Resolution: The document root and log file paths are converted to canonical absolute paths via `realpath()` prior to `chdir("/")`.
7. Descriptor Redirection: Standard descriptors (`stdin`, `stdout`, `stderr`) are redirected to `/dev/null` or the designated log file.

### Multi-Process Concurrency & Zombie Reaping
- Per-connection process isolation using `fork()`.
- Signal handling for `SIGCHLD`: A signal action (`sigaction`) with `SA_RESTART` and `SA_NOCLDSTOP` invokes a non-blocking `waitpid(-1, NULL, WNOHANG)` loop to reap all terminated children, completely preventing zombie accumulation while preserving `errno`.
- `SIGPIPE` is ignored (`SIG_IGN`) so broken client connections do not terminate worker processes.

### HTTP/1.0 Request Parsing & Methods
- Strict RFC 1945 parsing of the Request-Line: `Method URI HTTP-Version`.
- Methods supported: `GET`, `HEAD`.
- Any unimplemented HTTP method (such as `POST`, `PUT`, `DELETE`, `OPTIONS`) immediately returns `501 Not Implemented`.
- Invalid or malformed requests return `400 Bad Request`.

### Response Generation & RFC 1123 Headers
- Formats status lines compliant with HTTP/1.0:
  - `200 OK`
  - `304 Not Modified`
  - `400 Bad Request`
  - `403 Forbidden`
  - `404 Not Found`
  - `500 Internal Server Error`
  - `501 Not Implemented`
- Mandatory headers included on every response:
  - `Date`: Formatted using RFC 1123 GMT via `strftime(..., "%a, %d %b %Y %H:%M:%S GMT", ...)`.
  - `Server`: `cs631-httpd/1.0`.
  - `Last-Modified`: Modification time in RFC 1123 GMT.
  - `Content-Type`: Automatically detected MIME type (HTML, CSS, JS, JSON, images, plain text).
  - `Content-Length`: Accurate byte count of the response payload.
  - `Connection: close`: Explicitly marks non-persistent HTTP/1.0 closure.
- `HEAD` requests: Emits exact headers matching `GET`, but suppresses response body transmission.

### HTTP Caching via `If-Modified-Since`
- Parses HTTP-date headers supporting RFC 1123 (`Sun, 06 Nov 1994 08:49:37 GMT`), RFC 850, and ANSI C asctime formats using POSIX `strptime`.
- If the target file modification time `st_mtime` is less than or equal to the `If-Modified-Since` timestamp, the server responds with `304 Not Modified` and zero body bytes.

### Security: Directory Traversal Prevention
- Defense-in-depth security approach:
  1. Lexical URI check: Traversal sequences (`..`) in path segments are detected and blocked before file system calls are made.
  2. URL decoding check: Hex percent encodings (such as `%2e%2e%2f` or null byte injections `%00`) are decoded and verified.
  3. Canonical path boundary check: Using `realpath()`, the resolved file system path must strictly begin with the canonical document root or user directory.
- Any attempt to escape root returns `403 Forbidden`.

### User Directories (`~username/`)
- Requests beginning with `~<user>/...` are mapped using POSIX `getpwnam()` to `<pw_dir>/public_html/...`.
- If user does not exist or access is restricted, `404 Not Found` or `403 Forbidden` is returned.

### Dynamic Directory Indexing
- If a URI maps to a directory:
  - If `index.html` exists inside the directory, it is served automatically.
  - If `index.html` is absent, the directory is read using POSIX `opendir()` and `readdir()`.
  - Generates a styled HTML directory index featuring file names, clickable hyperlinks, file sizes, GMT timestamps, and parent directory `[Parent Directory]` navigation.

### CGI 1.1 Execution Model
- Identifies requests directed to `/cgi-bin/`.
- Extracts query strings into `QUERY_STRING`.
- Uses `pipe()`, `fork()`, and `dup2()` to connect the child's stdout to the server.
- Sets standard CGI/1.1 environment variables:
  `REQUEST_METHOD`, `QUERY_STRING`, `SERVER_PROTOCOL`, `SERVER_SOFTWARE`, `SCRIPT_NAME`, `PATH_INFO`, `REMOTE_ADDR`, `GATEWAY_INTERFACE`, `REQUEST_URI`.
- Executes the script using `execve()` (with fallback to `/bin/sh` for shell scripts returning `ENOEXEC`).

---

## Building the Server

The codebase compiles under strict POSIX C99 flags:
```bash
make clean
make
```

Compiler options configured in the `Makefile`:
```makefile
CC = gcc
CFLAGS = -Wall -Wextra -Werror -pedantic -std=c99 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Iinclude
```

---

## Command-Line Options

```bash
./httpd [OPTIONS]
```

| Option | Argument | Description | Default |
|--------|----------|-------------|---------|
| `-p`   | `<port>` | TCP port to bind and listen on | `8080` |
| `-d`   | `<path>` | Document root directory | `www` |
| `-l`   | `<path>` | File destination for logging output | Terminal / `/dev/null` |
| `-f`   | _None_   | Foreground mode (disables dæmonization) | Daemonized |
| `-h`   | _None_   | Display usage information | - |

---

## Testing & Verification Guide

### 1. Launch Server in Foreground Mode
```bash
./httpd -f -p 8080 -d www
```

### 2. Verify GET Request
```bash
curl -i http://localhost:8080/
```
Expected output:
```http
HTTP/1.0 200 OK
Date: Thu, 29 Oct 2026 02:37:35 GMT
Server: cs631-httpd/1.0
Last-Modified: Thu, 29 Oct 2026 02:36:25 GMT
Content-Type: text/html; charset=utf-8
Content-Length: 5051
Connection: close
```

### 3. Verify HEAD Request (Headers Only)
```bash
curl -I http://localhost:8080/about.html
```
Expected output: Headers returned with accurate `Content-Length`, no body transferred.

### 4. Verify Caching (304 Not Modified)
```bash
curl -i -H 'If-Modified-Since: Fri, 30 Oct 2026 12:00:00 GMT' http://localhost:8080/
```
Expected output:
```http
HTTP/1.0 304 Not Modified
Date: Thu, 29 Oct 2026 02:37:47 GMT
Server: cs631-httpd/1.0
Last-Modified: Thu, 29 Oct 2026 02:36:25 GMT
Connection: close
```

### 5. Verify Directory Auto-Indexing
```bash
mkdir -p www/files && touch www/files/test.txt
curl -i http://localhost:8080/files/
```
Expected output: Returns `Index of /files/` with parent directory and `test.txt` listed.

### 6. Verify CGI 1.1 Script Execution
```bash
curl -i "http://localhost:8080/cgi-bin/hello.cgi?user=student&course=cs631"
```
Expected output: Returns dynamic HTML table detailing all populated CGI environment variables.

### 7. Verify Directory Traversal Security
```bash
curl -i --path-as-is "http://localhost:8080/../Makefile"
curl -i --path-as-is "http://localhost:8080/%2e%2e/Makefile"
```
Expected output:
```http
HTTP/1.0 403 Forbidden
Date: Thu, 29 Oct 2026 02:38:54 GMT
Server: cs631-httpd/1.0
Content-Type: text/html; charset=utf-8
Content-Length: 266
Connection: close
```

### 8. Verify Unsupported Method (501 Not Implemented)
```bash
curl -i -X POST http://localhost:8080/
curl -i -X DELETE http://localhost:8080/index.html
```
Expected output:
```http
HTTP/1.0 501 Not Implemented
Date: Thu, 29 Oct 2026 02:39:20 GMT
Server: cs631-httpd/1.0
```

### 9. Verify Dæmonization
```bash
./httpd -p 8080 -d www -l /tmp/httpd.log
ps -ef | grep httpd
```
The server immediately dissociates from the controlling terminal, redirecting diagnostic output to `/tmp/httpd.log`.
