# =============================================================================
# FluxServer unit-test driver
# Profiles: debug (default) | asan | release
# Default target builds and runs every test_* binary in unit/.
# =============================================================================

PROFILE ?= debug

CC      ?= gcc
CSTD     = -std=c11
INCLUDES = -I../include

WARN = -Wall -Wextra -Werror -Wshadow -Wpointer-arith -Wcast-align \
       -Wwrite-strings -Wmissing-prototypes -Wstrict-prototypes \
       -Wno-unused-parameter -Wno-unused-result -Wformat=2

BASE_CFLAGS  = $(CSTD) $(WARN) $(INCLUDES) -D_GNU_SOURCE -pthread
BASE_LDFLAGS = -pthread

ifeq ($(PROFILE),debug)
  CFLAGS  := $(BASE_CFLAGS) -O0 -g3
  LDFLAGS := $(BASE_LDFLAGS)
else ifeq ($(PROFILE),asan)
  CFLAGS  := $(BASE_CFLAGS) -O1 -g3 -fno-omit-frame-pointer \
             -fsanitize=address,undefined
  LDFLAGS := $(BASE_LDFLAGS) -fsanitize=address,undefined
else ifeq ($(PROFILE),release)
  CFLAGS  := $(BASE_CFLAGS) -O2 -DNDEBUG
  LDFLAGS := $(BASE_LDFLAGS)
else
  $(error Unknown PROFILE='$(PROFILE)'. Use: debug|asan|release)
endif

SRC = ../src

TEST_BUF         = unit/test_buf
TEST_HTTP_PARSER = unit/test_http_parser
TEST_TIMER_HEAP  = unit/test_timer_heap

TESTS = $(TEST_BUF) $(TEST_HTTP_PARSER) $(TEST_TIMER_HEAP)

.PHONY: all build run clean

all: run

build: $(TESTS)

$(TEST_BUF): unit/test_buf.c $(SRC)/buf.c
	@echo "  LD    $@  [$(PROFILE)]"
	@$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

$(TEST_HTTP_PARSER): unit/test_http_parser.c $(SRC)/http_parser.c
	@echo "  LD    $@  [$(PROFILE)]"
	@$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

$(TEST_TIMER_HEAP): unit/test_timer_heap.c \
                    $(SRC)/timer_heap.c $(SRC)/conn.c \
                    $(SRC)/buf.c $(SRC)/http_parser.c \
                    $(SRC)/metrics.c $(SRC)/logger.c
	@echo "  LD    $@  [$(PROFILE)]"
	@$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

run: build
	@fails=0; \
	for t in $(TESTS); do \
	    ./$$t || fails=$$((fails + 1)); \
	done; \
	if [ $$fails -ne 0 ]; then \
	    echo "FAILED: $$fails suite(s)"; exit 1; \
	fi

clean:
	@rm -f $(TESTS)
