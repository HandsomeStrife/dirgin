# WS-R recompiler build.
#   make gen         -- regenerate the 2-func prototype generated/ee_funcs.c
#   make run         -- build + run the prototype correctness harness
#   make emit-all    -- recompile ALL 13,089 funcs -> generated_all/ (44 TUs)
#   make build-all   -- compile every generated TU + table (whole-program syntax)
#   make scale       -- build + run the scaled differential test harness
#   make clean / clean-all
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -I.
PY      ?= python3

ELF     := /srv/games/dirge-port/elf/SLES_541.85
CSV     := /srv/games/dirge-port/analysis/ee_boundaries.csv
TU_SIZE ?= 300
# Leaf functions recompiled for the prototype (space-separated guest VAs):
FUNCS   := 0x00417940 0x0036c338

GEN     := generated/ee_funcs.c
SRCS    := test_recomp.c ee_runtime.c $(GEN) generated/function_table.c

OUTALL  := generated_all
ALLSRCS := $(wildcard $(OUTALL)/eetu_*.c) $(OUTALL)/function_table.c

all: test_recomp

$(GEN): tools/recompile.py
	$(PY) tools/recompile.py $(ELF) $(FUNCS) > $@

gen: ; $(PY) tools/recompile.py $(ELF) $(FUNCS) > $(GEN)

test_recomp: $(SRCS) ee_runtime.h
	$(CC) $(CFLAGS) -o $@ test_recomp.c ee_runtime.c $(GEN) generated/function_table.c

run: test_recomp
	./test_recomp

# ---- whole-program ----
emit-all: tools/recompile.py
	$(PY) tools/recompile.py emit --elf $(ELF) --csv $(CSV) --outdir $(OUTALL) --tu-size $(TU_SIZE)

build-all: emit-all
	@set -e; for f in $(OUTALL)/eetu_*.c $(OUTALL)/function_table.c; do \
	  $(CC) $(CFLAGS) -I$(OUTALL) -c $$f -o $$f.o; done; \
	echo "build-all: compiled $$(ls $(OUTALL)/*.o | wc -l) objects (-Wall -Wextra clean)"

test_scale: emit-all test_scale.c ee_runtime.c ee_stubs.c ee_runtime.h
	$(CC) $(CFLAGS) -I$(OUTALL) -o $@ test_scale.c ee_runtime.c ee_stubs.c $(ALLSRCS) -lm

scale: test_scale
	./test_scale

clean:
	rm -f test_recomp $(GEN)

clean-all: clean
	rm -rf $(OUTALL) test_scale $(OUTALL)/*.o

.PHONY: all gen run emit-all build-all scale clean clean-all
