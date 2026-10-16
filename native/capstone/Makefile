# =====================================================================
# capstone/Makefile -- the native engine loop + native GPU renderer, one
# program. Built ONLY from the engine SNAPSHOT (engine_snapshot/, a one-
# time copy of the STABLE native/engine modules; actor.{h,c} excluded) so
# it is immune to the live edits happening in native/engine.
#
# native/gpu (render core + GL loader + EGL context) and native/assets
# (doc_model.h for the renderer_gl.h include, libdtex.a) are compiled /
# linked READ-ONLY from their own trees and never modified in place.
# =====================================================================
CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra
LDFLAGS ?= -lEGL -lGL -lm

# --- read-only sibling trees ---
GPU_DIR   := ../gpu
MODEL_DIR := ../assets/models
TEX_DIR   := ../assets/textures
LIBDTEX   := $(TEX_DIR)/libdtex.a

# --- engine snapshot (the ONLY engine source we build) ---
SNAP := engine_snapshot
ENGINE_SRC := \
  $(SNAP)/math/dmath.c \
  $(SNAP)/math/vu0.c \
  $(SNAP)/core/frame_loop.c \
  $(SNAP)/core/engine_state.c \
  $(SNAP)/core/input.c \
  $(SNAP)/scene/scene_graph.c \
  $(SNAP)/scene/object_list.c \
  $(SNAP)/scene/material.c \
  $(SNAP)/scene/camera.c \
  $(SNAP)/scene/light.c

# include paths: snapshot (for "math/..".-style and bare includes), gpu,
# the model dir (renderer_gl.h pulls in doc_model.h), the texture dir.
INCLUDES := -I$(SNAP) -I$(SNAP)/math -I$(SNAP)/core -I$(SNAP)/scene \
            -I$(GPU_DIR) -I$(MODEL_DIR) -I$(TEX_DIR)

BUILD := build
BIN   := capstone

ENGINE_OBJ := $(patsubst $(SNAP)/%.c,$(BUILD)/engine/%.o,$(ENGINE_SRC))
GPU_OBJ    := $(BUILD)/gpu/renderer_gl.o $(BUILD)/gpu/gl_api.o $(BUILD)/gpu/gfx_context_egl.o
MAIN_OBJ   := $(BUILD)/main.o
OBJS       := $(MAIN_OBJ) $(ENGINE_OBJ) $(GPU_OBJ)

all: $(BIN)

$(LIBDTEX):
	$(MAKE) -C $(TEX_DIR) libdtex.a

# main.c (our code: strict -Wall -Wextra)
$(BUILD)/main.o: main.c | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# engine snapshot objects (ported engine code; strict -Wall -Wextra)
$(BUILD)/engine/%.o: $(SNAP)/%.c | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# native/gpu objects (read-only; they intentionally ignore unused params)
$(BUILD)/gpu/renderer_gl.o: $(GPU_DIR)/renderer_gl.c | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-unused-parameter $(INCLUDES) -c -o $@ $<
$(BUILD)/gpu/gl_api.o: $(GPU_DIR)/gl_api.c | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-unused-parameter $(INCLUDES) -c -o $@ $<
$(BUILD)/gpu/gfx_context_egl.o: $(GPU_DIR)/gfx_context_egl.c | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-unused-parameter $(INCLUDES) -c -o $@ $<

$(BIN): $(OBJS) $(LIBDTEX)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBDTEX) $(LDFLAGS)

$(BUILD):
	@mkdir -p $(BUILD)/engine/math $(BUILD)/engine/core $(BUILD)/engine/scene $(BUILD)/gpu

run: $(BIN)
	./$(BIN)

clean:
	rm -rf $(BUILD) $(BIN)

.PHONY: all clean run
