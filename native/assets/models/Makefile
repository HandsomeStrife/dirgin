# Dirge of Cerberus -- native model loader/exporter (Milestone P1 geometry)
CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter
OBJS     = doc_model.o export_obj.o export_gltf.o main.o
BIN      = doc_model

all: $(BIN) env_decode

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c doc_model.h
	$(CC) $(CFLAGS) -c -o $@ $<

# native environment (room) geometry decoder + its parity CLI
env_decode: env_decode.c doc_env.c doc_env.h
	$(CC) $(CFLAGS) -o $@ env_decode.c doc_env.c

clean:
	rm -f $(OBJS) $(BIN) env_decode doc_env.o env_decode.o

.PHONY: all clean env-test

# Vertex-exact parity: native doc_env decoder vs the Python oracle. Zone-agnostic.
EXTD ?= /srv/games/dirge-port/extract/_KEL/noPath
env-test: env_decode
	@ok=1; for z in 068f0112 068f0212 06960212 06800212; do \
	  f=$(EXTD)/$$z.bin; [ -f $$f ] || continue; \
	  ./env_decode $$f 2>/dev/null > /tmp/envc.$$z; python3 -I env_fnv.py $$f 2>/dev/null > /tmp/envp.$$z; \
	  if cmp -s /tmp/envc.$$z /tmp/envp.$$z; \
	  then echo "  $$z: C == Python (vertex-exact)"; else echo "  $$z: MISMATCH"; ok=0; fi; \
	  rm -f /tmp/envc.$$z /tmp/envp.$$z; done; \
	  if [ $$ok = 1 ]; then echo "env-test PASS"; else echo "env-test FAIL"; exit 1; fi
