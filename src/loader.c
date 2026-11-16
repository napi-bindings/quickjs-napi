/*
 * Loading `.node` addons.
 *
 * Resolution order, as in Node.js:
 *   1. node_api_module_get_api_version_v1 (optional, default 8)
 *   2. napi_register_module_v1 (symbol-based registration)
 *   3. a napi_module registered by napi_module_register() from a static
 *      constructor that ran *during* dlopen (legacy registration)
 *
 * Addons do not link against this library: they resolve napi_* from the
 * process, so the host executable must export them (see CMakeLists.txt).
 * Library handles are never closed: finalizers and cleanup hooks may point
 * into the addon code until the very end of the process.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "qjsnapi_internal.h"

#if defined(_MSC_VER)
#define QN_THREAD_LOCAL __declspec(thread)
#else
#define QN_THREAD_LOCAL _Thread_local
#endif

/* Set by napi_module_register while a library is being loaded. */
static QN_THREAD_LOCAL napi_module *qn_pending_module;
static QN_THREAD_LOCAL int qn_loading_depth;

NAPI_EXTERN void NAPI_CDECL napi_module_register(napi_module *mod) {
  if (qn_loading_depth > 0) {
    qn_pending_module = mod;
  } else {
    /* Registration outside of qjsnapi_load_addon (e.g. an addon linked into
     * the executable): there is no module being loaded to attach it to. */
    fprintf(stderr,
            "quickjs-napi: napi_module_register(\"%s\") called outside of an "
            "addon load; use qjsnapi_init_static_addon instead\n",
            (mod && mod->nm_modname) ? mod->nm_modname : "?");
  }
}

/* ------------------------------------------------------------------------ */
/* Errors                                                                   */
/* ------------------------------------------------------------------------ */

static void throw_with_code(qjsnapi_host *host, const char *code,
                            const char *message) {
  JSContext *ctx = host->ctx;
  JSValue msg = JS_NewString(ctx, message);
  if (JS_IsException(msg)) return;
  JSValue err = JS_CallConstructor(ctx, host->error_ctors[QN_ERROR], 1, &msg);
  JS_FreeValue(ctx, msg);
  if (JS_IsException(err)) return;
  JS_SetPropertyStr(ctx, err, "code", JS_NewString(ctx, code));
  JS_Throw(ctx, err);
}

/* ------------------------------------------------------------------------ */
/* Paths                                                                    */
/* ------------------------------------------------------------------------ */

/* Absolute, canonical path in UTF-8, or NULL if the file does not exist. */
static char *qn_absolute_path(const char *path) {
#ifdef _WIN32
  int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
  if (wlen <= 0) return NULL;
  wchar_t *wpath = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
  if (!wpath) return NULL;
  MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);
  DWORD flen = GetFullPathNameW(wpath, 0, NULL, NULL);
  wchar_t *wfull =
      flen ? (wchar_t *)malloc((size_t)flen * sizeof(wchar_t)) : NULL;
  char *out = NULL;
  if (wfull && GetFullPathNameW(wpath, flen, wfull, NULL) > 0 &&
      GetFileAttributesW(wfull) != INVALID_FILE_ATTRIBUTES) {
    int len = WideCharToMultiByte(CP_UTF8, 0, wfull, -1, NULL, 0, NULL, NULL);
    out = len > 0 ? (char *)malloc((size_t)len) : NULL;
    if (out) WideCharToMultiByte(CP_UTF8, 0, wfull, -1, out, len, NULL, NULL);
  }
  free(wfull);
  free(wpath);
  return out;
#else
  return realpath(path, NULL);
#endif
}

/* file:// URL for an absolute path, as returned by
 * node_api_get_module_file_name. Bytes outside the URL path character set
 * are percent-encoded; Windows separators become '/'. */
static char *qn_path_to_file_url(const char *abs_path) {
  static const char hex[] = "0123456789ABCDEF";
  size_t n = strlen(abs_path);
  /* "file:///" + worst case 3 bytes per input byte + NUL */
  char *url = (char *)malloc(8 + 3 * n + 1);
  if (!url) return NULL;
  char *p = url;
  memcpy(p, "file://", 7);
  p += 7;
#ifdef _WIN32
  *p++ = '/'; /* file:///C:/... */
#endif
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)abs_path[i];
#ifdef _WIN32
    if (c == '\\') c = '/';
#endif
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        (c != 0 && strchr("-._~/:!$&'()*+,;=@", c) != NULL)) {
      *p++ = (char)c;
    } else {
      *p++ = '%';
      *p++ = hex[c >> 4];
      *p++ = hex[c & 15];
    }
  }
  *p = '\0';
  return url;
}

/* ------------------------------------------------------------------------ */
/* Dynamic libraries                                                        */
/* ------------------------------------------------------------------------ */

static void *qn_dlopen(const char *abs_path, char *errbuf, size_t errlen) {
#ifdef _WIN32
  int wlen = MultiByteToWideChar(CP_UTF8, 0, abs_path, -1, NULL, 0);
  wchar_t *wpath =
      wlen > 0 ? (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t)) : NULL;
  if (!wpath) {
    snprintf(errbuf, errlen, "out of memory");
    return NULL;
  }
  MultiByteToWideChar(CP_UTF8, 0, abs_path, -1, wpath, wlen);
  /* Same flags as libuv's uv_dlopen, used by Node.js. */
  HMODULE h = LoadLibraryExW(wpath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
  free(wpath);
  if (!h) {
    DWORD code = GetLastError();
    wchar_t wmsg[512];
    DWORD len = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code,
        0, wmsg, (DWORD)(sizeof(wmsg) / sizeof(wmsg[0])), NULL);
    while (len > 0 && (wmsg[len - 1] == L'\r' || wmsg[len - 1] == L'\n'))
      wmsg[--len] = L'\0';
    if (len == 0 || WideCharToMultiByte(CP_UTF8, 0, wmsg, -1, errbuf,
                                        (int)errlen, NULL, NULL) <= 0)
      snprintf(errbuf, errlen, "LoadLibraryExW failed with error %lu",
               (unsigned long)code);
  }
  return (void *)h;
#else
  void *h = dlopen(abs_path, RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    const char *e = dlerror();
    snprintf(errbuf, errlen, "%s", e ? e : "dlopen failed");
  }
  return h;
#endif
}

/* Function pointers obtained from a library: converted through an integer
 * type to keep ISO C (-Wpedantic) quiet about object/function casts. */
typedef void (*qn_generic_fn)(void);

static qn_generic_fn qn_dlsym(void *handle, const char *name) {
#ifdef _WIN32
  return (qn_generic_fn)(uintptr_t)GetProcAddress((HMODULE)handle, name);
#else
  return (qn_generic_fn)(uintptr_t)dlsym(handle, name);
#endif
}

/* ------------------------------------------------------------------------ */
/* Module initialization                                                    */
/* ------------------------------------------------------------------------ */

static qn_module_record *find_module(qjsnapi_host *host, const char *key) {
  for (qn_list *el = host->modules.next; el != &host->modules; el = el->next) {
    qn_module_record *m = QN_CONTAINER_OF(el, qn_module_record, link);
    if (strcmp(m->path, key) == 0) return m;
  }
  return NULL;
}

void qn_modules_free(qjsnapi_host *host) {
  while (!qn_list_empty(&host->modules)) {
    qn_module_record *m =
        QN_CONTAINER_OF(host->modules.next, qn_module_record, link);
    qn_list_del(&m->link);
    JS_FreeValue(host->ctx, m->exports);
    free(m->path);
    free(m);
  }
}

/* Validates the module API version like Node.js does: newer than supported
 * is an error, older than the default is raised to the default. */
static int32_t effective_api_version(qjsnapi_host *host, const char *path,
                                     int32_t version) {
  if (version == NAPI_VERSION_EXPERIMENTAL) return version;
  if (version > QN_NAPI_VERSION) {
    char msg[1024];
    snprintf(msg, sizeof(msg),
             "%s: the addon was built for Node-API version %d, but "
             "quickjs-napi supports up to version %d",
             path, (int)version, QN_NAPI_VERSION);
    throw_with_code(host, "ERR_DLOPEN_FAILED", msg);
    return -1;
  }
  if (version < QN_DEFAULT_MODULE_API_VERSION)
    version = QN_DEFAULT_MODULE_API_VERSION;
  return version;
}

static char *qn_strdup(const char *s) {
  size_t n = strlen(s) + 1;
  char *d = (char *)malloc(n);
  if (d) memcpy(d, s, n);
  return d;
}

static int run_init(qjsnapi_host *host, const char *key, void *handle,
                    napi_addon_register_func init, int32_t api_version,
                    const char *file_url, JSValue *exports) {
  JSContext *ctx = host->ctx;
  napi_env env = qn_env_new(host, api_version, file_url);
  if (!env) {
    JS_ThrowOutOfMemory(ctx);
    return -1;
  }
  /* From here on the env stays registered even on failure: the addon may
   * already have installed cleanup hooks. It is torn down with the host. */
  qn_module_record *rec = (qn_module_record *)calloc(1, sizeof(*rec));
  char *key_copy = rec ? qn_strdup(key) : NULL;
  if (!key_copy) {
    free(rec);
    JS_ThrowOutOfMemory(ctx);
    return -1;
  }

  qn_scope *scope = qn_scope_open(env, false);
  napi_value exports_h = scope ? qn_new_handle(env, JS_NewObject(ctx)) : NULL;
  if (!exports_h || JS_IsException(qn_value(exports_h))) {
    if (scope) qn_scope_close(env, scope);
    free(key_copy);
    free(rec);
    if (!JS_HasException(ctx)) JS_ThrowOutOfMemory(ctx);
    return -1;
  }

  qn_clear_last_error(env); /* Node: CallIntoModule */
  napi_value returned = init(env, exports_h);

  if (JS_HasException(ctx)) {
    qn_scope_close(env, scope);
    free(key_copy);
    free(rec);
    return -1;
  }
  /* A different returned value replaces the exports object, as Node.js
   * assigns it to module.exports. */
  JSValue result =
      JS_DupValue(ctx, qn_value(returned != NULL ? returned : exports_h));
  qn_scope_close(env, scope);

  rec->path = key_copy;
  rec->handle = handle;
  rec->env = env;
  rec->exports = JS_DupValue(ctx, result);
  qn_list_add_tail(&rec->link, &host->modules);
  *exports = result;
  return 0;
}

int qjsnapi_load_addon(qjsnapi_host *host, const char *path, JSValue *exports) {
  JSContext *ctx = host->ctx;
  if (host->destroying) {
    JS_ThrowInternalError(ctx, "quickjs-napi: host is being destroyed");
    return -1;
  }
  char *abs = qn_absolute_path(path);
  if (!abs) {
    char msg[1024];
    snprintf(msg, sizeof(msg), "Cannot find module '%s'", path);
    throw_with_code(host, "MODULE_NOT_FOUND", msg);
    return -1;
  }

  qn_module_record *cached = find_module(host, abs);
  if (cached) {
    free(abs);
    *exports = JS_DupValue(ctx, cached->exports);
    return 0;
  }

  char err[1024];
  napi_module *prev_pending = qn_pending_module;
  qn_pending_module = NULL;
  qn_loading_depth++;
  void *handle = qn_dlopen(abs, err, sizeof(err));
  qn_loading_depth--;
  napi_module *legacy = qn_pending_module;
  qn_pending_module = prev_pending;

  if (!handle) {
    throw_with_code(host, "ERR_DLOPEN_FAILED", err);
    free(abs);
    return -1;
  }

  napi_addon_register_func init =
      (napi_addon_register_func)qn_dlsym(handle, "napi_register_module_v1");
  node_api_addon_get_api_version_func get_version =
      (node_api_addon_get_api_version_func)qn_dlsym(
          handle, "node_api_module_get_api_version_v1");
  int32_t version = QN_DEFAULT_MODULE_API_VERSION;
  if (init != NULL) {
    if (get_version != NULL) version = get_version();
  } else if (legacy != NULL && legacy->nm_register_func != NULL) {
    init = legacy->nm_register_func;
  } else {
    char msg[1024];
    snprintf(msg, sizeof(msg), "Module did not self-register: '%s'.", abs);
    throw_with_code(host, "ERR_DLOPEN_FAILED", msg);
    free(abs);
    return -1;
  }

  version = effective_api_version(host, abs, version);
  if (version < 0) {
    free(abs);
    return -1;
  }

  char *url = qn_path_to_file_url(abs);
  if (!url) {
    free(abs);
    JS_ThrowOutOfMemory(ctx);
    return -1;
  }
  int r = run_init(host, abs, handle, init, version, url, exports);
  free(url);
  free(abs);
  return r;
}

int qjsnapi_init_static_addon(qjsnapi_host *host, const char *name,
                              napi_addon_register_func init,
                              int32_t module_api_version, JSValue *exports) {
  JSContext *ctx = host->ctx;
  if (!name || !init) {
    JS_ThrowTypeError(ctx, "quickjs-napi: invalid static addon");
    return -1;
  }
  if (host->destroying) {
    JS_ThrowInternalError(ctx, "quickjs-napi: host is being destroyed");
    return -1;
  }
  qn_module_record *cached = find_module(host, name);
  if (cached) {
    *exports = JS_DupValue(ctx, cached->exports);
    return 0;
  }
  int32_t version = effective_api_version(
      host, name,
      module_api_version > 0 ? module_api_version
                             : QN_DEFAULT_MODULE_API_VERSION);
  if (version < 0) return -1;
  return run_init(host, name, NULL, init, version, name, exports);
}
