# Compiler and Flags
CXX      := g++
# -O3: Max optimization
# -march=native: Use AVX/SSE instructions specific to YOUR cpu (critical for checksums)
# -Wall -Wextra: Show all warnings
# -std=c++20: Modern C++ features
CXXFLAGS := -O3 -march=native -Wall -Wextra -std=c++20 -g -Iinclude

# Libraries to link
# -luring: The IO Ring library
# -lfmt: Fast formatting logging
# -lcrypto: OpenSSL for checksums
LDFLAGS  := -luring -lfmt -lcrypto

# Directories
SRC_DIR   := src
OBJ_DIR   := obj
BIN_DIR   := bin
UNIT_DIR  := tests/unit
E2E_DIR   := tests/e2e
PERF_DIR  := tests/perf

# Target Executable Name
TARGET  := $(BIN_DIR)/uring-sync

# Auto-detect all .cpp files in src/
SRCS    := $(wildcard $(SRC_DIR)/*.cpp)
# Create corresponding .o file paths
OBJS    := $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(SRCS))

# Unit test configuration
UNIT_SRCS    := $(wildcard $(UNIT_DIR)/*.cpp)
UNIT_OBJS    := $(patsubst $(UNIT_DIR)/%.cpp, $(OBJ_DIR)/unit_%.o, $(UNIT_SRCS))
UNIT_TARGET  := $(BIN_DIR)/test_runner
# Tests need liburing for RingManager tests, libfmt for utils tests
UNIT_LDFLAGS := -lgtest -lgtest_main -lpthread -luring -lfmt

# Default Rule
all: $(TARGET)

# Link Rule
$(TARGET): $(OBJS) | $(BIN_DIR)
	@echo "Linking $@"
	@$(CXX) $(OBJS) -o $@ $(LDFLAGS)

# Compilation Rule
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	@echo "Compiling $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# Unit test compilation rule
$(OBJ_DIR)/unit_%.o: $(UNIT_DIR)/%.cpp | $(OBJ_DIR)
	@echo "Compiling unit test $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# Unit test target
$(UNIT_TARGET): $(UNIT_OBJS) | $(BIN_DIR)
	@echo "Linking $@"
	@$(CXX) $(UNIT_OBJS) -o $@ $(UNIT_LDFLAGS)

# Build and run unit tests
test: $(UNIT_TARGET)
	@echo "Running unit tests..."
	@$(UNIT_TARGET)

# Build unit tests only (without running)
build-tests: $(UNIT_TARGET)

# Run end-to-end tests (requires binary to be built)
e2e: $(TARGET)
	@echo "Running end-to-end tests..."
	@$(E2E_DIR)/e2e_tests.sh

# Run performance tests (requires binary to be built)
perf: $(TARGET)
	@echo "Running performance tests..."
	@$(PERF_DIR)/bench.sh

# Run all tests (unit + e2e)
test-all: test e2e

# Create directories if they don't exist
$(BIN_DIR) $(OBJ_DIR):
	@mkdir -p $@

# Clean Rule
clean:
	@rm -rf $(OBJ_DIR) $(BIN_DIR)
	@echo "Cleaned build artifacts"

.PHONY: all clean test build-tests e2e perf test-all
