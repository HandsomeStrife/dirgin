# =====================================================================
# oracle/ -- reusable executable-oracle harness.
#
# Links the recompiled EE corpus (recomp/generated_all/*.c.o, prebuilt by
# `cd ../recomp && make build-all`) with the workstream runtime (hle/ sif/
# cop2/ gs/ vu/ input/ src/kel.c) and the boot/ seams (runtime_hooks.c,
# sif_glue.c, vu1_stub.c), plus this directory's oracle library + drivers.
#
#   make              -- build oracle-cli + oracle-bones
#   make test         -- build + run the self-test (pure recompiled funcs)
#   make bones        -- build + run the km01vnw bone-matrix dump attempt
#   make clean
#
# NOTHING outside this directory is modified; external sources are compiled
# into obj/ here and the prebuilt EE .o files are linked as-is.
# =====================================================================
CC      ?= gcc
OPT     ?= -O1
CFLAGS  ?= $(OPT) -g -std=c11 -Wall -Wextra -Wno-unused-parameter
ROOT    := ..
GEN     := $(ROOT)/recomp/generated_all
OBJ     := obj

INCS := -I$(ROOT)/recomp -I$(GEN) -I$(ROOT)/hle -I$(ROOT)/sif \
        -I$(ROOT)/gs -I$(ROOT)/vu -I$(ROOT)/input -I$(ROOT)/src -I$(ROOT)
LIBS := -lz -lm

# The stock recompiler (recomp/tools/recompile.py) only translated the primary
# .text [0x100000,0x460720); the .kel_* OVERLAY code sections above that (incl.
# .kel_draw_section @0x5a6d00 = the whole MOD character/skin/skeleton subsystem:
# FUN_005aecf8/005b0470/005b0d30/005b2550/005b1260 ...) were emitted as EMPTY
# STUBS.  tools/recompile_keldraw.py is a local, segment-aware copy that also
# translates those overlays (0 unimpl).  We relink TUs 0038 + 0039 (which hold
# the .kel_extended + .kel_draw_section funcs) from gen/ and take every OTHER TU
# + the function table from the stock corpus, unchanged.  No duplicate symbols:
# the two stock stub TUs are excluded.
STOCK_EE := $(filter-out $(GEN)/eetu_0038.c.o $(GEN)/eetu_0039.c.o,$(wildcard $(GEN)/eetu_*.c.o))
GEN_EE   := gen/eetu_0038.c.o gen/eetu_0039.c.o
EE_OBJS  := $(STOCK_EE) $(GEN_EE) $(GEN)/function_table.c.o

# External workstream + boot-seam sources (compiled into obj/ here).
EXT_SRCS := \
    $(ROOT)/hle/kernel.c $(ROOT)/hle/mmio.c \
    $(ROOT)/sif/sif.c \
    $(ROOT)/input/pad.c \
    $(ROOT)/src/kel.c \
    $(ROOT)/gs/gs_core.c $(ROOT)/gs/backend_log.c \
    $(ROOT)/cop2/vu0_macro.c \
    $(ROOT)/boot/runtime_hooks.c $(ROOT)/boot/sif_glue.c $(ROOT)/boot/vu1_stub.c
EXT_OBJS := $(addprefix $(OBJ)/,$(notdir $(EXT_SRCS:.c=.o)))

LIB_OBJS := $(OBJ)/oracle.o

all: oracle-cli oracle-bones

oracle-cli: $(LIB_OBJS) $(OBJ)/oracle_cli.o $(EXT_OBJS) $(EE_OBJS)
	$(CC) $(CFLAGS) -rdynamic -o $@ $^ $(LIBS)
	@echo "built $@"

oracle-bones: $(LIB_OBJS) $(OBJ)/oracle_bones.o $(EXT_OBJS) $(EE_OBJS)
	$(CC) $(CFLAGS) -rdynamic -o $@ $^ $(LIBS)
	@echo "built $@"

oracle-test: $(LIB_OBJS) $(OBJ)/oracle_test.o $(EXT_OBJS) $(EE_OBJS)
	$(CC) $(CFLAGS) -rdynamic -o $@ $^ $(LIBS)
	@echo "built $@"

# local sources
$(OBJ)/%.o: %.c | $(OBJ)
	$(CC) $(CFLAGS) $(INCS) -c $< -o $@

# external sources (pattern per directory)
$(OBJ)/%.o: $(ROOT)/hle/%.c   | $(OBJ) ; $(CC) $(CFLAGS) $(INCS) -c $< -o $@
$(OBJ)/%.o: $(ROOT)/sif/%.c   | $(OBJ) ; $(CC) $(CFLAGS) $(INCS) -c $< -o $@
$(OBJ)/%.o: $(ROOT)/input/%.c | $(OBJ) ; $(CC) $(CFLAGS) $(INCS) -c $< -o $@
$(OBJ)/%.o: $(ROOT)/src/%.c   | $(OBJ) ; $(CC) $(CFLAGS) $(INCS) -c $< -o $@
$(OBJ)/%.o: $(ROOT)/gs/%.c    | $(OBJ) ; $(CC) $(CFLAGS) $(INCS) -c $< -o $@
$(OBJ)/%.o: $(ROOT)/cop2/%.c  | $(OBJ) ; $(CC) $(CFLAGS) $(INCS) -c $< -o $@
$(OBJ)/%.o: $(ROOT)/boot/%.c  | $(OBJ) ; $(CC) $(CFLAGS) $(INCS) -c $< -o $@

$(OBJ):
	mkdir -p $(OBJ)

# Compile the two re-generated overlay TUs (real bodies for .kel_draw_section).
gen/%.c.o: gen/%.c
	$(CC) $(CFLAGS) $(INCS) -c $< -o $@

$(STOCK_EE):
	@echo "stock EE objects missing -- run: (cd $(ROOT)/recomp && make build-all)"; false

test: oracle-test
	./oracle-test

bones: oracle-bones
	./oracle-bones

clean:
	rm -rf $(OBJ) oracle-cli oracle-bones oracle-test

.PHONY: all test bones clean
