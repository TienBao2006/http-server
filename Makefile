CC = gcc
CFLAGS = -Wall -Wextra -Werror -pedantic -std=c99 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Iinclude
LDFLAGS =

TARGET = httpd
SRCS = src/main.c \
       src/server.c \
       src/http_parser.c \
       src/response.c \
       src/file_handler.c \
       src/directory.c \
       src/cgi_handler.c

OBJS = $(SRCS:.c=.o)

.PHONY: all clean run test

all: $(TARGET)
	@chmod +x cgi-bin/hello.cgi 2>/dev/null || true

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

run: all
	./$(TARGET) -f -p 8080 -d www
