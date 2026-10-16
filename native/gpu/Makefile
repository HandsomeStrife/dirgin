# Makefile -- Dirge of Cerberus native GPU renderer (Milestone P3, GPU backend).
#
# A real OpenGL 3.3-core renderer. On this headless Linux build server the GL
# context comes from EGL (surfaceless/pbuffer) -> an FBO; the same render core
# (renderer_gl.c + the shaders + gl_api loader) targets Windows unchanged behind
# a WGL/swapchain backend (see README.md "Windows portability").
#
# Links the sibling native loaders READ-ONLY (never modified in place):
#   ../assets/models/doc_model.c   (BMD/MOD geometry)
#   ../assets/textures/libdtex.a   (TXD/TIM2/RST -> RGBA + PNG writer)

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?= -lEGL -lGL -lm

MODEL_DIR := ../assets/models
TEX_DIR   := ../assets/textures

INCLUDES  := -I. -I$(MODEL_DIR) -I$(TEX_DIR)
LIBDTEX   := $(TEX_DIR)/libdtex.a

BIN  := gpu_renderer
OBJS := main.o renderer_gl.o gl_api.o gfx_context_egl.o doc_model.o

# Synthetic-mesh validation of the purist RENDER-MATH shading. Deliberately
# does NOT link ../assets/models (doc_model) -- that dir is busy -- so it builds
# from the render core + gl loader + EGL context + libdtex only.
VBIN  := validate_shading
VOBJS := validate_shading.o renderer_gl.o gl_api.o gfx_context_egl.o

# Real-character render: loads the posed+textured mesh exported by
# ../assets/models/render_skinned.py and renders it through rgl_render_mesh.
# Uses the render core + gl loader + EGL context + libdtex (no doc_model).
RCBIN  := render_char
RCOBJS := render_char.o renderer_gl.o gl_api.o gfx_context_egl.o

# Composed-scene render: loads a BMD environment mesh (PRELIT) AND the character
# mesh (LIT+textured) into one GL world and frames the character in the room.
# Render core + gl loader + EGL context + libdtex (no doc_model).
RSBIN  := render_scene
RSOBJS := render_scene.o renderer_gl.o gl_api.o gfx_context_egl.o

all: $(BIN)

$(LIBDTEX):
	$(MAKE) -C $(TEX_DIR) libdtex.a

main.o: main.c renderer_gl.h gfx_context.h
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<
renderer_gl.o: renderer_gl.c renderer_gl.h gl_api.h
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<
gl_api.o: gl_api.c gl_api.h
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<
gfx_context_egl.o: gfx_context_egl.c gfx_context.h gl_api.h
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

# compile the model loader source here (do not modify it in place)
doc_model.o: $(MODEL_DIR)/doc_model.c $(MODEL_DIR)/doc_model.h
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(BIN): $(OBJS) $(LIBDTEX)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBDTEX) $(LDFLAGS)

validate_shading.o: validate_shading.c renderer_gl.h gfx_context.h
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(VBIN): $(VOBJS) $(LIBDTEX)
	$(CC) $(CFLAGS) -o $@ $(VOBJS) $(LIBDTEX) $(LDFLAGS)

validate: $(VBIN)

render_char.o: render_char.c renderer_gl.h gfx_context.h
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(RCBIN): $(RCOBJS) $(LIBDTEX)
	$(CC) $(CFLAGS) -o $@ $(RCOBJS) $(LIBDTEX) $(LDFLAGS)

char: $(RCBIN)

render_scene.o: render_scene.c renderer_gl.h gfx_context.h
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(RSBIN): $(RSOBJS) $(LIBDTEX)
	$(CC) $(CFLAGS) -o $@ $(RSOBJS) $(LIBDTEX) $(LDFLAGS)

scene: $(RSBIN)

# Textured BMD environment render: loads the per-material-group DMSH meshes +
# TXD textures emitted by ../assets/models/export_bmd_textured.py (via its .list
# sidecar) and draws every group PRELIT + MODULATE + its texture in one GL world.
# Render core + gl loader + EGL context + libdtex (no doc_model).
REBIN  := render_env_textured
REOBJS := render_env_textured.o renderer_gl.o gl_api.o gfx_context_egl.o

render_env_textured.o: render_env_textured.c renderer_gl.h gfx_context.h
	$(CC) $(CFLAGS) $(INCLUDES) -Wno-format-truncation -c -o $@ $<

$(REBIN): $(REOBJS) $(LIBDTEX)
	$(CC) $(CFLAGS) -o $@ $(REOBJS) $(LIBDTEX) $(LDFLAGS)

env: $(REBIN)

# Composed TEXTURED scene: the textured character (LIT + body atlas) standing in
# the TEXTURED BMD room (every material group PRELIT + its TXD texture), one GL
# world / one depth buffer. Render core + gl loader + EGL context + libdtex.
RSTBIN  := render_scene_textured
RSTOBJS := render_scene_textured.o renderer_gl.o gl_api.o gfx_context_egl.o

render_scene_textured.o: render_scene_textured.c renderer_gl.h gfx_context.h
	$(CC) $(CFLAGS) $(INCLUDES) -Wno-format-truncation -c -o $@ $<

$(RSTBIN): $(RSTOBJS) $(LIBDTEX)
	$(CC) $(CFLAGS) -o $@ $(RSTOBJS) $(LIBDTEX) $(LDFLAGS)

scene_textured: $(RSTBIN)

# Engine-native whole-room render: geometry via ../assets/models/doc_env, binding
# via ../engine/scene/material_loader, textures via libdtex, drawn through the GL
# render core. The native equivalent of ../assets/models/export_room_bound.py.
ENGINE_DIR := ../engine/scene
RRNBIN  := render_room_native
RRNOBJS := render_room_native.o renderer_gl.o gl_api.o gfx_context_egl.o doc_env_gpu.o material_loader_gpu.o

render_room_native.o: render_room_native.c renderer_gl.h gfx_context.h
	$(CC) $(CFLAGS) $(INCLUDES) -I$(ENGINE_DIR) -Wno-format-truncation -c -o $@ $<
doc_env_gpu.o: $(MODEL_DIR)/doc_env.c $(MODEL_DIR)/doc_env.h
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<
material_loader_gpu.o: $(ENGINE_DIR)/material_loader.c $(ENGINE_DIR)/material_loader.h
	$(CC) $(CFLAGS) $(INCLUDES) -I$(ENGINE_DIR) -c -o $@ $<

$(RRNBIN): $(RRNOBJS) $(LIBDTEX)
	$(CC) $(CFLAGS) -o $@ $(RRNOBJS) $(LIBDTEX) $(LDFLAGS)

room: $(RRNBIN)

# Live-engine room render: drives the recovered room through the LIVE doc_engine
# frame loop (libdoc_engine.a) -- the real frame loop, not the frozen snapshot.
ENGINE_INC := -I../engine/scene -I../engine/core -I../engine/math
LIBDOCENG  := ../engine/build/libdoc_engine.a
RLBIN  := room_live
RLOBJS := room_live.o renderer_gl.o gl_api.o gfx_context_egl.o doc_env_gpu.o

room_live.o: room_live.c renderer_gl.h gfx_context.h
	$(CC) $(CFLAGS) $(INCLUDES) $(ENGINE_INC) -Wno-format-truncation -c -o $@ $<

$(RLBIN): $(RLOBJS) $(LIBDOCENG) $(LIBDTEX)
	$(CC) $(CFLAGS) -o $@ $(RLOBJS) $(LIBDOCENG) $(LIBDTEX) $(LDFLAGS)

live: $(RLBIN)

clean:
	rm -f $(OBJS) $(VOBJS) $(RCOBJS) $(RSOBJS) $(REOBJS) $(RSTOBJS) $(RRNOBJS) $(RLOBJS) $(BIN) $(VBIN) $(RCBIN) $(RSBIN) $(REBIN) $(RSTBIN) $(RRNBIN) $(RLBIN)

.PHONY: all clean validate char scene env scene_textured room live
