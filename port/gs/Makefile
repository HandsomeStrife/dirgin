# GS backend (WS-G) -- standalone headless build.
# The interpreter + log backend need no GPU and no window system.
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
CORE     = gs_core.c backend_log.c

all: test_triangle

# pure-logic interpreter test (no GPU)
test_triangle: test_triangle.c $(CORE) gs.h gs_regs.h
	$(CC) $(CFLAGS) -o $@ test_triangle.c $(CORE)

# compile the core into an object to prove it builds library-style too
gs_core.o: gs_core.c gs.h gs_regs.h
	$(CC) $(CFLAGS) -c -o $@ gs_core.c

check: test_triangle
	./test_triangle

clean:
	rm -f test_triangle gs_core.o

.PHONY: all check clean
