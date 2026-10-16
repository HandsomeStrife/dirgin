# Makefile -- Dirge of Cerberus native software renderer (Milestone P3).
#
# Links the sibling native loaders:
#   ../assets/models/doc_model.c   (BMD/MOD geometry)
#   ../assets/textures/libdtex.a   (TXD/TIM2/RST -> RGBA + PNG writer)
#
# Dependency-free, portable C99. This is the headless software reference
# rasterizer; the GPU (Vulkan/D3D) backend is a later Windows milestone.

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?= -lm

MODEL_DIR := ../assets/models
TEX_DIR   := ../assets/textures

INCLUDES  := -I$(MODEL_DIR) -I$(TEX_DIR)
LIBDTEX   := $(TEX_DIR)/libdtex.a

BIN := renderer
OBJS := renderer.o doc_model.o

all: $(BIN)

# the texture library is built by its own Makefile
$(LIBDTEX):
	$(MAKE) -C $(TEX_DIR) libdtex.a

renderer.o: renderer.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# compile the model loader source here (do not modify it in place)
doc_model.o: $(MODEL_DIR)/doc_model.c $(MODEL_DIR)/doc_model.h
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(BIN): $(OBJS) $(LIBDTEX)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBDTEX) $(LDFLAGS)

clean:
	rm -f $(OBJS) $(BIN)

.PHONY: all clean
