/*
 * The host's libuv loop: ownership, the finalizer queue run from the loop,
 * and the teardown phases that need the loop.
 *
 * Handles of the library on the loop (per host):
 *   finalizer_check  uv_check_t, started at creation and unreferenced: runs
 *                    the queued finalizers at the end of every iteration,
 *                    then a microtask checkpoint.
 *   finalizer_idle   uv_idle_t, active only while finalizers are queued:
 *                    keeps the loop alive and makes the poll phase return at
 *                    once, so the check phase comes next (Node.js runs the
 *                    queue from a referenced SetImmediate).
 * Everything else on the loop belongs to async work (src/async.c), to
 * thread-safe functions (src/tsfn.c), to addons or to the embedder.
 *
 * Teardown (qjsnapi_host_destroy):
 *   qn_loop_drain  after the cleanup hooks: uv_run(UV_RUN_ONCE) while async
 *                  work is in flight, thread-safe functions are not
 *                  finalized or async cleanup hooks are not removed, as
 *                  Node's Environment::RunCleanup does.
 *   qn_loop_close  last: closes the library's handles and, when the host
 *                  owns the loop, the loop itself (handles an addon left
 *                  open are closed first).
 */
#include <stdio.h>

#include "qjsnapi_internal.h"

static void on_finalizer_idle(uv_idle_t *handle) { (void)handle; }

static void on_finalizer_check(uv_check_t *handle) {
  qjsnapi_host *host = (qjsnapi_host *)handle->data;
  if (host->destroying) return;
  if (qjsnapi_host_run_finalizers(host) > 0) qn_microtask_checkpoint(host);
  if (!qjsnapi_host_has_pending_finalizers(host))
    uv_idle_stop(&host->finalizer_idle);
}

int qn_loop_init(qjsnapi_host *host, uv_loop_t *loop) {
  if (loop) {
    host->loop = loop;
    host->owns_loop = false;
  } else {
    if (uv_loop_init(&host->own_loop) != 0) return -1;
    host->loop = &host->own_loop;
    host->owns_loop = true;
  }
  uv_check_init(host->loop, &host->finalizer_check);
  uv_idle_init(host->loop, &host->finalizer_idle);
  host->finalizer_check.data = host;
  host->finalizer_idle.data = host;
  uv_check_start(&host->finalizer_check, on_finalizer_check);
  uv_unref((uv_handle_t *)&host->finalizer_check);
  host->finalizer_handles = true;
  return 0;
}

void qn_loop_schedule_finalizers(qjsnapi_host *host) {
  /* Idempotent; called when a finalizer is queued (possibly from inside
   * JS_RunGC, which is fine: this only links the handle into the loop). */
  if (host->finalizer_handles && !host->destroying)
    uv_idle_start(&host->finalizer_idle, on_finalizer_idle);
}

static bool waiting(const qjsnapi_host *host) {
  return host->active_works > 0 || host->live_tsfns > 0 ||
         host->running_async_cleanup > 0;
}

void qn_loop_drain(qjsnapi_host *host) {
  while (waiting(host)) {
    uv_run(host->loop, UV_RUN_ONCE);
    if (waiting(host) && !uv_loop_alive(host->loop)) {
      /* Nothing on the loop can finish them any more: typically an async
       * cleanup hook that never calls napi_remove_async_cleanup_hook
       * (Node.js would wait forever). */
      fprintf(stderr,
              "quickjs-napi: teardown gave up waiting for %u async cleanup "
              "hook(s), %u async work item(s), %u thread-safe function(s)\n",
              host->running_async_cleanup, host->active_works,
              host->live_tsfns);
      break;
    }
  }
}

static void close_leftover(uv_handle_t *handle, void *arg) {
  (void)arg;
  if (!uv_is_closing(handle)) uv_close(handle, NULL);
}

void qn_loop_close(qjsnapi_host *host) {
  if (host->finalizer_handles) {
    uv_close((uv_handle_t *)&host->finalizer_check, NULL);
    uv_close((uv_handle_t *)&host->finalizer_idle, NULL);
    host->finalizer_handles = false;
  }
  /* Runs the close callbacks: closing handles are processed by any
   * iteration, which does not block in NOWAIT mode. */
  uv_run(host->loop, UV_RUN_NOWAIT);
  if (!host->owns_loop) return;
  if (uv_loop_close(host->loop) == UV_EBUSY) {
    /* Handles an addon (or the embedder) never closed. */
    uv_walk(host->loop, close_leftover, NULL);
    uv_run(host->loop, UV_RUN_NOWAIT);
    uv_loop_close(host->loop);
  }
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

struct uv_loop_s *qjsnapi_host_get_loop(qjsnapi_host *host) {
  return host ? host->loop : NULL;
}

void qjsnapi_host_set_checkpoint(qjsnapi_host *host, qjsnapi_checkpoint_fn fn,
                                 void *opaque) {
  if (!host) return;
  host->checkpoint_fn = fn;
  host->checkpoint_opaque = opaque;
}

void qjsnapi_host_set_force_uncaught_exceptions_policy(qjsnapi_host *host,
                                                       int force) {
  if (host) host->force_uncaught_policy = force != 0;
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_get_uv_event_loop(node_api_basic_env env, struct uv_loop_s **loop) {
  CHECK_ENV(env);
  CHECK_ARG(env, loop);
  *loop = env->host->loop;
  return qn_clear_last_error(env);
}
