# Dirge of Cerberus script VM - native build
# Targets:
#   make            -> dcc (disassembler) + jvmrun (interpreter) + jvmtest
#   make test       -> build + run the deterministic self-test
#   make run        -> run KerberosIntermissionCalc.class on the interpreter
#   make clean
#
# Shared parser: classfile.c (decrypt + container parse + flat resolve), factored
# from dcc.c's reverse-engineered loader and consumed by the interpreter.

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra
LDLIBS  ?= -lm

SCRIPT_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
CALC := $(SCRIPT_DIR)../../extract/_KEL/data/etc/KerberosIntermissionCalc.class
INSTALL := $(SCRIPT_DIR)../../extract/_KEL/install/install.class

# ---- native engine (read-only dependency) --------------------------------
# The bridge's engine-backed handlers call the stable native/engine subsystems.
# We compile against its public headers and link its prebuilt static library
# (built by its own CMake: libdoc_engine.a). We never modify native/engine.
ENGINE_DIR := $(SCRIPT_DIR)../engine
ENGINE_INC := -I$(ENGINE_DIR)/scene -I$(ENGINE_DIR)/core -I$(ENGINE_DIR)/math
ENGINE_LIB := $(ENGINE_DIR)/build/libdoc_engine.a
ENGINE_HDRS := $(ENGINE_DIR)/scene/actor.h $(ENGINE_DIR)/scene/camera.h \
               $(ENGINE_DIR)/scene/light.h $(ENGINE_DIR)/core/engine_state.h \
               $(ENGINE_DIR)/math/dmath.h

CFLAGS += $(ENGINE_INC)

# Core VM objects that do NOT need the engine (the JVM self-test links only these).
CORE_OBJS := classfile.o jvm.o
# Full bridge objects (pull in the native engine).
VM_OBJS   := $(CORE_OBJS) bridge.o bridge_engine.o

all: dcc jvmrun jvmtest jvmbatch bridge_test

dcc: dcc.c
	$(CC) $(CFLAGS) -o $@ $<

classfile.o: classfile.c classfile.h
jvm.o:       jvm.c jvm.h classfile.h
bridge.o:    bridge.c bridge.h bridge_engine.h jvm.h classfile.h bridge_api_gen.h $(ENGINE_HDRS)
bridge_engine.o: bridge_engine.c bridge_engine.h bridge.h jvm.h classfile.h $(ENGINE_HDRS)

# jvmtest links only the core VM (no bridge, no engine) - proves the hardened
# JVM self-test is independent of the bridge/engine wiring.
jvmtest: jvm_test.c $(CORE_OBJS)
	$(CC) $(CFLAGS) -o $@ jvm_test.c $(CORE_OBJS) $(LDLIBS)

jvmrun: jvmrun.c $(VM_OBJS) $(ENGINE_LIB)
	$(CC) $(CFLAGS) -o $@ jvmrun.c $(VM_OBJS) $(ENGINE_LIB) $(LDLIBS)

jvmbatch: jvmbatch.c $(VM_OBJS) $(ENGINE_LIB)
	$(CC) $(CFLAGS) -o $@ jvmbatch.c $(VM_OBJS) $(ENGINE_LIB) $(LDLIBS)

# bridge_test: a script (hand-built bytecode) driving the REAL native engine
# through the bridge, asserting the engine state actually changed.
bridge_test: bridge_test.c $(VM_OBJS) $(ENGINE_LIB)
	$(CC) $(CFLAGS) -o $@ bridge_test.c $(VM_OBJS) $(ENGINE_LIB) $(LDLIBS)

$(ENGINE_LIB):
	@echo "ERROR: $(ENGINE_LIB) not found. Build the native engine first:" >&2
	@echo "  cmake -S $(ENGINE_DIR) -B $(ENGINE_DIR)/build && cmake --build $(ENGINE_DIR)/build" >&2
	@exit 1

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# regenerate the 281-method bridge table from the API list
bridge_api_gen.h: engine_bridge_api.txt
	@{ echo "/* bridge_api_gen.h - GENERATED from engine_bridge_api.txt (281 native methods)."; \
	   echo " * Do not edit by hand; regenerate with: make bridge_api_gen.h */"; \
	   echo "#ifndef BRIDGE_API_GEN_H"; echo "#define BRIDGE_API_GEN_H"; \
	   echo "static const char *const BRIDGE_API[] = {"; \
	   sed -E 's/\\/\\\\/g; s/"/\\"/g; s/^(.*)$$/    "\1",/' $<; \
	   echo "};"; \
	   echo "#define BRIDGE_API_COUNT ((int)(sizeof(BRIDGE_API)/sizeof(BRIDGE_API[0])))"; \
	   echo "#endif"; } > $@

test: jvmtest
	./jvmtest

# bridge<->engine integration demonstration
btest: bridge_test
	./bridge_test

run: jvmrun
	./jvmrun --trace=60 --seed "$(CALC)"

clean:
	rm -f dcc jvmrun jvmtest jvmbatch bridge_test $(VM_OBJS)

.PHONY: all test btest run clean
