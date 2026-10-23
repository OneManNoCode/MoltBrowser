# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""A small GN interpreter, enough to read targets' parameters.

It runs BUILDCONFIG.gn, the imports and a BUILD.gn file the way gn does, with
the build args of configs/linux-x64.gn, but it does not run templates: every
call with a block (a target, a config, a template invocation) is recorded
with the scope its block produced. Values gn would compute from the outside
world (exec_script, read_file, get_target_outputs) are UNKNOWN; a condition
that is UNKNOWN takes its "then" branch.
"""
import fnmatch
import os
import re
import subprocess
import sys

from .sources import log


class _Unknown:
    def __repr__(self):
        return "UNKNOWN"

    def __bool__(self):
        raise TypeError("UNKNOWN has no truth value")


UNKNOWN = _Unknown()

# The build directory, as in a checkout (sources.OUT_REL).
OUT = "//out/cc"

# Scripts exec_script() really runs (deterministic, local inputs only), with
# the files they import. Everything else gn would run is UNKNOWN.
EXEC_SCRIPTS = {
    "//build/util/version.py": ["//build/util/android_chrome_version.py",
                                "//chrome/VERSION"],
}

# ---------------------------------------------------------------------------
# Lexer

_TOKEN = re.compile(r"""
    (?P<ws>[ \t\r\n]+) |
    (?P<comment>\#[^\n]*) |
    (?P<string>"(?:\\.|[^"\\])*") |
    (?P<number>-?\d+) |
    (?P<ident>[A-Za-z_][A-Za-z0-9_]*) |
    (?P<op>==|!=|<=|>=|&&|\|\||\+=|-=|[=<>!+\-(){}\[\],.])
""", re.X)


def tokenize(text):
    pos = 0
    out = []
    while pos < len(text):
        m = _TOKEN.match(text, pos)
        if not m:
            raise SyntaxError("gn: bad character %r at %d" % (text[pos], pos))
        pos = m.end()
        kind = m.lastgroup
        if kind in ("ws", "comment"):
            continue
        out.append((kind, m.group()))
    out.append(("eof", ""))
    return out


# ---------------------------------------------------------------------------
# Parser -> tuples


class Parser:
    def __init__(self, text):
        self.toks = tokenize(text)
        self.i = 0

    def peek(self, k=0):
        return self.toks[self.i + k]

    def take(self, val=None):
        t = self.toks[self.i]
        if val is not None and t[1] != val:
            raise SyntaxError("gn: expected %r, got %r" % (val, t[1]))
        self.i += 1
        return t

    def parse_file(self):
        stmts = []
        while self.peek()[0] != "eof":
            stmts.append(self.statement())
        return stmts

    def block(self):
        self.take("{")
        stmts = []
        while self.peek()[1] != "}":
            stmts.append(self.statement())
        self.take("}")
        return stmts

    def statement(self):
        t = self.peek()
        if t == ("ident", "if"):
            return self.condition()
        expr = self.expr()
        nt = self.peek()
        if nt[1] in ("=", "+=", "-="):
            self.take()
            return ("assign", expr, nt[1], self.expr())
        if expr[0] == "call":
            if self.peek()[1] == "{":
                return ("call", expr[1], expr[2], self.block())
            return ("call", expr[1], expr[2], None)
        raise SyntaxError("gn: unexpected statement near %r" % (nt,))

    def condition(self):
        self.take("if")
        self.take("(")
        cond = self.expr()
        self.take(")")
        then = self.block()
        els = None
        if self.peek() == ("ident", "else"):
            self.take()
            if self.peek() == ("ident", "if"):
                els = [self.condition()]
            else:
                els = self.block()
        return ("if", cond, then, els)

    _PREC = [("||",), ("&&",), ("==", "!="), ("<", ">", "<=", ">="),
             ("+", "-")]

    def expr(self, level=0):
        if level == len(self._PREC):
            return self.unary()
        left = self.expr(level + 1)
        while self.peek()[0] == "op" and self.peek()[1] in self._PREC[level]:
            op = self.take()[1]
            right = self.expr(level + 1)
            left = ("bin", op, left, right)
        return left

    def unary(self):
        if self.peek()[1] == "!":
            self.take()
            return ("not", self.unary())
        return self.postfix()

    def postfix(self):
        e = self.primary()
        while True:
            t = self.peek()
            if t[1] == ".":
                self.take()
                e = ("member", e, self.take()[1])
            elif t[1] == "[":
                self.take()
                idx = self.expr()
                self.take("]")
                e = ("index", e, idx)
            else:
                return e

    def primary(self):
        kind, val = self.take()
        if kind == "string":
            return ("str", val[1:-1])
        if kind == "number":
            return ("num", int(val))
        if kind == "ident":
            if val in ("true", "false"):
                return ("bool", val == "true")
            if self.peek()[1] == "(":
                self.take("(")
                args = []
                while self.peek()[1] != ")":
                    args.append(self.expr())
                    if self.peek()[1] == ",":
                        self.take()
                self.take(")")
                return ("call", val, args)
            return ("id", val)
        if val == "(":
            e = self.expr()
            self.take(")")
            return e
        if val == "[":
            items = []
            while self.peek()[1] != "]":
                items.append(self.expr())
                if self.peek()[1] == ",":
                    self.take()
            self.take("]")
            return ("list", items)
        if val == "{":
            self.i -= 1
            return ("scope", self.block())
        if val == "-":
            return ("bin", "-", ("num", 0), self.primary())
        raise SyntaxError("gn: unexpected %r" % val)


# ---------------------------------------------------------------------------
# Evaluation


class Scope:
    def __init__(self, parent=None):
        self.parent = parent
        self.vars = {}

    def get(self, name):
        s = self
        while s is not None:
            if name in s.vars:
                return s.vars[name]
            s = s.parent
        raise KeyError(name)

    def has(self, name):
        try:
            self.get(name)
            return True
        except KeyError:
            return False

    def set(self, name, value):
        self.vars[name] = value

    def flatten(self):
        out = {}
        chain = []
        s = self
        while s is not None:
            chain.append(s)
            s = s.parent
        for s in reversed(chain):
            out.update(s.vars)
        return out


class Target:
    def __init__(self, func, name, scope, path):
        self.func = func
        self.name = name
        self.scope = scope
        self.path = path  # the BUILD.gn file's source-absolute dir ("//x/y")

    def get(self, key, default=None):
        return self.scope.vars.get(key, default)

    def __repr__(self):
        return "<%s %s:%s>" % (self.func, self.path, self.name)


# Templates not worth expanding (huge, and handled by dedicated generators).
NO_EXPAND = {"mojom", "mojom_component", "generate_jni",
             "android_library", "java_library", "android_resources",
             "android_apk", "rust_static_library", "fuzzer_test",
             "test", "component", "jumbo_component", "chrome_extension"}

BUILTIN_SKIP = {"assert", "print", "set_defaults", "not_needed",
                "set_sources_assignment_filter", "pool", "toolchain",
                "tool", "set_default_toolchain", "propagates_configs",
                "testonly", "print_stack_trace", "check_includes"}


class Interp:
    """Evaluates GN files rooted at `root_reader(path) -> text or None`."""

    def __init__(self, read, args, globals_=None, root=None):
        self.read = read          # "//a/b.gni" -> str or None
        self.root = root          # filesystem path of "//" (for rebase_path)
        self.args = args          # build args overrides
        self.import_cache = {}
        self.parse_cache = {}
        self.globals = globals_ or Scope()
        self.declared = {}
        self.warnings = []
        self._exec_cache = {}

    # -- files -----------------------------------------------------------
    def parse(self, path):
        if path not in self.parse_cache:
            text = self.read(path)
            if text is None:
                self.parse_cache[path] = None
            else:
                try:
                    self.parse_cache[path] = Parser(text).parse_file()
                except SyntaxError as e:
                    self.warnings.append("%s: %s" % (path, e))
                    self.parse_cache[path] = None
        return self.parse_cache[path]

    def run_buildconfig(self, path="//build/config/BUILDCONFIG.gn"):
        stmts = self.parse(path)
        if stmts is None:
            raise RuntimeError("cannot read " + path)
        ctx = {"dir": "//build/config", "targets": None}
        self.exec_block(stmts, self.globals, ctx)

    def import_file(self, path, into, ctx):
        if path not in self.import_cache:
            stmts = self.parse(path)
            s = Scope(self.globals)
            if stmts is not None:
                c = {"dir": path.rsplit("/", 1)[0], "targets": None,
                     "templates": {}}
                self.exec_block(stmts, s, c)
            else:
                self.warnings.append("import not found: " + path)
            self.import_cache[path] = s
        for k, v in self.import_cache[path].vars.items():
            if not k.startswith("_"):
                into.set(k, v)

    def run_file(self, path, expand=False):
        """Runs a BUILD.gn file; returns [Target]. With expand, template
        invocations also run the template body (recording the targets it
        defines), except for NO_EXPAND templates."""
        stmts = self.parse(path)
        if stmts is None:
            return []
        targets = []
        d = path.rsplit("/", 1)[0]
        ctx = {"dir": d, "targets": targets, "expand": expand}
        s = Scope(self.globals)
        self._set_dirs(s, d)
        self.exec_block(stmts, s, ctx)
        return targets

    def _set_dirs(self, s, d):
        rel = d[2:]
        s.set("target_gen_dir", OUT + "/gen/" + rel if rel else OUT + "/gen")
        s.set("target_out_dir", OUT + "/obj/" + rel if rel else OUT + "/obj")

    # -- statements ------------------------------------------------------
    def exec_block(self, stmts, scope, ctx):
        for st in stmts:
            self.exec_stmt(st, scope, ctx)

    def exec_stmt(self, st, scope, ctx):
        kind = st[0]
        if kind == "assign":
            self.assign(st[1], st[2], self.eval(st[3], scope, ctx), scope)
        elif kind == "if":
            c = self.eval(st[1], scope, ctx)
            if c is UNKNOWN:
                c = True
            if c:
                self.exec_block(st[2], scope, ctx)
            elif st[3] is not None:
                self.exec_block(st[3], scope, ctx)
        elif kind == "call":
            self.call_stmt(st[1], st[2], st[3], scope, ctx)

    def assign(self, lv, op, val, scope):
        if lv[0] == "id":
            name = lv[1]
            if op == "=":
                scope.set(name, val)
                return
            try:
                cur = scope.get(name)
            except KeyError:
                cur = [] if isinstance(val, list) else UNKNOWN
            scope.set(name, self.binop("+" if op == "+=" else "-", cur, val))
        elif lv[0] == "member":
            base = lv[1]
            if base[0] == "id":
                try:
                    target = scope.get(base[1])
                except KeyError:
                    return
                if isinstance(target, Scope):
                    if op == "=":
                        target.set(lv[2], val)
                    else:
                        cur = target.vars.get(lv[2], [])
                        target.set(lv[2], self.binop(
                            "+" if op == "+=" else "-", cur, val))
        # index assignment: ignored

    def call_stmt(self, name, args, block, scope, ctx):
        if name == "import":
            p = self.eval(args[0], scope, ctx)
            if isinstance(p, str):
                self.import_file(self.abspath(p, ctx), scope, ctx)
            return
        if name == "declare_args":
            s = Scope(scope)
            self.exec_block(block or [], s, ctx)
            for k, v in s.vars.items():
                self.declared[k] = v
                scope.set(k, self.args[k] if k in self.args else v)
            return
        if name == "foreach":
            var = args[0][1]
            lst = self.eval(args[1], scope, ctx)
            if isinstance(lst, list):
                for item in lst:
                    scope.set(var, item)
                    self.exec_block(block or [], scope, ctx)
            return
        if name == "forward_variables_from":
            src = self.eval(args[0], scope, ctx)
            what = self.eval(args[1], scope, ctx)
            excl = self.eval(args[2], scope, ctx) if len(args) > 2 else []
            if isinstance(src, Scope):
                items = src.vars
                if what == "*":
                    for k, v in items.items():
                        if not isinstance(excl, list) or k not in excl:
                            scope.set(k, v)
                elif isinstance(what, list):
                    for k in what:
                        if k in items:
                            scope.set(k, items[k])
            return
        if name == "template":
            tn = self.eval(args[0], scope, ctx) if args else None
            if isinstance(tn, str) and block is not None:
                scope.set("tmpl::" + tn, (block, scope))
            return
        if name in BUILTIN_SKIP or block is None:
            if name not in BUILTIN_SKIP and block is None:
                self.eval(("call", name, args), scope, ctx)
            return
        # A target, config or template invocation.
        tname = self.eval(args[0], scope, ctx) if args else ""
        s = Scope(scope)
        s.set("target_name", tname)
        self.exec_block(block, s, ctx)
        if ctx.get("targets") is not None:
            ctx["targets"].append(Target(name, tname, s, ctx["dir"]))
        if ctx.get("expand") and name not in NO_EXPAND:
            try:
                body, defscope = scope.get("tmpl::" + name)
            except KeyError:
                return
            depth = ctx.get("depth", 0)
            if depth > 12:
                return
            ts = Scope(defscope)
            ts.set("invoker", s)
            ts.set("target_name", tname)
            self._set_dirs(ts, ctx["dir"])
            c2 = dict(ctx)
            c2["depth"] = depth + 1
            try:
                self.exec_block(body, ts, c2)
            except (RecursionError, TypeError, AttributeError) as e:
                self.warnings.append("template %s in %s: %s" % (
                    name, ctx["dir"], e))

    # -- expressions -----------------------------------------------------
    def abspath(self, p, ctx):
        if p.startswith("/") and not p.startswith("//"):
            return p
        if p.startswith("//"):
            d, p = "//", p[2:]
        else:
            d = ctx["dir"]
        parts = d[2:].split("/") if d != "//" else []
        parts = [x for x in parts if x]
        for c in p.split("/"):
            if c == "..":
                if parts:
                    parts.pop()
            elif c not in (".", ""):
                parts.append(c)
        return "//" + "/".join(parts)

    def eval(self, e, scope, ctx):
        k = e[0]
        if k == "str":
            return self.interp_str(e[1], scope, ctx)
        if k == "num":
            return e[1]
        if k == "bool":
            return e[1]
        if k == "id":
            try:
                return scope.get(e[1])
            except KeyError:
                return UNKNOWN
        if k == "list":
            return [self.eval(x, scope, ctx) for x in e[1]]
        if k == "scope":
            s = Scope(scope)
            self.exec_block(e[1], s, ctx)
            out = Scope()
            out.vars = dict(s.vars)
            return out
        if k == "not":
            v = self.eval(e[1], scope, ctx)
            return UNKNOWN if v is UNKNOWN else (not v)
        if k == "bin":
            op = e[1]
            if op in ("&&", "||"):
                a = self.eval(e[2], scope, ctx)
                if op == "&&" and a is False:
                    return False
                if op == "||" and a is True:
                    return True
                b = self.eval(e[3], scope, ctx)
                if a is UNKNOWN or b is UNKNOWN:
                    if op == "&&" and b is False:
                        return False
                    if op == "||" and b is True:
                        return True
                    return UNKNOWN
                return (a and b) if op == "&&" else (a or b)
            return self.binop(op, self.eval(e[2], scope, ctx),
                              self.eval(e[3], scope, ctx))
        if k == "member":
            base = self.eval(e[1], scope, ctx)
            if isinstance(base, Scope):
                return base.vars.get(e[2], UNKNOWN)
            return UNKNOWN
        if k == "index":
            base = self.eval(e[1], scope, ctx)
            idx = self.eval(e[2], scope, ctx)
            if isinstance(base, list) and isinstance(idx, int) and \
                    0 <= idx < len(base):
                return base[idx]
            return UNKNOWN
        if k == "call":
            return self.call_expr(e[1], e[2], scope, ctx)
        return UNKNOWN

    def binop(self, op, a, b):
        if a is UNKNOWN or b is UNKNOWN:
            if op == "+" and isinstance(a, list):
                return a
            return UNKNOWN
        try:
            if op == "+":
                if isinstance(a, list) and not isinstance(b, list):
                    return a + [b]
                return a + b
            if op == "-":
                if isinstance(a, list):
                    rm = b if isinstance(b, list) else [b]
                    return [x for x in a if x not in rm]
                return a - b
            if op == "==":
                return a == b
            if op == "!=":
                return a != b
            if op == "<":
                return a < b
            if op == ">":
                return a > b
            if op == "<=":
                return a <= b
            if op == ">=":
                return a >= b
        except TypeError:
            return UNKNOWN
        return UNKNOWN

    _INTERP = re.compile(r"\$(\{([^}]*)\}|0x[0-9A-Fa-f]{2}|[A-Za-z_]\w*)")

    def interp_str(self, s, scope, ctx):
        s = s.replace('\\"', '"').replace("\\\\", "\x00")
        s = s.replace("\\$", "\x01")

        def rep(m):
            body = m.group(2) if m.group(2) is not None else m.group(1)
            if body.startswith("0x"):
                return chr(int(body[2:], 16))
            parts = body.split(".")
            try:
                v = scope.get(parts[0])
            except KeyError:
                return "<UNKNOWN>"
            for p in parts[1:]:
                v = v.vars.get(p, UNKNOWN) if isinstance(v, Scope) else UNKNOWN
            if v is UNKNOWN:
                return "<UNKNOWN>"
            if isinstance(v, bool):
                return "true" if v else "false"
            if isinstance(v, list):
                return "[" + ", ".join('"%s"' % x for x in v) + "]"
            return str(v)

        s = self._INTERP.sub(rep, s)
        return s.replace("\x00", "\\").replace("\x01", "$")

    def call_expr(self, name, args, scope, ctx):
        ev = lambda i: self.eval(args[i], scope, ctx)  # noqa: E731
        if name == "defined":
            a = args[0]
            if a[0] == "id":
                return scope.has(a[1])
            if a[0] == "member":
                base = self.eval(a[1], scope, ctx)
                return isinstance(base, Scope) and a[2] in base.vars
            return UNKNOWN
        if name == "getenv":
            return ""
        if name == "rebase_path":
            v = ev(0)
            new_base = ev(1) if len(args) > 1 else ""
            cur = ev(2) if len(args) > 2 else "."
            if isinstance(v, list):
                return [self._rebase(x, new_base, cur, ctx) for x in v]
            return self._rebase(v, new_base, cur, ctx)
        if name == "get_path_info":
            v, what = ev(0), ev(1)
            if isinstance(v, list):
                return [self._path_info(x, what, ctx) for x in v]
            return self._path_info(v, what, ctx)
        if name == "get_label_info":
            v, what = ev(0), ev(1)
            if not isinstance(v, str):
                return UNKNOWN
            label = v.split("(")[0]
            if ":" in label:
                d, n = label.split(":", 1)
            else:
                d, n = label, label.rsplit("/", 1)[-1]
            d = self.abspath(d, ctx) if d else ctx["dir"]
            if what == "dir":
                return d
            if what == "name":
                return n
            if what == "target_gen_dir":
                return OUT + "/gen/" + d[2:]
            if what == "target_out_dir":
                return OUT + "/obj/" + d[2:]
            if what == "root_gen_dir":
                return OUT + "/gen"
            if what == "label_no_toolchain":
                return d + ":" + n
            return UNKNOWN
        if name in ("filter_include", "filter_exclude"):
            lst, pats = ev(0), ev(1)
            if not isinstance(lst, list) or not isinstance(pats, list):
                return UNKNOWN
            m = [x for x in lst if isinstance(x, str) and
                 any(fnmatch.fnmatch(x, p) for p in pats)]
            return m if name == "filter_include" else \
                [x for x in lst if x not in m]
        if name == "string_join":
            sep, lst = ev(0), ev(1)
            if isinstance(lst, list) and all(isinstance(x, str) for x in lst):
                return sep.join(lst)
            return UNKNOWN
        if name == "string_split":
            v = ev(0)
            sep = ev(1) if len(args) > 1 else " "
            return v.split(sep) if isinstance(v, str) else UNKNOWN
        if name == "string_replace":
            v, a, b = ev(0), ev(1), ev(2)
            if all(isinstance(x, str) for x in (v, a, b)):
                return v.replace(a, b)
            return UNKNOWN
        if name == "exec_script":
            return self._exec_script([ev(i) for i in range(len(args))], ctx)
        if name == "label_matches":
            return UNKNOWN
        if name == "split_list":
            return UNKNOWN
        return UNKNOWN

    def _exec_script(self, a, ctx):
        """exec_script() for the scripts in EXEC_SCRIPTS, run as gn would
        (from the build directory); every other script is UNKNOWN."""
        if not a or not isinstance(a[0], str) or self.root is None:
            return UNKNOWN
        script = self.abspath(a[0], ctx)
        if script not in EXEC_SCRIPTS:
            return UNKNOWN
        sargs = a[1] if len(a) > 1 and isinstance(a[1], list) else []
        if any(not isinstance(x, str) for x in sargs):
            return UNKNOWN
        conv = a[2] if len(a) > 2 and isinstance(a[2], str) else ""
        key = (script, tuple(sargs), conv)
        if key in self._exec_cache:
            return self._exec_cache[key]
        deps = [script] + EXEC_SCRIPTS[script]
        deps += [x for x in (a[3] if len(a) > 3 and isinstance(a[3], list)
                             else []) if isinstance(x, str)]
        for d in deps:
            self.read(self.abspath(d, ctx))  # materializes it
        out_dir = os.path.join(self.root, OUT[2:])
        os.makedirs(out_dir, exist_ok=True)
        r = subprocess.run([sys.executable, "-E", "-s",
                            os.path.join(self.root, script[2:])] + sargs,
                           cwd=out_dir, capture_output=True, text=True)
        if r.returncode != 0:
            self.warnings.append("exec_script %s failed: %s" % (
                script, r.stderr[-300:]))
            val = UNKNOWN
        elif conv == "scope":
            sc = Scope()
            try:
                self.exec_block(Parser(r.stdout).parse_file(), sc,
                                {"dir": "//", "targets": None})
                val = sc
            except SyntaxError:
                val = UNKNOWN
        elif conv in ("value", "json"):
            try:
                val = self.eval(Parser(r.stdout).expr(), Scope(),
                                {"dir": "//"}) if conv == "value" else \
                    __import__("json").loads(r.stdout)
            except Exception:  # noqa: BLE001
                val = UNKNOWN
        elif conv == "list lines":
            val = r.stdout.splitlines()
        elif conv in ("trim string", "string"):
            val = r.stdout.strip() if conv == "trim string" else r.stdout
        else:
            val = UNKNOWN
        self._exec_cache[key] = val
        return val

    def _rebase(self, v, new_base, cur, ctx):
        if not isinstance(v, str) or not isinstance(new_base, str) or \
                not isinstance(cur, str):
            return UNKNOWN
        if v.startswith("/") and not v.startswith("//"):
            return v
        c = self.abspath(cur, ctx) if cur != "." else ctx["dir"]
        a = self.abspath(v, {"dir": c})
        if new_base == "":
            if self.root is None:
                return a
            return os.path.normpath(os.path.join(self.root, a[2:]))
        nb = self.abspath(new_base, ctx)
        rel = os.path.relpath("/" + a[2:], "/" + nb[2:])
        return rel

    def _path_info(self, v, what, ctx):
        if not isinstance(v, str):
            return UNKNOWN
        base = v.rsplit("/", 1)[-1]
        if what == "file":
            return base
        if what == "name":
            return base.rsplit(".", 1)[0] if "." in base else base
        if what == "extension":
            return base.rsplit(".", 1)[1] if "." in base else ""
        if what == "dir":
            return v.rsplit("/", 1)[0] if "/" in v else "."
        if what == "abspath":
            return self.abspath(v, ctx)
        if what == "gen_dir":
            d = self.abspath(v, ctx).rsplit("/", 1)[0]
            return OUT + "/gen/" + d[2:]
        if what == "out_dir":
            d = self.abspath(v, ctx).rsplit("/", 1)[0]
            return OUT + "/obj/" + d[2:]
        return UNKNOWN


def parse_args_file(text):
    """configs/*.gn -> {name: value} (plain assignments only)."""
    out = {}
    stmts = Parser(text).parse_file()
    it = Interp(lambda p: None, {})
    for st in stmts:
        if st[0] == "assign" and st[1][0] == "id":
            out[st[1][1]] = it.eval(st[3], Scope(), {"dir": "//"})
    return out


# Files gn would find but this tree lacks: gclient writes the first one, the
# others live in DEPS repos without a usable GitHub mirror. Only the
# declare_args() that matter for the Linux build.
VIRTUAL_FILES = {
    "//build/config/gclient_args.gni": """
build_with_chromium = true
checkout_android = false
checkout_android_prebuilts_build_tools = false
checkout_android_native_support = false
checkout_ios_webkit = false
checkout_nacl = false
checkout_openxr = false
checkout_src_internal = false
checkout_google_benchmark = false
checkout_clang_coverage_tools = false
generate_location_tags = true
""",
    "//third_party/icu/config.gni": """
declare_args() {
  icu_use_data_file = true
  icu_disable_thin_archive = false
  icu_use_stub_data = false
}
""",
    "//third_party/webrtc/webrtc.gni": """
declare_args() {
  rtc_use_h265 = false
  rtc_use_h264 = false
  rtc_enable_protobuf = true
}
""",
    "//third_party/dawn/scripts/dawn_features.gni": """
declare_args() {
  dawn_enable_opengles = true
  dawn_enable_vulkan = true
  dawn_enable_desktop_gl = true
  dawn_use_swiftshader = true
  dawn_enable_d3d11 = false
  dawn_enable_d3d12 = false
  dawn_enable_metal = false
  dawn_enable_null = true
  dawn_use_x11 = true
  dawn_use_wayland = true
  dawn_supports_glfw_for_windowing = false
}
""",
}


def make_interp(tree, args_file_text, extra_args=None):
    """An Interp over the Chromium tree with BUILDCONFIG.gn already run."""

    def read(path):
        if path in VIRTUAL_FILES:
            return VIRTUAL_FILES[path]
        if not path.startswith("//"):
            return None
        data = tree.read(path[2:])
        return data.decode("utf-8", "replace") if data is not None else None

    args = {}
    # //.gn default_args
    dot = read("//.gn")
    if dot:
        try:
            stmts = Parser(dot).parse_file()
            it0 = Interp(read, {})
            s = Scope()
            it0.exec_block(stmts, s, {"dir": "//", "targets": None})
            da = s.vars.get("default_args")
            if isinstance(da, Scope):
                args.update(da.vars)
        except SyntaxError as e:
            log("gn: .gn: %s" % e)
    args.update(parse_args_file(args_file_text))
    args.update(extra_args or {})
    g = Scope()
    for k, v in {"host_os": "linux", "host_cpu": "x64", "target_os": "linux",
                 "target_cpu": "x64", "current_os": "linux",
                 "current_cpu": "x64", "root_build_dir": OUT,
                 "root_gen_dir": OUT + "/gen", "root_out_dir": OUT,
                 "python_path": "python3", "gn_version": 2300,
                 "current_toolchain": "//build/toolchain/linux:clang_x64",
                 "default_toolchain": "//build/toolchain/linux:clang_x64",
                 "host_toolchain": "//build/toolchain/linux:clang_x64",
                 }.items():
        g.set(k, v)
    it = Interp(read, args, g, root=str(tree.tree_dir))
    it.run_buildconfig()
    return it
