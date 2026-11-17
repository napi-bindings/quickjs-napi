/*
 * Child processes for the quickjs-napi executable (child_process module of
 * host/bootstrap.js): natives.spawnSync and natives.spawn, on libuv.
 */
#ifndef QJSNAPI_HOST_SPAWN_H
#define QJSNAPI_HOST_SPAWN_H

#include <uv.h>

#include "quickjs.h"

/* Calls `fn` from a libuv callback: the executable reports an exception as
 * uncaught and runs its checkpoint afterwards. */
typedef void (*spawn_dispatch_fn)(JSContext *ctx, JSValueConst fn, int argc,
                                  JSValueConst *argv);

/* Adds spawnSync and spawn to `natives`. Asynchronous children run on
 * `loop`; their events go through `dispatch`. Returns -1 with an exception
 * pending on failure. */
int spawn_add_natives(JSContext *ctx, JSValueConst natives, uv_loop_t *loop,
                      spawn_dispatch_fn dispatch);

/* Closes the handles of asynchronous children still running, without
 * waiting for them or calling back into JavaScript (process teardown). The
 * caller runs the loop afterwards so that the close callbacks free them. */
void spawn_close_all(void);

#endif /* QJSNAPI_HOST_SPAWN_H */
