# Basis standalone build. Needs musl-gcc in PATH (or MUSL_GCC set).
# The ui/ console additionally needs Lattice sources: clone the lattice
# repo next to this one, or point LATTICE_DIR at it.
MUSL_GCC ?= musl-gcc
INC       = -Iinclude
LATTICE_DIR ?= ../lattice

CORE   = core/main.c
SVCMGR = svcmgr/main.c svcmgr/load.c svcmgr/dag.c svcmgr/run.c \
         svcmgr/state.c lib/toml.c
CTL    = ctl/main.c ctl/common.c ctl/status.c ctl/mutate.c ctl/validate.c \
         lib/toml.c lib/state.c

all: basis-core basis-svcmgr basisctl basis-log basis-ntpsync

basis-core: $(CORE)
	@mkdir -p out
	$(MUSL_GCC) -static -O2 -Wall $(INC) -o out/$@ $<

basis-svcmgr: $(SVCMGR)
	@mkdir -p out
	$(MUSL_GCC) -static -O2 -Wall $(INC) -o out/$@ $^

basisctl: $(CTL)
	@mkdir -p out
	$(MUSL_GCC) -static -O2 -Wall $(INC) -o out/$@ $^

basis-log: log/main.c
	@mkdir -p out
	$(MUSL_GCC) -static -O2 -Wall $(INC) -o out/$@ $<

basis-ntpsync: ntp/main.c
	@mkdir -p out
	$(MUSL_GCC) -static -O2 -Wall $(INC) -o out/$@ $<

UI_SRCS = ui/main.c ui/data.c ui/ui.c
ui: all
	@mkdir -p out
	$(MUSL_GCC) -static -O2 -Wall -I$(LATTICE_DIR) \
	    -I$(LATTICE_DIR)/cli -Iui \
	    $(UI_SRCS) \
	    $(LATTICE_DIR)/core/arena.c $(LATTICE_DIR)/l0/surface.c \
	    $(LATTICE_DIR)/l2/reactive.c $(LATTICE_DIR)/l1/compositor.c \
	    $(LATTICE_DIR)/cli/term.c \
	    -o out/basisui

clean:
	rm -rf out

.PHONY: all ui clean
