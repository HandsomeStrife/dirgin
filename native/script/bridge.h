/*
 * bridge.h - the script<->engine seam for the Dirge JVM.
 *
 * Registers all 281 Kerberos.* native methods (engine_bridge_api.txt) as the
 * VM's native-handler table. Most are logging STUBS returning typed defaults;
 * the pure state getters/setters + math the engine implements trivially are
 * backed by real in-memory engine state so a compute script runs end to end.
 */
#ifndef BRIDGE_H
#define BRIDGE_H

#include "jvm.h"

/* Real native-engine subsystems (native/engine, linked as libdoc_engine).
 * The engine-backed bridge classes route to these instead of logging. */
#include "actor.h"          /* DocActor / DocActorManager  */
#include "camera.h"         /* DocCamera                   */
#include "light.h"          /* DocLightSet                 */
#include "engine_state.h"   /* EngineState (mode/scene)    */

/* Size of the actor pool DocActorManager carves entities from. Generous for
 * any single event/map script; doc_actor_load returns NULL (handled) if full. */
#define DIRGE_ACTOR_POOL 64

/* Engine state the bridge drives. The int arrays are the KerberosZone/Event
 * flag STORE and the Intermission stat tables (pure in-memory engine state,
 * as before); the embedded subsystems are the REAL native engine the
 * KerberosActor/Camera/Light/Event-state bridges now call into. */
typedef struct {
    /* ---- flag store + intermission stats (pure engine state) ---- */
    int  event_flags[256];   /* KerberosEvent.get/setflag                     */
    int  zone_flags[256];    /* KerberosZone.get/setflag                      */
    int  section_stat[256];
    int  total_stat[256];
    int  difficulty;         /* getGameDifficultyLevel()                      */
    int  is_tutorial;        /* isTutorial()                                  */

    /* ---- real native engine subsystems ---- */
    DocActorManager actor_mgr;                 /* KerberosActor.*             */
    DocActor        actor_pool[DIRGE_ACTOR_POOL];
    DocCamera       camera;                    /* KerberosCamera.*            */
    DocLightSet     lights;                    /* KerberosLight.*             */
    EngineState     estate;                    /* KerberosEvent game-state    */
} DirgeEngine;

void dirge_engine_init(DirgeEngine *e);

/* Register the full bridge (stubs + real pure methods) on the VM.
 * vm->user is set to `eng`. */
void bridge_install(jvm *vm, DirgeEngine *eng);

#endif
