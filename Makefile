# =============================================================================
# FluxServer Makefile
# Profiles: release (default) | debug | asan | tsan | ubsan
# Targets:  all clean test bench format tidy compile_commands
# =============================================================================

PROFILE ?= release

CC      ?= gcc
CSTD     = -std=c11
INCLUDES = -Iinclude

WARN = -Wall -Wextra -Werror -Wshadow -Wpointer-arith -Wcast-align \
       -Wwrite-strings -Wmissing-prototypes -Wstrict-prototypes \
       -Wno-unused-parameter -Wno-unused-result -Wformat=2

BASE_CFLAGS  = $(CSTD) $(WARN) $(INCLUDES) -D_GNU_SOURCE -pthread -MMD -MP
BASE_LDFLAGS = -pthread

ifeq ($(PROFILE),release)
  CFLAGS  := $(BASE_CFLAGS) -O2 -DNDEBUG
  LDFLAGS := $(BASE_LDFLAGS)
  BUILD_DIR := build/release
  BIN := flux-server
else ifeq ($(PROFILE),debug)
  CFLAGS  := $(BASE_CFLAGS) -O0 -g3 -DDEBUG
  LDFLAGS := $(BASE_LDFLAGS)
  BUILD_DIR := build/debug
  BIN := flux-server-debug
else ifeq ($(PROFILE),asan)
  CFLAGS  := $(BASE_CFLAGS) -O1 -g3 -fno-omit-frame-pointer \
             -fsanitize=address,undefined
  LDFLAGS := $(BASE_LDFLAGS) -fsanitize=address,undefined
  BUILD_DIR := build/asan
  BIN := flux-server-asan
else ifeq ($(PROFILE),tsan)
  CFLAGS  := $(BASE_CFLAGS) -O1 -g3 -fno-omit-frame-pointer -fsanitize=thread
  LDFLAGS := $(BASE_LDFLAGS) -fsanitize=thread
  BUILD_DIR := build/tsan
  BIN := flux-server-tsan
else ifeq ($(PROFILE),ubsan)
  CFLAGS  := $(BASE_CFLAGS) -O1 -g3 -fsanitize=undefined
  LDFLAGS := $(BASE_LDFLAGS) -fsanitize=undefined
  BUILD_DIR := build/ubsan
  BIN := flux-server-ubsan
else
  $(error Unknown PROFILE='$(PROFILE)'. Use: release|debug|asan|tsan|ubsan)
endif

# ----- sources / objects -----------------------------------------------------
SRCS := $(wildcard src/*.c)
OBJS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# ----- top-level targets -----------------------------------------------------
.PHONY: all clean test bench format tidy compile_commands help \
        debug asan tsan ubsan release run

all: $(BIN)

release:
	@$(MAKE) PROFILE=release --no-print-directory

debug:
	@$(MAKE) PROFILE=debug --no-print-directory

asan:
	@$(MAKE) PROFILE=asan --no-print-directory

tsan:
	@$(MAKE) PROFILE=tsan --no-print-directory

ubsan:
	@$(MAKE) PROFILE=ubsan --no-print-directory

$(BIN): $(OBJS)
	@echo "  LD    $@  [$(PROFILE)]"
	@$(CC) $(OBJS) $(LDFLAGS) -o $@

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# ----- run / clean -----------------------------------------------------------
run: all
	./$(BIN)

clean:
	rm -rf build flux-server flux-server-debug flux-server-asan \
	       flux-server-tsan flux-server-ubsan
	$(MAKE) -C tests clean 2>/dev/null || true

# ----- tests / benchmarks ----------------------------------------------------
test:
	@$(MAKE) -C tests

bench: all
	@command -v wrk >/dev/null || { echo "install wrk first (apt install wrk)"; exit 1; }
	bench/scripts/run_bench.sh

bench-scale: all
	@command -v wrk >/dev/null || { echo "install wrk first (apt install wrk)"; exit 1; }
	bench/scripts/scale_threads.sh

# ----- code quality ----------------------------------------------------------
format:
	@command -v clang-format >/dev/null || { echo "clang-format not found"; exit 1; }
	clang-format -i src/*.c include/*.h

tidy:
	@command -v clang-tidy >/dev/null || { echo "clang-tidy not found"; exit 1; }
	clang-tidy src/*.c -- $(CFLAGS)

# bear is the easiest way to get compile_commands.json for clangd / IDEs
compile_commands:
	@command -v bear >/dev/null || { echo "install 'bear' (apt install bear)"; exit 1; }
	$(MAKE) clean
	bear -- $(MAKE) all

help:
	@echo "FluxServer build system"
	@echo ""
	@echo "Profiles (set PROFILE=... or use target shortcut):"
	@echo "  release   -O2, no asserts (default)"
	@echo "  debug     -O0 -g3"
	@echo "  asan      AddressSanitizer + UBSan"
	@echo "  tsan      ThreadSanitizer"
	@echo "  ubsan     UndefinedBehaviorSanitizer"
	@echo ""
	@echo "Targets:"
	@echo "  all                 build server (current profile)"
	@echo "  run                 build and run"
	@echo "  test                run test suite"
	@echo "  bench               run benchmark suite"
	@echo "  format / tidy       clang-format / clang-tidy"
	@echo "  compile_commands    generate compile_commands.json via bear"
	@echo "  clean               remove build artifacts"

-include $(DEPS)
