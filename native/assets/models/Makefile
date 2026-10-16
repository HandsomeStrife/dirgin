# Dirge of Cerberus -- native model loader/exporter (Milestone P1 geometry)
CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter
OBJS     = doc_model.o export_obj.o export_gltf.o main.o
BIN      = doc_model

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c doc_model.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(BIN)

.PHONY: all clean
