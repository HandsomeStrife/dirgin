# =====================================================================
# M1 headless-boot integration build.  Links every workstream into one
# program:  generated EE TUs + function table + hle + sif + input + kel +
# gs(log) + vu1(stub) + cop2 + boot harness.
#
#   make            -- build ./dirge-m1
#   make run        -- build + run (300 frames, paced)
#   make fast       -- build + run unpaced with tracing
#   make regen-ee   -- recompile the generated EE TUs (slow; usually reuse .o)
#   make clean
#
# NOTE: recomp/ee_runtime.c and recomp/ee_stubs.c are the STANDALONE prototype
# runtime + logging stubs; they are intentionally NOT linked here (their
# bodies are superseded by hle/ + cop2/ + boot/runtime_hooks.c).  See
# boot/runtime_hooks.c for the seam resolution.
# =====================================================================
CC      ?= gcc
OPT     ?= -O1
CFLAGS  ?= $(OPT) -g -std=c11 -Wall -Wextra -Wno-unused-parameter
ROOT    := ..
GEN     := $(ROOT)/recomp/generated_all

INCS    := -I$(ROOT)/recomp -I$(GEN) -I$(ROOT)/hle -I$(ROOT)/sif \
           -I$(ROOT)/gs -I$(ROOT)/vu -I$(ROOT)/input -I$(ROOT)/src -I$(ROOT)
LIBS    := -lz -lm

# Prebuilt recompiled EE objects (made by `cd ../recomp && make build-all`).
EE_OBJS := $(wildcard $(GEN)/eetu_*.c.o) $(GEN)/function_table.c.o

# Hand-written workstream + integration sources.
SRCS    := \
    $(ROOT)/hle/kernel.c $(ROOT)/hle/mmio.c \
    $(ROOT)/sif/sif.c \
    $(ROOT)/input/pad.c \
    $(ROOT)/src/kel.c \
    $(ROOT)/gs/gs_core.c $(ROOT)/gs/backend_log.c \
    $(ROOT)/cop2/vu0_macro.c \
    runtime_hooks.c vu1_stub.c sif_glue.c main.c

OBJS    := $(SRCS:.c=.o)

dirge-m1: $(OBJS) $(EE_OBJS)
	$(CC) $(CFLAGS) -rdynamic -o $@ $(OBJS) $(EE_OBJS) $(LIBS)
	@echo "built $@"

%.o: %.c
	$(CC) $(CFLAGS) $(INCS) -c $< -o $@

$(EE_OBJS):
	@echo "generated EE objects missing -- run: (cd $(ROOT)/recomp && make build-all)"; false

regen-ee:
	$(MAKE) -C $(ROOT)/recomp build-all

run: dirge-m1
	./dirge-m1

fast: dirge-m1
	DIRGE_FAST=1 DIRGE_TRACE=1 ./dirge-m1

clean:
	rm -f $(OBJS) dirge-m1

.PHONY: run fast regen-ee clean
