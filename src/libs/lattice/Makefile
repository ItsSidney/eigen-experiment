# Lattice standalone build (host). The same sources build freestanding
# for kernels with -ffreestanding -fno-builtin.
CC      ?= gcc
MUSL_GCC ?= musl-gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2

CORE = core/arena.c l0/surface.c l2/reactive.c l1/compositor.c
CLI  = cli/main.c cli/term.c cli/dialogs.c
INC  = -I. -Icli

all: lattice

lattice: $(CORE) $(CLI)
	$(MUSL_GCC) -static -O2 -Wall $(INC) -o $@ $^

lattice-host: $(CORE) $(CLI)
	$(CC) $(CFLAGS) $(INC) -o /tmp/lattice-host $^

test: lattice-host
	$(CC) $(CFLAGS) $(INC) $(CORE) tests/test_main.c -o /tmp/lattice-test
	/tmp/lattice-test
	@echo "[cli checks]"
	@printf '\n' | /tmp/lattice-host --menu "Pick" 12 40 3 a A b B \
	    >/dev/null 2>/tmp/cli_res.txt; \
	    echo "  menu rc=$$? result=$$(cat /tmp/cli_res.txt)"

clean:
	rm -f lattice /tmp/lattice-host /tmp/lattice-test

.PHONY: all test clean lattice-host
