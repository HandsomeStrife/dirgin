CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDLIBS  ?= -lz

all: kel_tool

kel_tool: src/kel_tool.c src/kel.c src/kel.h
	$(CC) $(CFLAGS) -o $@ src/kel_tool.c src/kel.c $(LDLIBS)

clean:
	rm -f kel_tool
