# High-Performance Epoll Proxy - Makefile
# Optimized for production use and easy development

# ============================================================================
# CONFIGURATION
# ============================================================================

# Compiler and tools
CC := gcc
AR := ar
INSTALL := install

# Directories
SRC_DIR := src
INC_DIR := include
BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin
TEST_DIR := tests

# Output binaries
TARGET := $(BIN_DIR)/epoll-proxy
TEST_TARGET := $(BIN_DIR)/test-proxy

# ============================================================================
# COMPILER FLAGS
# ============================================================================

# Base flags - always enabled
CFLAGS := -Wall -Wextra -Werror -std=c11 -I$(INC_DIR)
LDFLAGS := -pthread

# Development flags (make DEBUG=1)
ifdef DEBUG
    CFLAGS += -g -O0 -DDEBUG
    CFLAGS += -fsanitize=address -fsanitize=undefined
    LDFLAGS += -fsanitize=address -fsanitize=undefined
else
    # Production flags - maximum performance
    CFLAGS += -O3 -march=native -flto
    CFLAGS += -DNDEBUG
    CFLAGS += -fomit-frame-pointer
    CFLAGS += -funroll-loops
    LDFLAGS += -flto
endif

# Profile-guided optimization (make PROFILE=1)
ifdef PROFILE
    CFLAGS += -fprofile-generate
    LDFLAGS += -fprofile-generate
endif

# Use profile data (make USE_PROFILE=1)
ifdef USE_PROFILE
    CFLAGS += -fprofile-use -fprofile-correction
    LDFLAGS += -fprofile-use
endif

# Additional warnings for clean code
CFLAGS += -Wshadow -Wpointer-arith -Wcast-qual -Wstrict-prototypes
CFLAGS += -Wmissing-prototypes -Wno-unused-parameter

# ============================================================================
# SOURCE FILES
# ============================================================================

# Find all .c files in src/ subdirectories
SOURCES := $(shell find $(SRC_DIR) -name '*.c')
OBJECTS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SOURCES))

# Test files
TEST_SOURCES := $(shell find $(TEST_DIR) -name '*.c' 2>/dev/null)
TEST_OBJECTS := $(patsubst $(TEST_DIR)/%.c,$(OBJ_DIR)/tests/%.o,$(TEST_SOURCES))

# ============================================================================
# TARGETS
# ============================================================================

.PHONY: all clean install uninstall test benchmark help

# Default target
all: $(TARGET)

# Main binary
$(TARGET): $(OBJECTS) | $(BIN_DIR)
	@echo "🔗 Linking $@"
	@$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	@echo "✅ Build complete: $@"

# Compile source files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	@echo "📦 Compiling $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Compile test files
$(OBJ_DIR)/tests/%.o: $(TEST_DIR)/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	@echo "📦 Compiling test $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Create build directories
$(BIN_DIR) $(OBJ_DIR):
	@mkdir -p $@

# ============================================================================
# DEVELOPMENT TARGETS
# ============================================================================

# Debug build
debug:
	@$(MAKE) DEBUG=1

# Run with valgrind
valgrind: debug
	valgrind --leak-check=full --show-leak-kinds=all $(TARGET)

# Profile-guided optimization (2-step process)
pgo:
	@echo "Step 1: Building with profiling..."
	@$(MAKE) clean
	@$(MAKE) PROFILE=1
	@echo "Step 2: Run the binary with representative workload, then run 'make pgo-use'"

pgo-use:
	@echo "Step 3: Rebuilding with profile data..."
	@$(MAKE) clean
	@$(MAKE) USE_PROFILE=1
	@echo "✅ PGO build complete"

# ============================================================================
# TESTING
# ============================================================================

test: $(TEST_TARGET)
	@echo "🧪 Running tests..."
	@$(TEST_TARGET)

$(TEST_TARGET): $(TEST_OBJECTS) $(filter-out $(OBJ_DIR)/core/main.o,$(OBJECTS)) | $(BIN_DIR)
	@echo "🔗 Linking tests"
	@$(CC) $^ $(LDFLAGS) -o $@

# ============================================================================
# BENCHMARKING
# ============================================================================

benchmark: $(TARGET)
	@echo "⚡ Running benchmarks..."
	@echo ""
	@echo "Starting backend server on port 8081..."
	@python3 -m http.server 8081 > /dev/null 2>&1 & echo $$! > /tmp/backend.pid
	@sleep 1
	@echo "Starting proxy..."
	@$(TARGET) -m http > /dev/null 2>&1 & echo $$! > /tmp/proxy.pid
	@sleep 1
	@echo ""
	@echo "Running wrk benchmark (30 seconds)..."
	@wrk -t4 -c100 -d30s http://localhost:8080 || true
	@echo ""
	@echo "Cleaning up..."
	@kill `cat /tmp/proxy.pid` 2>/dev/null || true
	@kill `cat /tmp/backend.pid` 2>/dev/null || true
	@rm -f /tmp/proxy.pid /tmp/backend.pid

# Quick performance test
perf: $(TARGET)
	@echo "⚡ Quick performance test..."
	@python3 -m http.server 8081 > /dev/null 2>&1 & echo $$! > /tmp/backend.pid
	@sleep 1
	@$(TARGET) -m http > /dev/null 2>&1 & echo $$! > /tmp/proxy.pid
	@sleep 1
	@wrk -t2 -c50 -d10s http://localhost:8080 || true
	@kill `cat /tmp/proxy.pid` 2>/dev/null || true
	@kill `cat /tmp/backend.pid` 2>/dev/null || true
	@rm -f /tmp/proxy.pid /tmp/backend.pid

# ============================================================================
# INSTALLATION
# ============================================================================

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

install: $(TARGET)
	@echo "📥 Installing to $(BINDIR)"
	@$(INSTALL) -d $(BINDIR)
	@$(INSTALL) -m 755 $(TARGET) $(BINDIR)/epoll-proxy
	@echo "✅ Installed: $(BINDIR)/epoll-proxy"

uninstall:
	@echo "🗑️  Uninstalling..."
	@rm -f $(BINDIR)/epoll-proxy
	@echo "✅ Uninstalled"

# ============================================================================
# CLEANUP
# ============================================================================

clean:
	@echo "🧹 Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR)
	@rm -f *.o *.gcda *.gcno
	@echo "✅ Clean complete"

distclean: clean
	@echo "🧹 Deep clean..."
	@rm -f $(TARGET) $(TEST_TARGET)
	@find . -name '*~' -delete
	@find . -name '*.swp' -delete

# ============================================================================
# HELP
# ============================================================================

help:
	@echo "Epoll Proxy - Makefile Targets"
	@echo ""
	@echo "Building:"
	@echo "  make              - Build optimized binary"
	@echo "  make debug        - Build with debug symbols and sanitizers"
	@echo "  make pgo          - Profile-guided optimization (step 1)"
	@echo "  make pgo-use      - Use PGO profile data (step 2)"
	@echo ""
	@echo "Testing:"
	@echo "  make test         - Run unit tests"
	@echo "  make benchmark    - Run full benchmark suite"
	@echo "  make perf         - Quick performance test"
	@echo "  make valgrind     - Run with memory checker"
	@echo ""
	@echo "Installation:"
	@echo "  make install      - Install to $(PREFIX)"
	@echo "  make uninstall    - Remove installation"
	@echo ""
	@echo "Cleaning:"
	@echo "  make clean        - Remove build artifacts"
	@echo "  make distclean    - Deep clean"
	@echo ""
	@echo "Variables:"
	@echo "  DEBUG=1           - Enable debug build"
	@echo "  PREFIX=/path      - Installation prefix"

# ============================================================================
# DEPENDENCIES
# ============================================================================

# Auto-generate dependencies
-include $(OBJECTS:.o=.d)

$(OBJ_DIR)/%.d: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -MM -MT $(OBJ_DIR)/$*.o $< > $@

