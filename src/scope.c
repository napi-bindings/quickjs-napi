/*
 * Handle scopes.
 *
 * Every napi_value is a pointer to a JSValue slot in a chunked arena owned by
 * the env. Chunks never move, so a napi_value stays valid until its scope is
 * closed. Each slot owns exactly one reference: closing a scope releases, in
 * LIFO order, every slot allocated since the scope was opened.
 *
 * Escapable scopes reserve one slot in the parent scope *before* recording
 * their own start mark; napi_escape_handle stores a duplicated reference in
 * that slot, so the escaped value survives the inner scope.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "qjsnapi_internal.h"

static qn_chunk *chunk_new(qn_chunk *prev) {
  qn_chunk *c = (qn_chunk *)malloc(sizeof(qn_chunk));
  if (c) {
    c->prev = prev;
    c->next = NULL;
  }
  return c;
}

bool qn_arena_init(napi_env env) {
  qn_arena *a = &env->arena;
  a->first = chunk_new(NULL);
  if (!a->first) return false;
  a->top.chunk = a->first;
  a->top.pos = 0;
  a->current = NULL;
  a->free_scopes = NULL;
  return true;
}

napi_value qn_new_handle(napi_env env, JSValue v) {
  qn_arena *a = &env->arena;
  if (a->top.pos == QN_CHUNK_SLOTS) {
    qn_chunk *next = a->top.chunk->next;
    if (!next) {
      next = chunk_new(a->top.chunk);
      if (!next) {
        JS_FreeValue(env->ctx, v);
        return NULL;
      }
      a->top.chunk->next = next;
    }
    a->top.chunk = next;
    a->top.pos = 0;
  }
  JSValue *slot = &a->top.chunk->slots[a->top.pos++];
  *slot = v;
  return (napi_value)(void *)slot;
}

/* Releases every slot above `mark`. The position is moved *before* each
 * JS_FreeValue: a finalizer triggered by the free never sees a slot that is
 * about to be released. */
static void arena_unwind(napi_env env, qn_mark mark) {
  qn_arena *a = &env->arena;
  while (a->top.chunk != mark.chunk || a->top.pos != mark.pos) {
    if (a->top.pos == 0) {
      assert(a->top.chunk->prev != NULL && "handle arena underflow");
      a->top.chunk = a->top.chunk->prev;
      a->top.pos = QN_CHUNK_SLOTS;
      continue;
    }
    JSValue v = a->top.chunk->slots[--a->top.pos];
    JS_FreeValue(env->ctx, v);
  }
  /* Keep a single spare chunk after the current one to avoid thrashing on
   * scopes that straddle a chunk boundary; release the rest. */
  qn_chunk *spare = a->top.chunk->next;
  if (spare) {
    qn_chunk *c = spare->next;
    spare->next = NULL;
    while (c) {
      qn_chunk *n = c->next;
      free(c);
      c = n;
    }
  }
}

static void scope_recycle(qn_arena *a, qn_scope *s) {
  s->magic = 0;
  s->parent = a->free_scopes;
  a->free_scopes = s;
}

qn_scope *qn_scope_open(napi_env env, bool escapable) {
  qn_arena *a = &env->arena;
  qn_scope *s = a->free_scopes;
  if (s) {
    a->free_scopes = s->parent;
  } else {
    s = (qn_scope *)malloc(sizeof(qn_scope));
    if (!s) return NULL;
  }
  JSValue *escape_slot = NULL;
  if (escapable) {
    napi_value h = qn_new_handle(env, JS_UNDEFINED);
    if (!h) {
      scope_recycle(a, s);
      return NULL;
    }
    escape_slot = (JSValue *)(void *)h;
  }
  s->magic = QN_SCOPE_MAGIC;
  s->user = false;
  s->escapable = escapable;
  s->escaped = false;
  s->parent = a->current;
  s->mark = a->top;
  s->escape_slot = escape_slot;
  a->current = s;
  return s;
}

/* Closes `scope` and, defensively, every scope still open above it. This
 * happens when native code returns without closing a scope it opened: the
 * leaked scope is released together with the enclosing callback scope
 * instead of leaking references for the rest of the env lifetime. */
void qn_scope_close(napi_env env, qn_scope *scope) {
  qn_arena *a = &env->arena;
  for (;;) {
    qn_scope *s = a->current;
    assert(s != NULL && "closing a scope that is not open");
    if (!s) return;
    arena_unwind(env, s->mark);
    a->current = s->parent;
    if (s->user && env->open_handle_scopes > 0) env->open_handle_scopes--;
    if (s != scope) {
#ifndef NDEBUG
      fprintf(stderr,
              "quickjs-napi: warning: handle scope left open by native code "
              "was closed automatically\n");
#endif
      scope_recycle(a, s);
      continue;
    }
    scope_recycle(a, s);
    return;
  }
}

void qn_arena_destroy(napi_env env) {
  qn_arena *a = &env->arena;
  if (!a->first) return;
  qn_mark start = {a->first, 0};
  arena_unwind(env, start);
  /* Scope records are either on the open chain or on the free list. */
  qn_scope *s = a->current;
  while (s) {
    qn_scope *p = s->parent;
    free(s);
    s = p;
  }
  s = a->free_scopes;
  while (s) {
    qn_scope *p = s->parent;
    free(s);
    s = p;
  }
  qn_chunk *c = a->first;
  while (c) {
    qn_chunk *n = c->next;
    free(c);
    c = n;
  }
  a->first = NULL;
  a->current = NULL;
  a->free_scopes = NULL;
  a->top.chunk = NULL;
  a->top.pos = 0;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

static qn_scope *scope_from_handle(void *h) {
  qn_scope *s = (qn_scope *)h;
  return (s && s->magic == QN_SCOPE_MAGIC) ? s : NULL;
}

static napi_status open_user_scope(napi_env env, bool escapable, void **out) {
  qn_scope *s = qn_scope_open(env, escapable);
  RETURN_STATUS_IF_FALSE(env, s != NULL, napi_generic_failure);
  s->user = true;
  env->open_handle_scopes++;
  *out = s;
  return qn_clear_last_error(env);
}

static napi_status close_user_scope(napi_env env, void *handle,
                                    bool escapable) {
  RETURN_STATUS_IF_FALSE(env, env->open_handle_scopes > 0,
                         napi_handle_scope_mismatch);
  qn_scope *s = scope_from_handle(handle);
  RETURN_STATUS_IF_FALSE(env, s != NULL && s->user && s->escapable == escapable,
                         napi_invalid_arg);
  /* Scopes must be closed in reverse order of opening. */
  RETURN_STATUS_IF_FALSE(env, s == env->arena.current,
                         napi_handle_scope_mismatch);
  qn_scope_close(env, s);
  return qn_clear_last_error(env);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_open_handle_scope(napi_env env, napi_handle_scope *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, result);
  return open_user_scope(env, false, (void **)result);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_close_handle_scope(napi_env env, napi_handle_scope scope) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, scope);
  return close_user_scope(env, scope, false);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_open_escapable_handle_scope(
    napi_env env, napi_escapable_handle_scope *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, result);
  return open_user_scope(env, true, (void **)result);
}

NAPI_EXTERN napi_status NAPI_CDECL napi_close_escapable_handle_scope(
    napi_env env, napi_escapable_handle_scope scope) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, scope);
  return close_user_scope(env, scope, true);
}

NAPI_EXTERN napi_status NAPI_CDECL
napi_escape_handle(napi_env env, napi_escapable_handle_scope scope,
                   napi_value escapee, napi_value *result) {
  CHECK_ENV_NOT_IN_GC(env);
  CHECK_ARG(env, scope);
  CHECK_ARG(env, escapee);
  CHECK_ARG(env, result);
  qn_scope *s = scope_from_handle(scope);
  RETURN_STATUS_IF_FALSE(env, s != NULL && s->escapable, napi_invalid_arg);
  RETURN_STATUS_IF_FALSE(env, !s->escaped, napi_escape_called_twice);
  s->escaped = true;
  JSValue old = *s->escape_slot;
  *s->escape_slot = JS_DupValue(env->ctx, qn_value(escapee));
  JS_FreeValue(env->ctx, old);
  *result = (napi_value)(void *)s->escape_slot;
  return qn_clear_last_error(env);
}
