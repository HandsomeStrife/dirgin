# Dirge of Cerberus - CID map-collision loader
# Portable C99, dependency-free. Builds the `cidtool` CLI.

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wpedantic
BIN      = cidtool
OBJS     = main.o cid.o obj_export.o
WEB      = cidweb

# Real samples (adjust KEL if your extract lives elsewhere)
KEL     ?= /srv/games/dirge-port/extract/_KEL/noPath
OUT      = out

.PHONY: all clean check validate z128web

all: $(BIN) $(WEB)

$(WEB): web_export.o cid.o
	$(CC) $(CFLAGS) -o $@ web_export.o cid.o -lm

# z128 (FileCode 0680, sea of flames) -> browser collision
z128web: $(WEB)
	@mkdir -p $(OUT)/web
	./$(WEB) $(OUT)/web/z128_collision.bin $(KEL)/0680?[0-9a-c]11.bin

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) -lm

%.o: %.c cid.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Quick build sanity + run on the two shipped version-2 samples and a v19 map.
check: all
	@mkdir -p $(OUT)
	./$(BIN) $(KEL)/06010211.bin $(OUT)/06010211.obj
	@echo

# Full validation: parse several real CID files, export OBJ (with tag-2 boxes
# for the v19 map), and report counts/bbox for each.
validate: all
	@mkdir -p $(OUT)
	@echo "=== v2 inline quad-soup (tutorial collision) ==="
	./$(BIN) $(KEL)/06010211.bin $(OUT)/06010211.obj
	@echo
	./$(BIN) $(KEL)/06010111.bin $(OUT)/06010111.obj
	@echo
	@echo "=== v19 grid/indexed tree (real map collision) ==="
	./$(BIN) $(KEL)/06cc0111.bin $(OUT)/06cc0111.obj --boxes
	@echo
	./$(BIN) $(KEL)/068a0111.bin $(OUT)/068a0111.obj --boxes
	@echo
	./$(BIN) $(KEL)/06080111.bin $(OUT)/06080111.obj --boxes

clean:
	rm -f $(BIN) $(WEB) $(OBJS) web_export.o
	rm -rf $(OUT)
