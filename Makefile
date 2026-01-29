# ============================================================================
# HTTP Proxy Makefile
# ============================================================================

CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O3 -march=native -flto
LDFLAGS = -lrt

# Object files
OBJECTS = main.o proxy.o connection.o buffer.o epoll.o http_request.o
TARGET = proxy

# Default target
all: $(TARGET)

# Link all objects into final executable
$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS) $(LDFLAGS)

# Pattern rule for compiling .c files to .o files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Explicit dependencies (so make knows when to recompile)
main.o: main.c proxy.h
proxy.o: proxy.c proxy.h connection.h buffer.h epoll.h http_request.h
connection.o: connection.c connection.h buffer.h epoll.h proxy.h
buffer.o: buffer.c buffer.h
epoll.o: epoll.c epoll.h
http_request.o: http_request.c http_request.h

# Clean build artifacts
clean:
	rm -f $(OBJECTS) $(TARGET)
	@echo "Clean complete"

# Phony targets (not actual files)
.PHONY: all clean