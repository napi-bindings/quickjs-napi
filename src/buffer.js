// Minimal Node.js Buffer for quickjs-napi.
//
// Embedded in libqjsnapi (src/buffer.c) and evaluated once per host, before
// any user code. The completion value is a function returning
// [Buffer, makeBuffer, kMaxLength]: makeBuffer(arrayBuffer, byteOffset,
// length) is what napi_create_buffer* use. The library installs no global:
// the host decides where Buffer is visible (quickjs-napi: globalThis.Buffer
// and require('buffer')).
//
// As in Node.js, buffers are instances of FastBuffer, a Uint8Array subclass
// whose prototype is Buffer.prototype. Provided: Buffer.from, alloc,
// allocUnsafe, allocUnsafeSlow, isBuffer, isEncoding, byteLength, concat,
// compare; prototype toString, write, equals, compare, copy, fill, slice,
// subarray, toJSON, indexOf, includes. Encodings: utf8, utf16le (ucs2),
// latin1 (binary), ascii, hex, base64, base64url.
(function () {
  'use strict';

  const U8 = Uint8Array;
  const kMaxLength = 2 ** 32;

  class FastBuffer extends U8 {
    // Same signature as Uint8Array; keeps the subclass constructor cheap.
    constructor(a, b, c) { super(a, b, c); }
  }

  function makeBuffer(ab, byteOffset, length) {
    return new FastBuffer(ab, byteOffset, length);
  }

  // ------------------------------------------------------------ encodings
  function normalizeEncoding(enc) {
    if (enc === undefined || enc === null || enc === '') return 'utf8';
    switch (String(enc).toLowerCase()) {
      case 'utf8': case 'utf-8': return 'utf8';
      case 'ucs2': case 'ucs-2': case 'utf16le': case 'utf-16le': return 'utf16le';
      case 'latin1': case 'binary': return 'latin1';
      case 'ascii': return 'ascii';
      case 'hex': return 'hex';
      case 'base64': return 'base64';
      case 'base64url': return 'base64url';
      default: return undefined;
    }
  }

  function checkEncoding(enc) {
    const e = normalizeEncoding(enc);
    if (e === undefined) {
      const err = new TypeError(`Unknown encoding: ${enc}`);
      err.code = 'ERR_UNKNOWN_ENCODING';
      throw err;
    }
    return e;
  }

  function utf8Encode(s) {
    const out = [];
    for (let i = 0; i < s.length; i++) {
      let c = s.charCodeAt(i);
      if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length) {
        const d = s.charCodeAt(i + 1);
        if (d >= 0xdc00 && d <= 0xdfff) {
          c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00);
          i++;
        }
      }
      if (c >= 0xd800 && c <= 0xdfff) c = 0xfffd; // lone surrogate
      if (c < 0x80) {
        out.push(c);
      } else if (c < 0x800) {
        out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
      } else if (c < 0x10000) {
        out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
      } else {
        out.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 63),
                 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
      }
    }
    return out;
  }

  // WHATWG UTF-8 decoding: each maximal invalid subpart becomes U+FFFD.
  function utf8Decode(b, start, end) {
    let s = '';
    let chunk = [];
    const flush = () => { s += String.fromCharCode.apply(null, chunk); chunk = []; };
    let i = start;
    while (i < end) {
      const c = b[i];
      let cp = 0xfffd, n = 1;
      if (c < 0x80) {
        cp = c;
      } else if (c >= 0xc2 && c <= 0xf4) {
        const need = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
        let lo = 0x80, hi = 0xbf;
        if (c === 0xe0) lo = 0xa0;
        else if (c === 0xed) hi = 0x9f;
        else if (c === 0xf0) lo = 0x90;
        else if (c === 0xf4) hi = 0x8f;
        let v = c & (need === 1 ? 0x1f : need === 2 ? 0x0f : 0x07);
        let k = 1;
        for (; k <= need && i + k < end; k++) {
          const x = b[i + k];
          if (x < lo || x > hi) break;
          v = (v << 6) | (x & 63);
          lo = 0x80; hi = 0xbf;
        }
        if (k > need) { cp = v; n = need + 1; } else { n = k; }
      }
      if (cp > 0xffff) {
        cp -= 0x10000;
        chunk.push(0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff));
      } else {
        chunk.push(cp);
      }
      if (chunk.length >= 8192) flush();
      i += n;
    }
    flush();
    return s;
  }

  const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  const B64URL = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';
  const B64_REV = new Int8Array(256).fill(-1);
  for (let i = 0; i < 64; i++) B64_REV[B64.charCodeAt(i)] = i;
  B64_REV['-'.charCodeAt(0)] = 62;
  B64_REV['_'.charCodeAt(0)] = 63;

  // Node.js decodes base64 and base64url leniently: invalid characters are
  // skipped and decoding stops at the first '='.
  function base64Decode(s) {
    const out = [];
    let acc = 0, bits = 0;
    for (let i = 0; i < s.length; i++) {
      const c = s.charCodeAt(i);
      if (c === 61) break; // '='
      const v = c < 256 ? B64_REV[c] : -1;
      if (v < 0) continue;
      acc = ((acc << 6) | v) & 0xffffff;
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        out.push((acc >> bits) & 0xff);
      }
    }
    return out;
  }

  function base64Encode(b, start, end, url) {
    const t = url ? B64URL : B64;
    let s = '';
    let i = start;
    for (; i + 2 < end; i += 3) {
      const n = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
      s += t[n >> 18] + t[(n >> 12) & 63] + t[(n >> 6) & 63] + t[n & 63];
    }
    if (i < end) {
      const n = (b[i] << 16) | (i + 1 < end ? b[i + 1] << 8 : 0);
      s += t[n >> 18] + t[(n >> 12) & 63];
      if (i + 1 < end) s += t[(n >> 6) & 63];
      if (!url) s += i + 1 < end ? '=' : '==';
    }
    return s;
  }

  // Node.js stops at the first pair that is not a hexadecimal byte.
  function hexDecode(s) {
    const out = [];
    for (let i = 0; i + 1 < s.length; i += 2) {
      const pair = s.substr(i, 2);
      if (!/^[0-9a-fA-F]{2}$/.test(pair)) break;
      out.push(parseInt(pair, 16));
    }
    return out;
  }

  // string -> array of bytes
  function encode(str, enc) {
    switch (enc) {
      case 'utf8': return utf8Encode(str);
      case 'utf16le': {
        const out = new Array(str.length * 2);
        for (let i = 0; i < str.length; i++) {
          const c = str.charCodeAt(i);
          out[2 * i] = c & 0xff;
          out[2 * i + 1] = c >> 8;
        }
        return out;
      }
      case 'latin1': case 'ascii': {
        const out = new Array(str.length);
        for (let i = 0; i < str.length; i++) out[i] = str.charCodeAt(i) & 0xff;
        return out;
      }
      case 'hex': return hexDecode(str);
      default: return base64Decode(str); // base64, base64url
    }
  }

  function fromCodes(b, start, end, mask) {
    let s = '';
    for (let i = start; i < end; i += 8192) {
      const part = Array.prototype.slice.call(b, i, Math.min(end, i + 8192));
      s += String.fromCharCode.apply(null, mask ? part.map((x) => x & mask) : part);
    }
    return s;
  }

  // bytes [start, end) -> string
  function decode(b, start, end, enc) {
    switch (enc) {
      case 'utf8': return utf8Decode(b, start, end);
      case 'utf16le': {
        let s = '';
        for (let i = start; i + 1 < end; i += 2) {
          s += String.fromCharCode(b[i] | (b[i + 1] << 8));
        }
        return s;
      }
      case 'latin1': return fromCodes(b, start, end, 0);
      case 'ascii': return fromCodes(b, start, end, 0x7f);
      case 'hex': {
        let s = '';
        for (let i = start; i < end; i++) s += (b[i] < 16 ? '0' : '') + b[i].toString(16);
        return s;
      }
      case 'base64': return base64Encode(b, start, end, false);
      default: return base64Encode(b, start, end, true);
    }
  }

  // ---------------------------------------------------------- constructors
  function invalidArg(name, value) {
    const err = new TypeError(
      `The "${name}" argument must be of type string or an instance of ` +
      `Buffer, ArrayBuffer, or Array or an Array-like Object. Received ${
        value === null ? 'null' : typeof value}`);
    err.code = 'ERR_INVALID_ARG_TYPE';
    return err;
  }

  function checkSize(size) {
    if (typeof size !== 'number' || !(size >= 0 && size <= kMaxLength)) {
      const err = new RangeError(
        `The argument 'size' is invalid. Received ${String(size)}`);
      err.code = 'ERR_OUT_OF_RANGE';
      throw err;
    }
    return Math.floor(size);
  }

  function fromBytes(bytes) {
    const buf = new FastBuffer(bytes.length);
    buf.set(bytes);
    return buf;
  }

  function isAnyArrayBuffer(v) {
    return v instanceof ArrayBuffer ||
      (typeof SharedArrayBuffer === 'function' && v instanceof SharedArrayBuffer);
  }

  function Buffer(arg, encodingOrOffset, length) {
    if (typeof arg === 'number') return Buffer.alloc(arg);
    return Buffer.from(arg, encodingOrOffset, length);
  }

  Buffer.from = function from(value, encodingOrOffset, length) {
    if (typeof value === 'string') {
      return fromBytes(encode(value, checkEncoding(encodingOrOffset)));
    }
    if (isAnyArrayBuffer(value)) {
      const off = encodingOrOffset === undefined ? 0 : Math.trunc(Number(encodingOrOffset)) || 0;
      if (off < 0 || off > value.byteLength) {
        const err = new RangeError('"offset" is outside of buffer bounds');
        err.code = 'ERR_BUFFER_OUT_OF_BOUNDS';
        throw err;
      }
      const len = length === undefined ? value.byteLength - off : Math.trunc(Number(length)) || 0;
      if (len < 0 || off + len > value.byteLength) {
        const err = new RangeError('"length" is outside of buffer bounds');
        err.code = 'ERR_BUFFER_OUT_OF_BOUNDS';
        throw err;
      }
      return new FastBuffer(value, off, len);
    }
    if (value !== null && typeof value === 'object') {
      if (value instanceof U8) return fromBytes(value);
      if (typeof value.length === 'number') {
        return fromBytes(Array.prototype.slice.call(value).map((x) => x & 0xff));
      }
      if (value.type === 'Buffer' && Array.isArray(value.data)) {
        return fromBytes(value.data.map((x) => x & 0xff));
      }
      const prim = typeof value[Symbol.toPrimitive] === 'function' ?
        value[Symbol.toPrimitive]('string') : value.valueOf();
      if (prim !== value && prim !== null && prim !== undefined) {
        return Buffer.from(prim, encodingOrOffset, length);
      }
    }
    throw invalidArg('first', value);
  };

  Buffer.alloc = function alloc(size, fill, encoding) {
    const buf = new FastBuffer(checkSize(size));
    if (fill !== undefined && fill !== 0 && buf.length > 0) buf.fill(fill, 0, buf.length, encoding);
    return buf;
  };
  Buffer.allocUnsafe = (size) => new FastBuffer(checkSize(size));
  Buffer.allocUnsafeSlow = Buffer.allocUnsafe;
  Buffer.isBuffer = (b) => b instanceof Buffer;
  Buffer.isEncoding = (e) => typeof e === 'string' && e !== '' && normalizeEncoding(e) !== undefined;
  Buffer.byteLength = function byteLength(value, encoding) {
    if (typeof value !== 'string') {
      if (ArrayBuffer.isView(value) || isAnyArrayBuffer(value)) return value.byteLength;
      throw invalidArg('string', value);
    }
    const enc = normalizeEncoding(encoding) || 'utf8';
    if (enc === 'utf16le') return value.length * 2;
    if (enc === 'latin1' || enc === 'ascii') return value.length;
    return encode(value, enc).length;
  };
  Buffer.concat = function concat(list, totalLength) {
    if (!Array.isArray(list)) throw invalidArg('list', list);
    if (totalLength === undefined) {
      totalLength = 0;
      for (const b of list) totalLength += b.length;
    }
    const out = Buffer.alloc(totalLength);
    let pos = 0;
    for (const b of list) {
      if (!(b instanceof U8)) throw invalidArg('list', b);
      const n = Math.min(b.length, totalLength - pos);
      if (n <= 0) break;
      out.set(n === b.length ? b : b.subarray(0, n), pos);
      pos += n;
    }
    return out;
  };
  Buffer.compare = function compare(a, b) {
    const n = Math.min(a.length, b.length);
    for (let i = 0; i < n; i++) {
      if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return a.length < b.length ? -1 : a.length > b.length ? 1 : 0;
  };
  Buffer.poolSize = 8192;

  // Node.js: Buffer.prototype is FastBuffer.prototype and Buffer inherits
  // from Uint8Array (Buffer.of, species).
  Buffer.prototype = FastBuffer.prototype;
  Object.defineProperty(FastBuffer.prototype, 'constructor', {
    value: Buffer, writable: true, configurable: true, enumerable: false,
  });
  Object.setPrototypeOf(Buffer, U8);

  // ------------------------------------------------------------- prototype
  function clampRange(len, start, end) {
    start = start === undefined ? 0 : Math.max(0, Math.min(len, Math.trunc(start) || 0));
    end = end === undefined ? len : Math.max(start, Math.min(len, Math.trunc(end) || 0));
    return [start, end];
  }

  const methods = {
    toString(encoding, start, end) {
      if (arguments.length === 0) return utf8Decode(this, 0, this.length);
      const [s, e] = clampRange(this.length, start, end);
      return decode(this, s, e, checkEncoding(encoding));
    },
    toLocaleString(encoding, start, end) {
      return this.toString(encoding, start, end);
    },
    write(string, offset, length, encoding) {
      if (typeof offset === 'string') { encoding = offset; offset = 0; length = undefined; }
      else if (typeof length === 'string') { encoding = length; length = undefined; }
      offset = offset === undefined ? 0 : offset >>> 0;
      const remaining = Math.max(0, this.length - offset);
      length = length === undefined ? remaining : Math.min(remaining, length >>> 0);
      const enc = checkEncoding(encoding);
      let bytes = encode(String(string), enc);
      if (bytes.length > length) {
        bytes = bytes.slice(0, length);
        // Never write a partial UTF-8 character.
        if (enc === 'utf8') {
          let k = bytes.length;
          while (k > 0 && (bytes[k - 1] & 0xc0) === 0x80) k--;
          if (k > 0) {
            const lead = bytes[k - 1];
            const size = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
            if (bytes.length - (k - 1) < size) bytes = bytes.slice(0, k - 1);
          }
        }
      }
      this.set(bytes, offset);
      return bytes.length;
    },
    equals(other) {
      if (!(other instanceof U8)) throw invalidArg('otherBuffer', other);
      return Buffer.compare(this, other) === 0;
    },
    compare(target) {
      return Buffer.compare(this, target);
    },
    copy(target, targetStart, sourceStart, sourceEnd) {
      targetStart = targetStart === undefined ? 0 : Math.max(0, Math.trunc(targetStart) || 0);
      const [s, e] = clampRange(this.length, sourceStart, sourceEnd);
      const n = Math.max(0, Math.min(e - s, target.length - targetStart));
      if (n > 0) target.set(this.subarray(s, s + n), targetStart);
      return n;
    },
    fill(value, offset, end, encoding) {
      if (typeof offset === 'string') { encoding = offset; offset = 0; end = this.length; }
      else if (typeof end === 'string') { encoding = end; end = this.length; }
      const [s, e] = clampRange(this.length, offset, end);
      let pattern = null;
      if (typeof value === 'string') pattern = encode(value, checkEncoding(encoding));
      else if (value instanceof U8) pattern = value;
      if (pattern === null) return U8.prototype.fill.call(this, Number(value) & 0xff, s, e);
      if (pattern.length === 0) return U8.prototype.fill.call(this, 0, s, e);
      for (let i = s; i < e; i++) this[i] = pattern[(i - s) % pattern.length];
      return this;
    },
    // Node.js: slice() shares memory, like subarray().
    slice(start, end) {
      return this.subarray(start, end);
    },
    toJSON() {
      return { type: 'Buffer', data: Array.prototype.slice.call(this) };
    },
    indexOf(value, byteOffset, encoding) {
      const needle = typeof value === 'number' ? [value & 0xff] :
        typeof value === 'string' ? encode(value, checkEncoding(encoding)) : value;
      let from = byteOffset === undefined ? 0 : Math.trunc(byteOffset) || 0;
      if (from < 0) from = Math.max(0, this.length + from);
      if (needle.length === 0) return Math.min(from, this.length);
      outer: for (let i = from; i + needle.length <= this.length; i++) {
        for (let k = 0; k < needle.length; k++) {
          if (this[i + k] !== needle[k]) continue outer;
        }
        return i;
      }
      return -1;
    },
    includes(value, byteOffset, encoding) {
      return this.indexOf(value, byteOffset, encoding) !== -1;
    },
  };
  for (const name of Object.keys(methods)) {
    Object.defineProperty(Buffer.prototype, name, {
      value: methods[name], writable: true, configurable: true, enumerable: false,
    });
  }

  return [Buffer, makeBuffer, kMaxLength];
})
