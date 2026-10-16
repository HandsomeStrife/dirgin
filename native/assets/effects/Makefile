# FED/FEZ effect-definition parser — native port (purist, no emulation).
CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
LDLIBS  ?= -lz
BIN      = fed
OBJ      = fed.o fez.o fed_dump.o main.o

.PHONY: all clean check

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c fed.h fez.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(BIN) $(OBJ)

# Smoke test: parse a real sample if the extract tree is present.
KEL ?= /srv/games/dirge-port/extract/_KEL
check: $(BIN)
	@f="$(KEL)/data/effect/resident/r0000/r0001/fer/r0001.fed"; \
	if [ -f "$$f" ]; then ./$(BIN) "$$f" | head -20; \
	else echo "sample not found ($$f) — skipping check"; fi
