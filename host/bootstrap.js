// quickjs-napi host bootstrap: CommonJS modules and a minimal `process`.
//
// Evaluated once by host/main.c, which passes the native helpers. The
// completion value is a function: (natives) => { runMain, emitExit, ... }.
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
    env: (() => {
      const env = {};
      for (const kv of natives.environ) {
        // "=C:=C:\\dir" entries (Windows drive cwds) have an empty name.
        const i = kv.indexOf('=', 1);
        if (i > 0) env[kv.slice(0, i)] = kv.slice(i + 1);
      }
      return env;
    })(),
    platform: natives.platform,
    execPath: natives.execPath,
    // Same as napi_get_node_version: the Node.js release whose Node-API
    // behaviour is reproduced (process.versions.node is not set, so that
    // packages do not take the host for Node.js itself).
    version: `v${natives.nodeVersion}`,
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
  // The lists live here; the host owns one uv_timer_t and one uv_idle_t
  // and runs the callbacks one at a time (runDueTimer, runImmediate), with
  // a microtask checkpoint after each, as Node.js does. rearm() tells the
  // host when the next timer is due and whether something keeps the loop
  // alive, every time the lists or the ref state change.
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

  function rearmTimers() {
    const now = Date.now();
    let wait = -1;
    let refed = false;
    for (const t of timers) {
      const d = Math.max(t._due - now, 0);
      if (wait < 0 || d < wait) wait = d;
      if (t._ref) refed = true;
    }
    natives.armTimer(wait, refed);
  }

  function rearmImmediates() {
    let pending = false;
    let refed = false;
    for (const i of immediates) {
      if (!i._active) continue;
      pending = true;
      if (i._ref) refed = true;
    }
    natives.armImmediate(pending, refed);
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
    ref() { this._ref = true; rearmTimers(); return this; }
    unref() { this._ref = false; rearmTimers(); return this; }
    hasRef() { return this._ref; }
    refresh() {
      this._due = Date.now() + this._ms;
      if (this._active) rearmTimers();
      return this;
    }
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
    ref() { this._ref = true; rearmImmediates(); return this; }
    unref() { this._ref = false; rearmImmediates(); return this; }
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
    rearmTimers();
    return t;
  }
  function setInterval(fn, ms, ...args) {
    checkCallback(fn);
    const t = new Timeout(fn, args, delay(ms), true);
    timers.push(t);
    rearmTimers();
    return t;
  }
  function clearTimeout(t) {
    const timer = typeof t === 'object' && t !== null ? t :
      timers.find((x) => x._id === Number(t));
    if (timer instanceof Timeout && timer._active) {
      timer._active = false;
      const i = timers.indexOf(timer);
      if (i >= 0) timers.splice(i, 1);
      rearmTimers();
    }
  }
  function setImmediate(fn, ...args) {
    checkCallback(fn);
    const i = new Immediate(fn, args);
    immediates.push(i);
    rearmImmediates();
    return i;
  }
  function clearImmediate(i) {
    if (i instanceof Immediate && i._active) {
      i._active = false;
      rearmImmediates();
    }
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

  // Timers phase: runs the earliest due timer and returns true, or returns
  // false when none is due (the host then stops calling).
  function runDueTimer() {
    const now = Date.now();
    let next = null;
    for (const t of timers) {
      if (t._due <= now && (next === null || t._due < next._due)) next = t;
    }
    if (next === null) {
      rearmTimers();
      return false;
    }
    if (next._repeat) {
      next._due = now + next._ms;
    } else {
      next._active = false;
      timers.splice(timers.indexOf(next), 1);
    }
    rearmTimers();
    invoke(next);
    return true;
  }

  // Check phase: the host runs as many immediates as were queued when the
  // phase began (immediateCount); newer ones wait for the next iteration.
  function immediateCount() {
    return immediates.length;
  }
  function runImmediate() {
    while (immediates.length > 0) {
      const i = immediates.shift();
      if (i._active) {
        i._active = false;
        rearmImmediates();
        invoke(i);
        return true;
      }
    }
    rearmImmediates();
    return false;
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

  // --------------------------------------------- buffer, util, url, vm
  // Buffer comes from the library (napi_create_buffer returns its
  // instances); see src/buffer.js for what it provides.
  globalThis.Buffer = natives.Buffer;
  builtins.buffer = {
    Buffer: natives.Buffer,
    kMaxLength: 2 ** 32,
    constants: { MAX_LENGTH: 2 ** 32 },
  };

  const tag = (v) => Object.prototype.toString.call(v).slice(8, -1);
  const TypedArray = Object.getPrototypeOf(Int8Array);
  const customPromisify = Symbol.for('nodejs.util.promisify.custom');
  function promisify(fn) {
    checkCallback(fn);
    if (typeof fn[customPromisify] === 'function') return fn[customPromisify];
    function promisified(...args) {
      return new Promise((resolve, reject) => {
        fn.call(this, ...args, (err, value) => (err ? reject(err) : resolve(value)));
      });
    }
    Object.setPrototypeOf(promisified, Object.getPrototypeOf(fn));
    return Object.defineProperties(promisified, Object.getOwnPropertyDescriptors(fn));
  }
  promisify.custom = customPromisify;
  builtins.util = {
    promisify,
    types: {
      isArrayBuffer: (v) => v instanceof ArrayBuffer,
      isSharedArrayBuffer: (v) => typeof SharedArrayBuffer === 'function' &&
                                  v instanceof SharedArrayBuffer,
      isAnyArrayBuffer: (v) => v instanceof ArrayBuffer ||
        (typeof SharedArrayBuffer === 'function' && v instanceof SharedArrayBuffer),
      isArrayBufferView: (v) => ArrayBuffer.isView(v),
      isTypedArray: (v) => v instanceof TypedArray,
      isUint8Array: (v) => v instanceof Uint8Array,
      isDataView: (v) => v instanceof DataView,
      isDate: (v) => v instanceof Date,
      isPromise: (v) => v instanceof Promise,
      isRegExp: (v) => v instanceof RegExp,
      isNativeError: (v) => v instanceof Error && tag(v) === 'Error',
    },
  };

  // url.pathToFileURL: the encoding of Node.js (encodePathChars, then the
  // URL path percent-encode set). Returns a plain object with href and
  // pathname: QuickJS has no URL class.
  function encodePath(p) {
    let out = '';
    for (const ch of p) {
      const c = ch.codePointAt(0);
      if (c > 0x20 && c < 0x7f && !'"#%<>?`{}'.includes(ch) &&
          (isWindows || ch !== '\\')) {
        out += ch;
        continue;
      }
      for (const byte of natives.Buffer.from(ch, 'utf8')) {
        out += '%' + (byte < 16 ? '0' : '') + byte.toString(16).toUpperCase();
      }
    }
    return out;
  }
  function fileURL(pathname) {
    const href = `file://${pathname}`;
    return { href, protocol: 'file:', pathname, toString: () => href };
  }
  builtins.url = {
    pathToFileURL(p) {
      let resolved = resolvePath(natives.cwd(), String(p));
      if (/[\\/]$/.test(String(p)) && !/[\\/]$/.test(resolved)) resolved += sep;
      if (isWindows) resolved = '/' + resolved.replace(/\\/g, '/');
      return fileURL(encodePath(resolved));
    },
    fileURLToPath(u) {
      const href = typeof u === 'string' ? u : u && u.href;
      if (typeof href !== 'string' || !href.startsWith('file://')) {
        const err = new TypeError('The URL must be of scheme file');
        err.code = 'ERR_INVALID_URL_SCHEME';
        throw err;
      }
      let p = decodeURIComponent(href.slice(7).replace(/[?#].*$/, ''));
      if (isWindows) p = p.replace(/^\/(?=[a-zA-Z]:)/, '').replace(/\//g, '\\');
      return p;
    },
  };

  // vm: only runInNewContext(code), evaluated in a new realm.
  builtins.vm = {
    runInNewContext: (code) => natives.runInNewContext(String(code)),
  };
  builtins.module.builtinModules.push('buffer', 'util', 'url', 'vm');

  // ------------------------------------------------------- child_process
  // spawnSync and spawn (no shell: exec/execSync are not provided), on
  // natives.spawnSync / natives.spawn (host/spawn.c). stdio: 'pipe',
  // 'inherit', 'ignore' (a string or an array of three); an asynchronous
  // child's stdin is never a pipe.
  class Emitter {
    constructor() { this._events = Object.create(null); }
    on(event, fn) {
      (this._events[event] || (this._events[event] = [])).push(fn);
      return this;
    }
    once(event, fn) {
      const wrapped = (...args) => {
        this.removeListener(event, wrapped);
        fn.apply(this, args);
      };
      wrapped.listener = fn;
      return this.on(event, wrapped);
    }
    removeListener(event, fn) {
      const l = this._events[event];
      if (l) {
        const i = l.findIndex((f) => f === fn || f.listener === fn);
        if (i >= 0) l.splice(i, 1);
      }
      return this;
    }
    emit(event, ...args) {
      const l = this._events[event];
      if (!l || l.length === 0) {
        if (event === 'error') throw args[0];
        return false;
      }
      for (const fn of l.slice()) fn.apply(this, args);
      return true;
    }
  }
  Emitter.prototype.addListener = Emitter.prototype.on;
  Emitter.prototype.off = Emitter.prototype.removeListener;

  function stdioModes(stdio) {
    if (stdio === undefined) return ['pipe', 'pipe', 'pipe'];
    if (typeof stdio === 'string') return [stdio, stdio, stdio];
    return [0, 1, 2].map((i) => {
      const m = stdio[i];
      return m === undefined || m === null ? 'pipe' : m;
    });
  }

  function envList(env) {
    if (env === undefined || env === null) return undefined;
    return Object.keys(env).filter((k) => env[k] !== undefined)
      .map((k) => `${k}=${env[k]}`);
  }

  function spawnArgs(file, args, options) {
    if (typeof file !== 'string') {
      const e = new TypeError('The "file" argument must be of type string');
      e.code = 'ERR_INVALID_ARG_TYPE';
      throw e;
    }
    if (!Array.isArray(args)) {
      options = args;
      args = [];
    }
    options = options || {};
    return {
      args: args.map(String),
      options,
      native: {
        env: envList(options.env),
        cwd: options.cwd,
        stdio: stdioModes(options.stdio),
      },
    };
  }

  function spawnError(code, syscall, file, args) {
    const e = new Error(`${syscall} ${file} ${code}`);
    e.code = code;
    e.errno = code;
    e.syscall = `${syscall} ${file}`;
    e.path = file;
    e.spawnargs = args;
    return e;
  }

  function spawnSync(file, args, options) {
    const a = spawnArgs(file, args, options);
    const o = a.options;
    const input = o.input;
    if (input !== undefined) {
      a.native.input = typeof input === 'string'
        ? natives.Buffer.from(input, o.encoding || 'utf8').buffer
        : ArrayBuffer.isView(input)
          ? input.buffer.slice(input.byteOffset, input.byteOffset + input.byteLength)
          : input;
    }
    if (o.timeout) a.native.timeout = o.timeout;
    if (o.killSignal !== undefined) a.native.killSignal = o.killSignal;
    const r = natives.spawnSync(file, a.args, a.native);
    const conv = (ab) => {
      if (ab === null) return null;
      const b = natives.Buffer.from(ab);
      return o.encoding && o.encoding !== 'buffer' ? b.toString(o.encoding) : b;
    };
    const stdout = conv(r.stdout);
    const stderr = conv(r.stderr);
    const result = {
      pid: r.pid,
      output: [null, stdout, stderr],
      stdout,
      stderr,
      status: r.status,
      signal: r.signal,
    };
    if (r.error) result.error = spawnError(r.error, 'spawnSync', file, a.args);
    return result;
  }

  class Readable extends Emitter {
    constructor() {
      super();
      this._encoding = null;
    }
    setEncoding(enc) {
      this._encoding = enc;
      return this;
    }
    _push(ab) {
      const b = natives.Buffer.from(ab);
      this.emit('data', this._encoding ? b.toString(this._encoding) : b);
    }
  }

  class ChildProcess extends Emitter {
    constructor() {
      super();
      this.pid = undefined;
      this.exitCode = null;
      this.signalCode = null;
      this.killed = false;
      this.stdin = null;
      this.stdout = null;
      this.stderr = null;
      this._handle = null;
    }
    kill(signal) {
      if (!this._handle) return false;
      const ok = this._handle.kill(signal === undefined ? 'SIGTERM' : signal);
      if (ok) this.killed = true;
      return ok;
    }
  }

  function spawn(file, args, options) {
    const a = spawnArgs(file, args, options);
    const child = new ChildProcess();
    child.spawnfile = file;
    child.spawnargs = [file, ...a.args];
    const modes = a.native.stdio;
    if (modes[1] === 'pipe') child.stdout = new Readable();
    if (modes[2] === 'pipe') child.stderr = new Readable();
    const onEvent = (type, x, y) => {
      if (type === 'stdout') {
        child.stdout._push(x);
      } else if (type === 'stderr') {
        child.stderr._push(x);
      } else if (type === 'exit') {
        child.exitCode = x;
        child.signalCode = y;
        child.emit('exit', x, y);
      } else if (type === 'close') {
        if (child.stdout) child.stdout.emit('end');
        if (child.stderr) child.stderr.emit('end');
        child._handle = null;
        child.emit('close', x, y);
      }
    };
    try {
      child._handle = natives.spawn(file, a.args, a.native, onEvent);
      child.pid = child._handle.pid;
    } catch (e) {
      if (!e.code) throw e;
      const err = spawnError(e.code, 'spawn', file, a.args);
      process.nextTick(() => {
        child.emit('error', err);
        child.emit('close', -2, null);
      });
    }
    return child;
  }

  builtins.child_process = { spawnSync, spawn, ChildProcess };
  builtins.events = Emitter;
  Emitter.EventEmitter = Emitter;
  builtins.module.builtinModules.push('child_process', 'events');

  // -r / --require (command line, NODE_OPTIONS): loaded before the main
  // module, relative to the current directory.
  function runPreloads() {
    const req = makeRequire(new Module(resolvePath(natives.cwd(), '[preload]'), null));
    for (const p of natives.preloads) req(p.startsWith('.') ? resolvePath(natives.cwd(), p) : p);
  }

  function runMain(path) {
    runPreloads();
    const real = tryFile(resolvePath(natives.cwd(), path));
    if (!real) throw notFound(path, null);
    mainModule = new Module(real, null);
    mainModule.id = '.';
    cache[real] = mainModule;
    compileAndRun(mainModule, natives.readFile(real));
    mainModule.loaded = true;
  }

  // -e / --eval: the code runs as a module of the current directory, with
  // process.argv = [execPath, args...] (no script).
  function runEval(code) {
    runPreloads();
    const filename = resolvePath(natives.cwd(), '[eval]');
    mainModule = new Module(filename, null);
    mainModule.id = '[eval]';
    compileAndRun(mainModule, code);
    mainModule.loaded = true;
  }

  function emitBeforeExit() {
    process.emit('beforeExit', process.exitCode | 0);
  }

  return {
    runMain, runEval, emitExit, emitBeforeExit, runDueTimer, immediateCount,
    runImmediate, fatalException, unhandledRejection,
  };
})
