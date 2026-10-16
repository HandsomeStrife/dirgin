# Makefile - Dirge of Cerberus native texture decoder (dtex)
#
# Portable, dependency-free C11. Builds a static library (libdtex.a) feeding the
# future renderer, and the `dtex` CLI that exports textures to PNG.

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter
AR      ?= ar

LIB_SRC := swizzle.c decode.c txd.c tim2.c rst.c png.c
LIB_OBJ := $(LIB_SRC:.c=.o)

all: libdtex.a dtex

libdtex.a: $(LIB_OBJ)
	$(AR) rcs $@ $(LIB_OBJ)

dtex: main.c libdtex.a
	$(CC) $(CFLAGS) -o $@ main.c libdtex.a

%.o: %.c dtex.h dtex_internal.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(LIB_OBJ) libdtex.a dtex

.PHONY: all clean
