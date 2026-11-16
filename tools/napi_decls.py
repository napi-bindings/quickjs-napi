"""Parses the Node-API declarations from include/js_native_api.h and
include/node_api.h.

Shared by gen_stubs.py and check_exports.py so that both tools agree on the
exact set of exported functions.
"""

import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADERS = [
    os.path.join(ROOT, "include", "js_native_api.h"),
    os.path.join(ROOT, "include", "node_api.h"),
]

_DECL_RE = re.compile(
    r"NAPI_EXTERN\s+(?P<noreturn>NAPI_NO_RETURN\s+)?(?P<ret>\w+)\s+NAPI_CDECL\s+"
    r"(?P<name>\w+)\s*\((?P<params>[^;]*?)\)\s*;",
    re.S,
)


class Decl:
    def __init__(self, ret, name, params, noreturn):
        self.ret = ret
        self.name = name
        self.params = params  # list of "type name" strings
        self.noreturn = noreturn

    @property
    def first_param_is_env(self):
        if not self.params:
            return False
        p = self.params[0].split()
        return (
            len(p) == 2
            and p[0] in ("napi_env", "node_api_basic_env")
            and p[1] == "env"
        )


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def _split_params(params):
    out, depth, cur = [], 0, ""
    for ch in params:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return [" ".join(p.split()) for p in out if p.strip() and p.strip() != "void"]


def parse_declarations(headers=HEADERS):
    decls = []
    seen = set()
    for path in headers:
        with open(path, encoding="utf-8") as f:
            text = _strip_comments(f.read())
        for m in _DECL_RE.finditer(text):
            name = m.group("name")
            if name in seen:
                continue
            seen.add(name)
            decls.append(
                Decl(
                    m.group("ret"),
                    name,
                    _split_params(m.group("params")),
                    bool(m.group("noreturn")),
                )
            )
    return decls
