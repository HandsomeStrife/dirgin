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

.PHONY: all clean
