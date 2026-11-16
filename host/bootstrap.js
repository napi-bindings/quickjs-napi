// quickjs-napi host bootstrap: CommonJS modules and a minimal `process`.
//
// Evaluated once by host/main.c, which passes the native helpers. The
// completion value is a function: (natives) => { runMain, emitExit }.
//
// Module resolution follows Node.js for what addon packages use: relative
// and absolute paths, extensions .js/.json/.node, directories (package.json
// "main", index.*), node_modules lookup, builtin modules. ES modules,
// "exports" maps and conditional exports are not supported.
(function bootstrap(natives) {
  'use strict';

  const isWindows = natives.platform === 'win32';
  const sep = isWindows ? '\\' : '/';

  // ---------------------------------------------------------------- paths
  function isSep(c) { return c === '/' || (isWindows && c === '\\'); }

  function rootOf(p) {
    if (isWindows) {
      const m = /^(?:[a-zA-Z]:)?[\\/]?/.exec(p);
      return m ? m[0] : '';
    }
    return p[0] === '/' ? '/' : '';
  }

  function isAbsolute(p) {
    return isWindows ? /^(?:[a-zA-Z]:)?[\\/]/.test(p) : p[0] === '/';
  }

  function normalize(p) {
    const root = rootOf(p);
    const parts = [];
    for (const seg of p.slice(root.length).split(isWindows ? /[\\/]+/ : /\/+/)) {
      if (seg === '' || seg === '.') continue;
      if (seg === '..') {
        if (parts.length && parts[parts.length - 1] !== '..') parts.pop();
        else if (!root) parts.push('..');
        continue;
      }
      parts.push(seg);
    }
    const r = (isWindows ? root.replace(/\//g, '\\') : root) + parts.join(sep);
    return r || '.';
  }

  function join(a, b) { return normalize(a + sep + b); }

  function resolvePath(base, p) {
    return isAbsolute(p) ? normalize(p) : normalize(base + sep + p);
  }

  function dirname(p) {
    let i = p.length - 1;
    while (i > 0 && !isSep(p[i])) i--;
    if (i <= 0) return isSep(p[0]) ? p[0] : '.';
    const root = rootOf(p);
    return i < root.length ? root : p.slice(0, i);
  }

  // -------------------------------------------------------------- modules
  const builtins = Object.create(null);
  const cache = Object.create(null);
  const extensions = ['.js', '.json', '.node'];

  function Module(id, parent) {
    this.id = id;
    this.filename = id;
    this.path = dirname(id);
    this.exports = {};
    this.parent = parent;
    this.children = [];
    this.loaded = false;
    if (parent) parent.children.push(this);
  }

  function tryFile(p) {
    return natives.stat(p) === 1 ? natives.realpath(p) : null;
  }

  function tryExtensions(p) {
    const exact = tryFile(p);
    if (exact) return exact;
    for (const ext of extensions) {
      const f = tryFile(p + ext);
      if (f) return f;
    }
    return null;
  }

  function tryDirectory(dir) {
    if (natives.stat(dir) !== 2) return null;
    const pkgPath = join(dir, 'package.json');
    if (natives.stat(pkgPath) === 1) {
      let main;
      try {
        main = JSON.parse(natives.readFile(pkgPath)).main;
      } catch (e) {
        e.message = `Error parsing ${pkgPath}: ${e.message}`;
        throw e;
      }
      if (typeof main === 'string' && main) {
        const m = resolvePath(dir, main);
        const f = tryExtensions(m) || tryExtensions(join(m, 'index'));
        if (f) return f;
      }
    }
    return tryExtensions(join(dir, 'index'));
  }

  function notFound(request, parent) {
    const err = new Error(`Cannot find module '${request}'` +
                          (parent ? `\nRequire stack:\n- ${parent.filename}` : ''));
    err.code = 'MODULE_NOT_FOUND';
    return err;
  }

  function resolveFilename(request, parent) {
    if (typeof request !== 'string' || request === '') {
      const err = new TypeError('The "id" argument must be a non-empty string');
      err.code = 'ERR_INVALID_ARG_VALUE';
      throw err;
    }
    const bare = request.startsWith('node:') ? request.slice(5) : request;
    if (bare in builtins) return bare;
    const base = parent ? parent.path : natives.cwd();
    const relative = request === '.' || request === '..' ||
                     (isWindows ? /^\.\.?[\\/]/ : /^\.\.?\//).test(request);
    if (relative || isAbsolute(request)) {
      const p = resolvePath(base, request);
      const f = tryExtensions(p) || tryDirectory(p);
      if (f) return f;
      throw notFound(request, parent);
    }
    for (let dir = base; ; dir = dirname(dir)) {
      if (!/[\\/]node_modules$/.test(dir)) {
        const candidate = join(join(dir, 'node_modules'), request);
        const f = tryExtensions(candidate) || tryDirectory(candidate);
        if (f) return f;
      }
      if (dirname(dir) === dir) break;
    }
    throw notFound(request, parent);
  }

  let mainModule;

  function makeRequire(module) {
    function require(id) {
      return loadModule(id, module);
    }
    require.resolve = (id) => resolveFilename(id, module);
    require.cache = cache;
    require.main = mainModule;
    return require;
  }

  function compileAndRun(module, source) {
    if (source.startsWith('#!')) source = '//' + source;
    // Same line numbers as the file: the wrapper opens on line 1.
    const wrapper = natives.compile(
      '(function (exports, require, module, __filename, __dirname) { ' +
      source + '\n})', module.filename);
    wrapper.call(module.exports, module.exports, makeRequire(module), module,
                 module.filename, module.path);
  }

  function loadModule(request, parent) {
    const filename = resolveFilename(request, parent);
    if (filename in builtins) return builtins[filename];
    const cached = cache[filename];
    if (cached) return cached.exports;
    const module = new Module(filename, parent);
    cache[filename] = module;
    try {
      if (filename.endsWith('.node')) {
        module.exports = natives.loadAddon(filename);
      } else if (filename.endsWith('.json')) {
        module.exports = JSON.parse(natives.readFile(filename));
      } else {
        compileAndRun(module, natives.readFile(filename));
      }
    } catch (e) {
      delete cache[filename];
      throw e;
    }
    module.loaded = true;
    return module.exports;
  }

  builtins.module = {
    builtinModules: ['module'],
    createRequire(filename) {
      return makeRequire(new Module(resolvePath(natives.cwd(), filename), null));
    },
    // quickjs-napi extension: lets a script provide a builtin module (e.g. a
    // test harness providing 'assert').
    registerBuiltin(name, exports) {
      builtins[name] = exports;
      if (!this.builtinModules.includes(name)) this.builtinModules.push(name);
    },
  };

  // -------------------------------------------------------------- process
  const listeners = Object.create(null);
  let exiting = false;
  function emitExit() {
    if (!exiting) {
      exiting = true;
      process.emit('exit', process.exitCode | 0);
    }
    return process.exitCode | 0;
  }

  const process = {
    argv: natives.argv,
    env: {},
    platform: natives.platform,
    execPath: natives.execPath,
    release: { name: 'quickjs-napi' },
    versions: { quickjs: natives.quickjsVersion,
                napi: String(natives.napiVersion) },
    exitCode: undefined,
    cwd: () => natives.cwd(),
    nextTick(fn, ...args) {
      checkCallback(fn);
      Promise.resolve().then(() => {
        try {
          fn(...args);
        } catch (e) {
          fatalException(e);
        }
      });
    },
    on(event, fn) {
      (listeners[event] || (listeners[event] = [])).push(fn);
      return process;
    },
    once(event, fn) {
      const wrapped = (...args) => {
        process.removeListener(event, wrapped);
        fn(...args);
      };
      wrapped.listener = fn;
      return process.on(event, wrapped);
    },
    removeListener(event, fn) {
      const list = listeners[event];
      if (list) {
        const i = list.findIndex((l) => l === fn || l.listener === fn);
        if (i >= 0) list.splice(i, 1);
      }
      return process;
    },
    listeners(event) { return (listeners[event] || []).slice(); },
    emit(event, ...args) {
      const list = listeners[event];
      if (!list || list.length === 0) return false;
      for (const fn of list.slice()) fn.apply(process, args);
      return true;
    },
    exit(code) {
      if (code !== undefined) process.exitCode = code;
      emitExit();
      natives.exit(process.exitCode | 0);
    },
  };
  process.addListener = process.on;
  process.off = process.removeListener;

  globalThis.process = process;
  globalThis.global = globalThis;

  // -------------------------------------------------------------- console
  // quickjs-libc only provides console.log.
  function formatArgs(args) {
    return args.map((a) => (typeof a === 'string' ? a : String(a))).join(' ');
  }
  if (typeof console === 'object') {
    if (!console.error) {
      console.error = (...args) => natives.writeStderr(formatArgs(args) + '\n');
    }
    if (!console.warn) console.warn = console.error;
    if (!console.info) console.info = console.log;
  }

  // ----------------------------------------------------- uncaught errors
  function formatError(err) {
    if (err !== null && typeof err === 'object' && typeof err.stack === 'string') {
      // QuickJS stacks omit the "Name: message" line that V8 prints first.
      return `${String(err)}\n${err.stack}`;
    }
    return `Uncaught ${String(err)}`;
  }

  // Called by the host for an exception no JavaScript frame can catch:
  // a throwing timer or microtask, a Node-API finalizer, the main module.
  function fatalException(err) {
    if (process.listeners('uncaughtException').length > 0) {
      process.emit('uncaughtException', err, 'uncaughtException');
      return;
    }
    natives.writeStderr(formatError(err) + '\n');
    process.exit(1);
  }

  // Node.js default (--unhandled-rejections=throw).
  function unhandledRejection(reason, promise) {
    if (!process.emit('unhandledRejection', reason, promise)) {
      fatalException(reason);
    }
  }

  // --------------------------------------------------------------- timers
  // Run by the host loop one callback at a time (runTimers), so that
  // microtasks and Node-API finalizers run between callbacks as in Node.js.
  const timers = [];
  const immediates = [];
  let nextTimerId = 1;

  function checkCallback(fn) {
    if (typeof fn !== 'function') {
      const e = new TypeError(
        'The "callback" argument must be of type function. Received ' +
        (fn === null ? 'null' : typeof fn));
      e.code = 'ERR_INVALID_ARG_TYPE';
      throw e;
    }
  }

  class Timeout {
    constructor(fn, args, ms, repeat) {
      this._id = nextTimerId++;
      this._fn = fn;
      this._args = args;
      this._ms = ms;
      this._repeat = repeat;
      this._due = Date.now() + ms;
      this._active = true;
      this._ref = true;
    }
    ref() { this._ref = true; return this; }
    unref() { this._ref = false; return this; }
    hasRef() { return this._ref; }
    refresh() { this._due = Date.now() + this._ms; return this; }
    close() { clearTimeout(this); return this; }
    [Symbol.toPrimitive]() { return this._id; }
  }

  class Immediate {
    constructor(fn, args) {
      this._fn = fn;
      this._args = args;
      this._active = true;
      this._ref = true;
    }
    ref() { this._ref = true; return this; }
    unref() { this._ref = false; return this; }
    hasRef() { return this._ref; }
  }

  function delay(ms) {
    ms = Number(ms);
    return ms >= 1 && ms <= 2147483647 ? Math.trunc(ms) : 1;
  }

  function setTimeout(fn, ms, ...args) {
    checkCallback(fn);
    const t = new Timeout(fn, args, delay(ms), false);
    timers.push(t);
    return t;
  }
  function setInterval(fn, ms, ...args) {
    checkCallback(fn);
    const t = new Timeout(fn, args, delay(ms), true);
    timers.push(t);
    return t;
  }
  function clearTimeout(t) {
    const timer = typeof t === 'object' && t !== null ? t :
      timers.find((x) => x._id === Number(t));
    if (timer instanceof Timeout && timer._active) {
      timer._active = false;
      const i = timers.indexOf(timer);
      if (i >= 0) timers.splice(i, 1);
    }
  }
  function setImmediate(fn, ...args) {
    checkCallback(fn);
    const i = new Immediate(fn, args);
    immediates.push(i);
    return i;
  }
  function clearImmediate(i) {
    if (i instanceof Immediate) i._active = false;
  }
  function queueMicrotask(fn) {
    checkCallback(fn);
    Promise.resolve().then(() => {
      try {
        fn();
      } catch (e) {
        fatalException(e);
      }
    });
  }

  function invoke(task) {
    try {
      task._fn(...task._args);
    } catch (e) {
      fatalException(e);
    }
  }

  // Runs one due timer, else one immediate, and returns 0; otherwise returns
  // the delay in ms until the next referenced timer, or -1 when none is left.
  function runTimers() {
    const now = Date.now();
    let next = null;
    for (const t of timers) {
      if (t._due <= now && (next === null || t._due < next._due)) next = t;
    }
    if (next !== null) {
      if (next._repeat) {
        next._due = now + next._ms;
      } else {
        next._active = false;
        timers.splice(timers.indexOf(next), 1);
      }
      invoke(next);
      return 0;
    }
    while (immediates.length > 0) {
      const i = immediates.shift();
      if (i._active) {
        i._active = false;
        invoke(i);
        return 0;
      }
    }
    let wait = -1;
    for (const t of timers) {
      if (t._ref && (wait < 0 || t._due - now < wait)) wait = Math.max(t._due - now, 1);
    }
    return wait;
  }

  Object.assign(globalThis, {
    setTimeout, setInterval, clearTimeout, clearInterval: clearTimeout,
    setImmediate, clearImmediate, queueMicrotask,
  });
  builtins.timers = {
    setTimeout, setInterval, clearTimeout, clearInterval: clearTimeout,
    setImmediate, clearImmediate,
  };
  builtins['timers/promises'] = {
    setTimeout: (ms, value) => new Promise((resolve) => setTimeout(resolve, ms, value)),
    setImmediate: (value) => new Promise((resolve) => setImmediate(resolve, value)),
  };
  builtins.module.builtinModules.push('timers', 'timers/promises');

  function runMain(path) {
    const real = tryFile(resolvePath(natives.cwd(), path));
    if (!real) throw notFound(path, null);
    mainModule = new Module(real, null);
    mainModule.id = '.';
    cache[real] = mainModule;
    compileAndRun(mainModule, natives.readFile(real));
    mainModule.loaded = true;
  }

  return { runMain, emitExit, runTimers, fatalException, unhandledRejection };
})
