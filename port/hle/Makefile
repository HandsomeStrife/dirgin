# =====================================================================
# WS-K HLE kernel + SIF/IO + input -- standalone headless build & tests.
# Builds against the FROZEN recompiler contract (../recomp/ee_runtime.h) and
# the real GS backend (../gs) + native loader (../src/kel.c); no recompiled
# game and no GPU/window system required.
# =====================================================================
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
INC      = -I. -I../recomp -I../src -I../sif -I../input -I../gs
LDLIBS   = -lz

HLE_SRC  = kernel.c mmio.c
SIF_SRC  = ../sif/sif.c
PAD_SRC  = ../input/pad.c
GS_SRC   = ../gs/gs_core.c ../gs/backend_log.c
KEL_SRC  = ../src/kel.c
SRC      = $(HLE_SRC) $(SIF_SRC) $(PAD_SRC) $(GS_SRC) $(KEL_SRC)

all: test_hle

test_hle: test_hle.c $(SRC) hle.h hle_priv.h ee_mmio_regs.h ../sif/sif.h ../input/pad.h
	$(CC) $(CFLAGS) $(INC) -o $@ test_hle.c $(SRC) $(LDLIBS)

# prove the layer compiles library-style too (objects only)
objs:
	$(CC) $(CFLAGS) $(INC) -c kernel.c -o kernel.o
	$(CC) $(CFLAGS) $(INC) -c mmio.c   -o mmio.o
	$(CC) $(CFLAGS) $(INC) -c $(SIF_SRC) -o sif.o
	$(CC) $(CFLAGS) $(INC) -c $(PAD_SRC) -o pad.o

check: test_hle
	./test_hle

clean:
	rm -f test_hle *.o

.PHONY: all check clean objs
