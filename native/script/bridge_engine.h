/*
 * bridge_engine.h - the ENGINE-BACKED half of the Dirge native bridge.
 *
 * bridge.c registers all 281 Kerberos.* methods as logging stubs and backs the
 * pure flag/stat/math ones in memory. This module overrides the subset whose
 * native engine subsystem is ported (native/engine, linked as libdoc_engine)
 * so a decrypted .class script DRIVES the real engine:
 *
 *   KerberosActor.load/getpos/setpos/getdir/setdir/setrot/setVisible
 *                                        -> DocActorManager / DocActor
 *   KerberosCamera.move/set/setfov/getCurrentCameraVectors
 *                                        -> DocCamera
 *   KerberosLight.set_ambientcolor/set_directioncolor/set_directionvector/
 *                set_shadowalpha/set_shadowvector/reset
 *                                        -> DocLightSet
 *   KerberosEvent.setMainMode            -> EngineState mode-flag (real
 *                                           engine_set_mode_flag primitive)
 *   fake/base/KerberosVector.<init>(FFF) -> store x,y,z on the vector object so
 *                                           it can carry a position across the
 *                                           bridge (setpos / set_directionvector)
 *
 * Receiver resolution: KerberosActor is a per-instance object, so each actor
 * object caches its DocActor* in a hidden "$actor" field (vm->bridge_this gives
 * the receiver). Camera/Light/EngineState are engine singletons (the guest
 * DAT_ globals), so those handlers ignore the receiver and use the one
 * DirgeEngine instance.
 *
 * Still logging stubs (subsystem not ported this pass): KerberosWindow,
 * KerberosSound, KerberosEffect, KerberosGlobalMap, and the actor
 * animation/morph/optical-camouflage methods below the engine seam.
 */
#ifndef BRIDGE_ENGINE_H
#define BRIDGE_ENGINE_H

#include "jvm.h"
#include "bridge.h"

/* Register the engine-backed handlers on `vm`, overriding the stub/pure
 * registrations. Call AFTER bridge.c's stub table + pure handlers are in
 * place. vm->user must already point at `eng`. */
void bridge_engine_install(jvm *vm, DirgeEngine *eng);

#endif
