# ============================================================================
# Epoll Proxy Makefile
# ============================================================================

# Compiler and flags
CC = gcc

# Base flags: warnings and C standard
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror

# Optimization flags for release build
# -O3: Aggressive optimization
# -march=native: Optimize for current CPU (use -march=x86-64 for portability)
# -flto: Link-time optimization
OPTFLAGS = -O3 -march=native -flto

# Debug flags
# -g: Debug symbols
# -fsanitize=address: AddressSanitizer for memory errors
# -fsanitize=undefined: UndefinedBehaviorSanitizer
DEBUGFLAGS = -g -O0 -DDEBUG

# Libraries
# -lrt: POSIX real-time extensions (for clock_gettime)
LDFLAGS = -lrt

# Source files
SOURCES = main.c proxy.c connection.c buffer.c epoll.c
HEADERS = config.h proxy.h connection.h buffer.h epoll.h

# Object files
OBJECTS = $(SOURCES:.c=.o)

# Output binary
TARGET = proxy

# Default target: optimized release build
all: CFLAGS += $(OPTFLAGS)
all: $(TARGET)

# Debug build with sanitizers
debug: CFLAGS += $(DEBUGFLAGS)
debug: LDFLAGS += -fsanitize=address -fsanitize=undefined
debug: clean $(TARGET)

# Link the binary
$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Build complete: $(TARGET)"
	@echo "Run with: ./$(TARGET) --help"

# Compile source files
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	rm -f $(OBJECTS) $(TARGET)
	@echo "Clean complete"

# Install to /usr/local/bin (requires root)
install: all
	install -m 755 $(TARGET) /usr/local/bin/$(TARGET)
	@echo "Installed to /usr/local/bin/$(TARGET)"

# Uninstall
uninstall:
	rm -f /usr/local/bin/$(TARGET)
	@echo "Uninstalled from /usr/local/bin"

# Run with default settings (useful for testing)
run: all
	./$(TARGET)

# Format code (requires clang-format)
format:
	clang-format -i $(SOURCES) $(HEADERS)

# Static analysis (requires cppcheck)
analyze:
	cppcheck --enable=all --suppress=missingIncludeSystem $(SOURCES)

# Generate assembly for inspection
asm: main.c
	$(CC) $(CFLAGS) $(OPTFLAGS) -S -fverbose-asm -o main.s main.c

# Show build info
info:
	@echo "Compiler: $(CC)"
	@echo "CFLAGS: $(CFLAGS)"
	@echo "LDFLAGS: $(LDFLAGS)"
	@echo "Sources: $(SOURCES)"
	@echo "Target: $(TARGET)"

.PHONY: all debug clean install uninstall run format analyze asm info