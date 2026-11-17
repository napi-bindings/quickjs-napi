/*
 * napi_async_work on libuv's thread pool (Node: uvimpl::Work).
 *
 * Lifecycle of a work item:
 *   created   napi_create_async_work; owned by the addon, linked in
 *             env->works until deleted.
 *   queued    napi_queue_async_work: uv_queue_work; counted in
 *             host->active_works. `execute`
 *             runs on a pool thread and must not call into JavaScript.
 *   completed after_work on the loop thread: unlinked and uncounted first,
 *             then `complete(env, status, data)` inside a callback frame
 *             (handle scope, callback scope, exception -> uncaught: Node's
 *             CallbackIntoModule<true>, so the policy is always enforced),
 *             status napi_cancelled when napi_cancel_async_work won.
 *             Being uncounted first lets `complete` queue the item again.
 * napi_delete_async_work frees the item; Node.js frees it even when queued
 * (undefined behaviour). Here a queued item is only marked and freed after
 * its completion, which is safe because `complete` usually deletes the work
 * itself and nothing touches the item after `complete` returns.
 *
 * At teardown (qjsnapi_host_destroy) queued items are cancelled, and the
 * loop runs until every in-flight item has completed; `complete` then runs
 * with JavaScript disabled, so the addon can still release its data. Items
 * the addon never deleted are freed with the env (qn_env_free_works).
 *
 * No async_hooks: the async resource and its name are only validated.
 */
#include <stdlib.h>

#include "qjsnapi_internal.h"

struct napi_async_work__ {
  uv_work_t req;
  qn_list link; /* env->works until deleted */
  napi_env env;
  napi_async_execute_callback execute;
  napi_async_complete_callback complete;
  void *data;
  bool queued;
  bool delete_requested;
};

/* Node: uvimpl::ConvertUVErrorCode */
static napi_status from_uv(int code) {
  switch (code) {
    case 0:
      return napi_ok;
    case UV_EINVAL:
      return napi_invalid_arg;
    case UV_ECANCELED:
      return napi_cancelled;
    default:
      return napi_generic_failure;
  }
}

static void work_execute(uv_work_t *req) {
  napi_async_work work = (napi_async_work)req->data;
  work->execute(work->env, work->data);
}

static void work_after(uv_work_t *req, int status) {
  napi_async_work work = (napi_async_work)req->data;
  napi_env env = work->env;
  work->queued = false;
  env->host->active_works--;
  napi_async_complete_callback complete = work->complete;
  void *data = work->data;
  if (work->delete_requested) {
    /* Deleted while queued: the addon no longer expects `complete`. */
    qn_list_del(&work->link);
    free(work);
    return;
  }
  if (!complete) return;
  qn_callback_frame frame;
  qn_callback_enter(env, &frame);
  complete(env, from_uv(status), data); /* may delete `work` */
  qn_callback_leave(env, &frame, true);
}

void qn_env_cancel_works(napi_env env) {
  for (qn_list *el = env->works.next; el != &env->works; el = el->next) {
    napi_async_work work = QN_CONTAINER_OF(el, struct napi_async_work__, link);
    if (work->queued)
      uv_cancel((uv_req_t *)&work->req); /* UV_EBUSY when already running */
  }
}

void qn_env_free_works(napi_env env) {
  qn_list *el = env->works.next;
  while (el != &env->works) {
    napi_async_work work = QN_CONTAINER_OF(el, struct napi_async_work__, link);
    el = el->next;
    /* Still queued only when qn_loop_drain gave up: the loop may hold the
     * request, so the item is left alone. */
    if (work->queued) continue;
    qn_list_del(&work->link);
    free(work);
  }
}

NAPI_EXTERN napi_status NAPI_CDECL napi_create_async_work(
    napi_env env, napi_value async_resource, napi_value async_resource_name,
    napi_async_execute_callback execute, napi_async_complete_callback complete,
    void *data, napi_async_work *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, execute);
  CHECK_ARG(env, result);
  /* Node: CHECK_TO_OBJECT(async_resource), CHECK_TO_STRING(name). */
  if (async_resource != NULL) {
    JSValue r = qn_value(async_resource);
    RETURN_STATUS_IF_FALSE(env, !JS_IsNull(r) && !JS_IsUndefined(r),
                           napi_object_expected);
  }
  CHECK_ARG(env, async_resource_name);
  RETURN_STATUS_IF_FALSE(env, !JS_IsSymbol(qn_value(async_resource_name)),
                         napi_string_expected);
  napi_async_work work =
      (napi_async_work)calloc(1, sizeof(struct napi_async_work__));
  RETURN_STATUS_IF_FALSE(env, work != NULL, napi_generic_failure);
  work->env = env;
  work->execute = execute;
  work->complete = complete;
  work->data = data;
  work->req.data = work;
  qn_list_add_tail(&work->link, &env->works);
  *result = work;
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_delete_async_work(napi_env env,
                                                          napi_async_work work) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, work);
  if (work->queued) {
    work->delete_requested = true; /* freed by work_after */
  } else {
    qn_list_del(&work->link);
    free(work);
  }
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_queue_async_work(node_api_basic_env env,
                                                         napi_async_work work) {
  CHECK_ENV(env);
  CHECK_ARG(env, work);
  RETURN_STATUS_IF_FALSE(env, !work->queued && !env->host->destroying,
                         napi_generic_failure);
  int r = uv_queue_work(env->host->loop, &work->req, work_execute, work_after);
  if (r != 0) return qn_set_last_error(env, from_uv(r));
  work->queued = true;
  env->host->active_works++;
  return qn_clear_last_error(env);
}

/* Fails with napi_generic_failure once the work has started (UV_EBUSY);
 * otherwise `complete` runs with napi_cancelled. */
NAPI_EXTERN napi_status NAPI_CDECL
napi_cancel_async_work(node_api_basic_env env, napi_async_work work) {
  CHECK_ENV(env);
  CHECK_ARG(env, work);
  int r = uv_cancel((uv_req_t *)&work->req);
  if (r != 0) return qn_set_last_error(env, from_uv(r));
  return qn_clear_last_error(env);
}
