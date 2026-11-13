#!/usr/bin/env python3
"""Fail the build where game structures do not fit the storage the GBA gave them, on arm64.

The game keeps structures in fixed boxes sized for the GBA: a task's 32 bytes
of data (gTasks[].data), a sprite's 16, link buffers, heap blocks of a set
size. A structure that holds pointers grows on a 64-bit build and runs out of
its box, or moves the fields other code reads by halfword - silently, until
something else crashes (the list menus, the Union Room, entering a Pokemon
Center: arm64-port-findings). This finds them in the game's code instead of
waiting to walk into them.

Every game unit is preprocessed as the build does it (clang -E, then preproc)
and parsed by libclang for arm64, whose type sizes it then has (the 32-bit
layout is worked out from the same types). Per unit it gathers:

  overlays  a pointer conversion (a cast, or the implicit one from void *) to
            a type, from the address of some storage;
  copies    memcpy/memset/memmove and the BIOS and DMA copies (CpuSet,
            CpuFastSet, DmaSet: CpuCopy16, DmaCopy32...) of a constant size;
  reads     a task's data read or written by a constant index (data[4], a
            tFoo macro, Get/SetWordTaskArg);
  facts     what storage a pointer parameter, field, global or function result
            can stand for: each call's arguments, each assignment, each return.

Storage is known when it is an array, an element of one, an object, or a heap
block of a constant size; a pointer of unknown extent - a parameter, a field,
a global, a local - stands for everything the facts give it, across all units.
Tasks are told apart by their functions: CreateTask(F), SetTaskFunc...(F),
gTasks[id].func = F join F's to the task, and a helper handed a task's id
works on it too.

Findings:
  overflow    the type or the copy is larger than the storage left.
  drift       a type holding pointers over integer storage (task data, a
              buffer): its fields sit elsewhere than on the GBA. Over a task's
              data it goes in TASK_STATE (task.h) instead.
  stale-read  a task's data read by index where its own structure lay on the
              GBA, now kept apart (TASK_STATE): the read finds nothing there;
              or where a structure's pointers lie on arm64.
  copy32      a copy of a fixed byte count that is a structure's 32-bit size,
              not its arm64 one.
  twin        a pointer to one structure taken as a pointer to another, both
              holding pointers, which line up on 32-bit ARM and not on arm64
              (the game's and the sound mixer's MusicPlayerInfo).

Overflows count only where the 32-bit layout fit: what was already too big
on the GBA is the GBA's (and a different check's) business.

A finding checked to be harmless goes in check_overlays.allow as
"file:function what" with a comment saying why (functions, not lines, so an
edit elsewhere keeps it); everything else fails. --list also prints what could
not be judged (storage nothing says anything about).

  check_overlays.py [--jobs N] [--list] PORT_DIR MAKEFILE STAMP
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

try:
    import clang.cindex as ci
except ImportError:
    sys.exit("check_overlays: needs libclang's Python bindings (Arch: the clang package; "
             "elsewhere: pip install libclang) for the system python3")

TRIPLE = "aarch64-linux-android33"
ALLOW = Path(__file__).with_name("check_overlays.allow")
STORAGE_MARK = "emerald_overlay_storage"  # task.h: TASK_STATE's blocks
K = ci.CursorKind
T = ci.TypeKind
CASTS = (K.CSTYLE_CAST_EXPR, K.UNEXPOSED_EXPR, K.PAREN_EXPR)
ALLOCATORS = {"Alloc", "AllocZeroed", "Alloc_", "AllocZeroed_", "malloc"}
# Calls that name a task by its function: the argument, and whether the call
# gives the task's id back.
TASK_BY_FUNC = {"CreateTask": (0, True), "FindTaskIdByFunc": (0, True), "FuncIsActiveTask": (0, False)}
TASK_ID_NAME = re.compile(r"[Tt]ask(Id|ID|s)?\b|[Tt]ask(Id|ID)")
INTEGERS = (T.CHAR_S, T.CHAR_U, T.SCHAR, T.UCHAR, T.SHORT, T.USHORT, T.INT, T.UINT,
            T.LONG, T.ULONG, T.LONGLONG, T.ULONGLONG, T.BOOL)


# ── Units ───────────────────────────────────────────────────────────────────
def unit_commands(port_dir: Path, makefile: Path) -> dict[str, str]:
    """Each game unit's preprocessing command, as make would run it: up to
    preproc, without the dependency file it writes."""
    # Run from a build too: not as part of it (its jobserver, its level).
    env = {k: v for k, v in os.environ.items() if k not in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEOVERRIDES")}
    objs = subprocess.run(["make", "-f", str(makefile), "--eval", "print-%: ; @echo $($*)", "print-FULL_C_OBJS"],
                          cwd=port_dir, check=True, capture_output=True, text=True, env=env).stdout.split()
    dry = subprocess.run(["make", "-f", str(makefile), "-n", "-B", *objs], cwd=port_dir,
                         capture_output=True, text=True, env=env).stdout
    dry = re.sub(r"\\\n", "", dry)
    commands = {}
    for line in dry.splitlines():
        m = re.search(r" -E -x c (src/\S+\.c)", line)
        if not m:
            # A unit compiled straight, without preproc (full.mk: field_camera.c).
            m = re.search(r" -c (\S*?(src/\S+\.c)) -o build/root/src/\S+\.o$", line)
            if m:
                command = re.sub(r" -MMD| -MP| -MT \S+| -MF \S+", "", line)
                commands[m.group(2)] = command.replace(m.group(0), " -E " + m.group(1))
            continue
        stages = line.split("|")
        command = "|".join(stages[:2])  # clang -E | preproc
        command = re.sub(r" -MMD| -MP| -MT \S+| -MF \S+", "", command)
        commands[m.group(1)] = command
    return commands


# ── libclang helpers ────────────────────────────────────────────────────────
_EVAL_READY = False


def const_int(cursor) -> int | None:
    global _EVAL_READY
    lib = ci.conf.lib
    if not _EVAL_READY:
        lib.clang_Cursor_Evaluate.argtypes = [ci.Cursor]
        lib.clang_Cursor_Evaluate.restype = ctypes.c_void_p
        lib.clang_EvalResult_getKind.argtypes = [ctypes.c_void_p]
        lib.clang_EvalResult_getKind.restype = ctypes.c_int
        lib.clang_EvalResult_getAsLongLong.argtypes = [ctypes.c_void_p]
        lib.clang_EvalResult_getAsLongLong.restype = ctypes.c_longlong
        lib.clang_EvalResult_dispose.argtypes = [ctypes.c_void_p]
        _EVAL_READY = True
    if cursor is None:
        return None
    result = lib.clang_Cursor_Evaluate(cursor)
    if not result:
        return None
    try:
        return lib.clang_EvalResult_getAsLongLong(result) if lib.clang_EvalResult_getKind(result) == 1 else None
    finally:
        lib.clang_EvalResult_dispose(result)


def operands(cursor) -> list:
    """An expression's children without the types it names (TYPE_REF)."""
    return [c for c in cursor.get_children() if c.kind.is_expression()]


def strip(cursor):
    while cursor.kind in CASTS:
        children = operands(cursor)
        if len(children) != 1:
            break
        cursor = children[0]
    return cursor


def first_token(cursor) -> str:
    for token in cursor.get_tokens():
        return token.spelling
    return ""


def text(cursor) -> str:
    return " ".join(t.spelling for t in cursor.get_tokens())[:80]


def mentions_sizeof(cursor) -> bool:
    return any(t.spelling in ("sizeof", "offsetof", "__builtin_offsetof") for t in cursor.get_tokens())


def size64(t) -> int:
    n = t.get_canonical().get_size()
    return n if n > 0 else -1


def unqualified(t) -> str:
    return re.sub(r"\b(const|volatile|restrict)\s+", "", t.get_canonical().spelling)


def is_integer_box(t) -> bool:
    """Plain integer storage, the GBA's kind of box: s16 data[16], u8 buffer[]."""
    t = t.get_canonical()
    while t.kind == T.CONSTANTARRAY:
        t = t.element_type.get_canonical()
    return t.kind in INTEGERS


def pointers64(t, base: int = 0, depth: int = 0) -> list[int]:
    """Byte offsets of the pointers in a type's arm64 layout."""
    t = t.get_canonical()
    if depth > 8:
        return []
    if t.kind == T.POINTER:
        return [base]
    if t.kind == T.CONSTANTARRAY:
        inner = pointers64(t.element_type, 0, depth + 1)
        step = size64(t.element_type)
        return [base + i * step + o for i in range(min(t.element_count, 64)) for o in inner] if inner else []
    if t.kind == T.RECORD:
        out = []
        for field in t.get_fields():
            bits = field.get_field_offsetof()
            if bits >= 0:
                out += pointers64(field.type, base + bits // 8, depth + 1)
        return out
    return []


def layout32(t, depth: int = 0) -> tuple[int, int, list[int]]:
    """A type's size, alignment and pointer offsets on 32-bit ARM."""
    t = t.get_canonical()
    if depth > 8:
        return (max(size64(t), 0), max(t.get_align(), 1), [])
    if t.kind == T.POINTER:
        return (4, 4, [0])
    if t.kind in (T.LONG, T.ULONG):
        return (4, 4, [])
    if t.kind == T.CONSTANTARRAY:
        size, align, inner = layout32(t.element_type, depth + 1)
        return (size * t.element_count, align,
                [i * size + o for i in range(min(t.element_count, 64)) for o in inner])
    if t.kind == T.RECORD:
        if not pointers64(t):
            return (max(size64(t), 0), max(t.get_align(), 1), [])  # nothing that changes
        union = t.get_declaration().kind == K.UNION_DECL
        packed = t.get_align() == 1  # holds a pointer yet aligned to a byte
        bit = 0  # where the next field may start, in bits (bitfields pack)
        size = 0
        align = 1
        ptrs: list[int] = []
        for field in t.get_fields():
            fsize, falign, fptrs = layout32(field.type, depth + 1)
            if packed:
                falign = 1
            if field.is_bitfield():
                width = field.get_bitfield_width()
                unit = max(fsize, 1) * 8
                start = 0 if union else bit
                if width == 0:
                    bit = (start + unit - 1) // unit * unit
                    continue
                if not packed and start // unit != (start + width - 1) // unit:
                    start = (start + unit - 1) // unit * unit  # does not cross its unit
                end = start + width
                bit = end if not union else bit
                size = max(size, (end + 7) // 8)
                align = max(align, falign)
                continue
            at = 0 if union else ((bit + 7) // 8 + falign - 1) // falign * falign
            ptrs += [at + o for o in fptrs]
            if not union:
                bit = (at + fsize) * 8
            size = max(size, at + fsize)
            align = max(align, falign)
        return ((size + align - 1) // align * align, align, ptrs)
    return (max(size64(t), 0), max(t.get_align(), 1), [])


def line_map(source: str) -> list[tuple[str, int]]:
    """Preprocessed line (1-based index) -> (file, line), from the markers."""
    out = [("?", 0)]
    current, number = "?", 0
    for line in source.split("\n"):
        m = re.match(r'#\s*(\d+)\s+"([^"]*)"', line)
        if m:
            out.append((current, number))
            current, number = m.group(2), int(m.group(1))
            continue
        out.append((current, number))
        number += 1
    return out


# ── One unit ────────────────────────────────────────────────────────────────
class Unit:
    """Walks one translation unit and gathers its overlays, copies, task data
    reads and facts, as plain data for the whole program's check."""

    def __init__(self, name: str, source: str):
        self.name = name
        self.lines = line_map(source)
        self.sites: list[dict] = []
        self.reads: list[dict] = []
        self.facts: list[dict] = []
        self.task_facts: list[tuple[str, str]] = []
        self.task_links: list[tuple[str, str]] = []
        self.task_funcs: set[str] = set()
        self.function = ""
        self.fkey = ""
        self.params: dict[str, int] = {}
        self.locals: dict[str, dict | None] = {}
        self.task_vars: dict[str, str | None] = {}

    # Names --------------------------------------------------------------
    def where(self, cursor) -> tuple[str, int]:
        line = cursor.location.line
        return self.lines[line] if line < len(self.lines) else ("?", 0)

    def func_key(self, decl) -> str | None:
        if decl is None or decl.kind != K.FUNCTION_DECL:
            return None
        if decl.linkage == ci.LinkageKind.INTERNAL:
            return "%s::%s" % (self.name, decl.spelling)
        return decl.spelling

    def func_ref(self, cursor) -> str | None:
        cursor = strip(cursor)
        if cursor.kind == K.UNARY_OPERATOR and first_token(cursor) == "&":
            cursor = strip(operands(cursor)[0])
        if cursor.kind == K.DECL_REF_EXPR:
            return self.func_key(cursor.referenced)
        return None

    @staticmethod
    def is_local(decl) -> bool:
        parent = decl.semantic_parent
        return parent is not None and parent.kind == K.FUNCTION_DECL

    # Tasks ----------------------------------------------------------------
    def task_of(self, cursor) -> str | None:
        """The task a task id expression names: task:F, or param:G:i."""
        cursor = strip(cursor)
        if cursor.kind == K.BINARY_OPERATOR and cursor.binary_operator == ci.BinaryOperator.Add:
            left, right = operands(cursor)
            if const_int(right) == 0:  # TASK_STATE's taskId + 0 * sizeof(...)
                return self.task_of(left)
        if cursor.kind == K.DECL_REF_EXPR:
            decl = cursor.referenced
            if decl is None:
                return None
            if decl.kind == K.PARM_DECL and decl.get_usr() in self.params:
                return "param:%s:%d" % (self.fkey, self.params[decl.get_usr()])
            if decl.kind == K.VAR_DECL and self.is_local(decl):
                return self.task_vars.get(decl.get_usr())
            if decl.kind == K.VAR_DECL and TASK_ID_NAME.search(decl.spelling):
                return "global:" + decl.get_usr()
        # A task's id kept in a field or global named for it (taskId, sTaskIds[],
        # listTaskId); not in a general purpose array (a sprite's data), which
        # would make every task whose id ever went there one.
        if cursor.kind == K.MEMBER_REF_EXPR and cursor.type.get_canonical().kind in INTEGERS \
                and TASK_ID_NAME.search(cursor.spelling):
            key = self.field_key(cursor)
            return "field:" + key if key else None
        if cursor.kind == K.ARRAY_SUBSCRIPT_EXPR and cursor.type.get_canonical().kind in INTEGERS:
            # sTaskIds[i]: an array of ids is one place, whatever the index.
            base = strip(operands(cursor)[0])
            if base.kind in (K.DECL_REF_EXPR, K.MEMBER_REF_EXPR) and base.referenced is not None \
                    and TASK_ID_NAME.search(base.spelling):
                return ("field:" + self.field_key(base)) if base.kind == K.MEMBER_REF_EXPR and self.field_key(base) \
                    else "global:" + base.referenced.get_usr()
        if cursor.kind == K.CALL_EXPR and cursor.spelling in TASK_BY_FUNC:
            arg, gives_id = TASK_BY_FUNC[cursor.spelling]
            args = list(cursor.get_arguments())
            if gives_id and len(args) > arg:
                func = self.func_ref(args[arg])
                return "task:" + func if func else None
        return None

    def task_box(self, cursor) -> str | None:
        """For gTasks[id].data (a MEMBER_REF named data), the task."""
        if cursor.kind != K.MEMBER_REF_EXPR or cursor.spelling != "data":
            return None
        inner = operands(cursor)
        if not inner:
            return None
        subscript = strip(inner[0])
        if subscript.kind != K.ARRAY_SUBSCRIPT_EXPR:
            return None
        base, index = operands(subscript)[:2]
        if strip(base).spelling != "gTasks":
            return None
        return self.task_of(index) or "unknown"

    # Storage ------------------------------------------------------------
    @staticmethod
    def root_decl(cursor):
        while True:
            cursor = strip(cursor)
            if cursor.kind == K.ARRAY_SUBSCRIPT_EXPR:
                cursor = operands(cursor)[0]
                continue
            return cursor.referenced if cursor.kind in (K.DECL_REF_EXPR, K.MEMBER_REF_EXPR) else None

    def marked(self, cursor) -> bool:
        decl = self.root_decl(cursor)
        if decl is None:
            return False
        return any(c.kind == K.ANNOTATE_ATTR and c.spelling == STORAGE_MARK for c in decl.get_children())

    def object(self, cursor, offset: int) -> dict | None:
        """Concrete storage: the object cursor designates, from offset on."""
        size = size64(cursor.type)
        if size <= 0:
            return None
        size32 = layout32(cursor.type)[0]
        desc = {"bytes": size - offset, "bytes32": size32 - offset * size32 // size, "box": is_integer_box(cursor.type), "text": text(cursor),
                "marked": self.marked(cursor), "off": offset, "type": cursor.type.get_canonical().spelling}
        task = self.task_box(cursor)
        if task is None and desc["marked"]:
            sub = strip(cursor)
            if sub.kind == K.ARRAY_SUBSCRIPT_EXPR:
                task = self.task_of(operands(sub)[1])
        if task is not None:
            desc["task"] = task
        return desc

    @staticmethod
    def shift(desc: dict | None, by: int) -> dict | None:
        if desc is None:
            return None
        desc = dict(desc)
        if "sym" in desc:
            desc["soff"] = desc.get("soff", 0) + by
        else:
            ratio = desc["bytes32"] / desc["bytes"] if desc.get("bytes32") and desc["bytes"] > 0 else 1
            desc["bytes"] -= by
            desc["bytes32"] = desc.get("bytes32", desc["bytes"] + by) - int(by * ratio)
            desc["off"] += by
        return desc

    def storage(self, cursor) -> dict | None:
        """What the pointer value cursor yields points into: a concrete
        object, a symbol (param, field, global, function result) or None."""
        cursor = strip(cursor)
        t = cursor.type.get_canonical()
        kind = cursor.kind
        if t.kind == T.CONSTANTARRAY and kind in (K.DECL_REF_EXPR, K.MEMBER_REF_EXPR, K.ARRAY_SUBSCRIPT_EXPR):
            if kind == K.ARRAY_SUBSCRIPT_EXPR:
                return self.lvalue(cursor)  # an element that is itself an array, decaying
            return self.object(cursor, 0)
        if kind == K.UNARY_OPERATOR and first_token(cursor) == "&":
            return self.lvalue(operands(cursor)[0])
        if kind == K.BINARY_OPERATOR and cursor.binary_operator in (ci.BinaryOperator.Add, ci.BinaryOperator.Sub):
            left, right = operands(cursor)
            left_t = strip(left).type.get_canonical()
            if left_t.kind in (T.POINTER, T.CONSTANTARRAY):
                step = size64(left_t.get_pointee() if left_t.kind == T.POINTER else left_t.element_type)
                by = const_int(right)
                if by is None or step <= 0:
                    return None
                if cursor.binary_operator == ci.BinaryOperator.Sub:
                    by = -by
                return self.shift(self.storage(left), by * step)
            return None
        if kind == K.CALL_EXPR:
            if cursor.spelling in ALLOCATORS:
                args = list(cursor.get_arguments())
                size = const_int(args[0]) if args else None
                if size is None:
                    return None
                return {"bytes": size, "bytes32": self.size32_of(args[0], size), "box": False,
                        "text": text(cursor), "marked": False, "off": 0, "type": "heap"}
            func = self.func_key(cursor.referenced)
            return {"sym": "ret:" + func} if func and t.kind == T.POINTER else None
        if t.kind != T.POINTER:
            return None
        if kind == K.DECL_REF_EXPR:
            decl = cursor.referenced
            if decl is None:
                return None
            if decl.kind == K.PARM_DECL and decl.get_usr() in self.params:
                return {"sym": "param:%s:%d" % (self.fkey, self.params[decl.get_usr()])}
            if decl.kind == K.VAR_DECL:
                if self.is_local(decl):
                    return self.locals.get(decl.get_usr())
                return {"sym": "global:" + decl.get_usr()}
            return None
        if kind == K.MEMBER_REF_EXPR:
            key = self.field_key(cursor)
            return {"sym": "field:" + key} if key else None
        return None

    def lvalue(self, cursor) -> dict | None:
        """Storage at the object an lvalue designates (for &lvalue)."""
        cursor = strip(cursor)
        if cursor.kind == K.ARRAY_SUBSCRIPT_EXPR:
            base, index = operands(cursor)[:2]
            base_s = strip(base)
            base_t = base_s.type.get_canonical()
            value = const_int(index)
            if base_t.kind == T.CONSTANTARRAY:
                step = size64(base_t.element_type)
                if value is None:
                    # Some element, how far in unknown - except TASK_STATE's
                    # gTaskState[taskId]: that task's whole block.
                    if base_s.kind == K.ARRAY_SUBSCRIPT_EXPR or not self.marked(base_s):
                        return None
                    desc = self.object(base_s, 0)
                    if desc is None:
                        return None
                    desc = dict(desc, bytes=step, bytes32=step, off=0, text=text(cursor))
                    task = self.task_of(index)
                    if task:
                        desc["task"] = task
                    return desc
                if base_s.kind == K.ARRAY_SUBSCRIPT_EXPR:
                    return self.shift(self.lvalue(base_s), value * step)
                return self.object(base_s, value * step)
            if base_t.kind == T.POINTER and value is not None:
                return self.shift(self.storage(base), value * size64(base_t.get_pointee()))
            return None
        if cursor.kind in (K.DECL_REF_EXPR, K.MEMBER_REF_EXPR):
            return self.object(cursor, 0)
        if cursor.kind == K.UNARY_OPERATOR and first_token(cursor) == "*":
            return self.storage(operands(cursor)[0])
        return None

    @staticmethod
    def size32_of(expression, size: int) -> int:
        """A byte count's value on 32-bit ARM: scaled by the type it takes
        the size of (n * sizeof(struct X)), when there is one."""
        types = [c.type for c in expression.walk_preorder() if c.kind == K.TYPE_REF]
        if len(types) != 1:
            return size
        s64, s32 = size64(types[0]), layout32(types[0])[0]
        return size * s32 // s64 if s64 > 0 else size

    @staticmethod
    def field_key(cursor) -> str | None:
        decl = cursor.referenced
        if decl is None or decl.kind != K.FIELD_DECL:
            return None
        parent = decl.semantic_parent
        return "%s.%s" % (parent.type.get_canonical().spelling if parent else "?", decl.spelling)

    def target(self, cursor) -> str | None:
        """The symbol an assignment to cursor gives a fact about."""
        cursor = strip(cursor)
        if cursor.type.get_canonical().kind != T.POINTER:
            return None
        if cursor.kind == K.MEMBER_REF_EXPR:
            key = self.field_key(cursor)
            return "field:" + key if key else None
        if cursor.kind == K.DECL_REF_EXPR and cursor.referenced is not None \
                and cursor.referenced.kind == K.VAR_DECL and not self.is_local(cursor.referenced):
            return "global:" + cursor.referenced.get_usr()
        return None

    # Records --------------------------------------------------------------
    def type_info(self, t) -> dict:
        t = t.get_canonical()
        size32, _, ptrs32 = layout32(t)
        ptrs = pointers64(t)
        return {"type": t.spelling, "need": size64(t), "size32": size32,
                "first": min(ptrs + ptrs32) if ptrs else None,
                "pointer_array": t.kind == T.POINTER}

    def add_site(self, cursor, kind: str, desc: dict | None, info: dict, extra: dict | None = None):
        file, line = self.where(cursor)
        site = dict(info, kind=kind, desc=desc, file=file, line=line, function=self.function, unit=self.name,
                    expr=text(cursor)[:60])
        if extra:
            site.update(extra)
        self.sites.append(site)

    def add_read(self, cursor, desc: dict | None, start: int, size: int):
        if desc is None:
            return
        file, line = self.where(cursor)
        self.reads.append({"desc": desc, "start": start, "size": size, "file": file, "line": line,
                           "function": self.function, "expr": text(cursor)[:60]})

    def fact(self, key: str, desc: dict | None, cursor):
        if desc is not None:
            file, line = self.where(cursor)
            self.facts.append({"key": key, "desc": desc, "file": file, "line": line})

    # The walk -------------------------------------------------------------
    def walk(self, cursor):
        kind = cursor.kind
        if kind == K.FUNCTION_DECL and cursor.is_definition():
            saved = (self.function, self.fkey, self.params, self.locals, self.task_vars)
            self.function, self.fkey = cursor.spelling, self.func_key(cursor)
            self.params = {p.get_usr(): i for i, p in enumerate(cursor.get_arguments())}
            # Shaped like a task's function, void F(u8): whether CreateTask
            # names it here or a script does (the battle animations'
            # createvisualtask), it runs as a task.
            params = list(cursor.get_arguments())
            if cursor.result_type.get_canonical().kind == T.VOID and len(params) == 1 \
                    and params[0].type.get_canonical().kind == T.UCHAR and self.fkey:
                self.task_funcs.add(self.fkey)
            self.locals, self.task_vars = {}, {}
            for child in cursor.get_children():
                self.walk(child)
            self.function, self.fkey, self.params, self.locals, self.task_vars = saved
            return
        if kind == K.VAR_DECL:
            init = [c for c in cursor.get_children() if c.kind.is_expression()]
            if init:
                self.assign(cursor, init[-1], declared=True)
        elif kind == K.BINARY_OPERATOR and cursor.binary_operator == ci.BinaryOperator.Assign:
            left, right = operands(cursor)[:2]
            self.assign(left, right)
        elif kind == K.RETURN_STMT and self.fkey:
            values = operands(cursor)
            if values and values[0].type.get_canonical().kind == T.POINTER:
                self.fact("ret:" + self.fkey, self.storage(values[0]), cursor)
        elif kind == K.CALL_EXPR:
            self.call(cursor)
        elif kind == K.ARRAY_SUBSCRIPT_EXPR:
            self.subscript(cursor)
        elif kind in (K.CSTYLE_CAST_EXPR, K.UNEXPOSED_EXPR):
            self.conversion(cursor)
        for child in cursor.get_children():
            self.walk(child)

    def assign(self, left, right, declared: bool = False):
        if declared:
            decl = left
            if not self.is_local(decl):
                if decl.type.get_canonical().kind == T.POINTER:
                    self.fact("global:" + decl.get_usr(), self.storage(right), right)
                elif decl.type.get_canonical().kind in INTEGERS:
                    task = self.task_of(right)
                    if task:
                        self.task_facts.append(("global:" + decl.get_usr(), task))
                return
        else:
            left = strip(left)
            decl = left.referenced if left.kind == K.DECL_REF_EXPR else None
            if decl is not None and decl.kind == K.VAR_DECL and not self.is_local(decl):
                decl = None
            if decl is None:
                key = self.target(left)
                if key:
                    self.fact(key, self.storage(right), right)
                elif left.type.get_canonical().kind in INTEGERS:
                    # A task's id kept in a field, a global or an array of them.
                    place, task = self.task_of(left), self.task_of(right)
                    if place and task and not place.startswith(("task:", "param:")):
                        self.task_facts.append((place, task))
                if left.kind == K.MEMBER_REF_EXPR and left.spelling == "func":
                    # gTasks[id].func = F: F runs as that task.
                    inner = strip(operands(left)[0]) if operands(left) else None
                    func = self.func_ref(right)
                    if inner is not None and inner.kind == K.ARRAY_SUBSCRIPT_EXPR and func \
                            and strip(operands(inner)[0]).spelling == "gTasks":
                        task = self.task_of(operands(inner)[1])
                        self.task_funcs.add(func)
                        if task:
                            self.task_links.append((task, "task:" + func))
                return
        t = decl.type.get_canonical()
        if t.kind == T.POINTER:
            self.locals[decl.get_usr()] = self.storage(right)
        elif t.kind in INTEGERS:
            self.task_vars[decl.get_usr()] = self.task_of(right)

    def call(self, cursor):
        name = cursor.spelling
        args = list(cursor.get_arguments())
        callee = self.func_key(cursor.referenced)
        # A task named by its function, or given one.
        if name in TASK_BY_FUNC and args:
            func = self.func_ref(args[TASK_BY_FUNC[name][0]])
            if func:
                self.task_funcs.add(func)
        if name in ("SetTaskFuncWithFollowupFunc", "SetTaskFunc") and args:
            task = self.task_of(args[0])
            for arg in args[1:3]:
                func = self.func_ref(arg)
                if func:
                    self.task_funcs.add(func)
                    if task:
                        self.task_links.append((task, "task:" + func))
        if name in ("SetWordTaskArg", "GetWordTaskArg") and len(args) >= 2:
            task, index = self.task_of(args[0]), const_int(args[1])
            if index is not None:
                self.add_read(cursor, {"bytes": 32, "bytes32": 32, "box": True, "text": "gTasks[].data", "marked": False,
                                       "off": 0, "type": "s16[16]", "task": task or "unknown"}, index * 2, 4)
        # What each argument hands the callee: its storage, or its task.
        if callee:
            for i, arg in enumerate(args):
                arg_t = arg.type.get_canonical()
                if arg_t.kind in (T.POINTER, T.CONSTANTARRAY):
                    self.fact("param:%s:%d" % (callee, i), self.storage(arg), arg)
                elif arg_t.kind in INTEGERS:
                    task = self.task_of(arg)
                    if task:
                        self.task_facts.append(("param:%s:%d" % (callee, i), task))
        self.copy(cursor, name, args)

    def copy(self, cursor, name: str, args: list):
        sides = []  # (argument, is the destination)
        size = None
        if name in ("memcpy", "memmove") and len(args) == 3:
            sides, size = [(args[0], True), (args[1], False)], const_int(args[2])
            size_arg = args[2]
        elif name == "memset" and len(args) == 3:
            sides, size = [(args[0], True)], const_int(args[2])
            size_arg = args[2]
        elif name in ("CpuSet", "CpuFastSet") and len(args) == 3:
            control = const_int(args[2])
            if control is None:
                return
            count = control & 0x1FFFFF
            unit = 4 if name == "CpuFastSet" or control & (1 << 26) else 2
            size, size_arg = count * unit, args[2]
            sides = [(args[1], True)] + ([] if control & (1 << 24) else [(args[0], False)])
        elif name == "DmaSet" and len(args) == 4:
            control = const_int(args[3])
            if control is None:
                return
            flags = (control >> 16) & 0xFFFF
            size, size_arg = (control & 0xFFFF) * (4 if flags & 0x0400 else 2), args[3]
            sides = ([] if flags & 0x0040 else [(args[2], True)]) + ([] if flags & 0x0100 else [(args[1], False)])
        if size is None or size <= 0:
            return
        fixed = not mentions_sizeof(size_arg)
        for arg, is_dest in sides:
            desc = self.storage(arg)
            # What it copies, as the argument's own type says (before void *).
            pointee = None
            inner = strip(arg)
            inner_t = inner.type.get_canonical()
            if inner_t.kind == T.POINTER:
                pointee = inner_t.get_pointee()
            elif inner_t.kind == T.CONSTANTARRAY:
                pointee = inner_t.element_type
            info = {"type": "%d bytes (%s)" % (size, name), "need": size, "size32": size, "first": None,
                    "pointer_array": False}
            extra = {"side": "to" if is_dest else "from", "fixed": fixed}
            if pointee is not None and pointee.get_canonical().kind == T.RECORD and fixed:
                p_info = self.type_info(pointee)
                if p_info["first"] is not None and size == p_info["size32"] != p_info["need"]:
                    extra["copy32"] = p_info["type"]
            self.add_site(cursor, "copy", desc, info, extra)

    def subscript(self, cursor):
        """data[k] on a task's data, or on a pointer into it."""
        children = operands(cursor)
        if len(children) < 2:
            return
        base, index = children[:2]
        value = const_int(index)
        if value is None:
            return
        base_s = strip(base)
        base_t = base_s.type.get_canonical()
        if base_t.kind == T.CONSTANTARRAY:
            step = size64(base_t.element_type)
            task = self.task_box(base_s)
            if task is not None:
                desc = self.object(base_s, 0)
                self.add_read(cursor, desc, value * step, step)
        elif base_t.kind == T.POINTER:
            pointee = base_t.get_pointee().get_canonical()
            if pointee.kind not in INTEGERS:
                return
            step = size64(pointee)
            desc = self.storage(base)
            if desc is not None and ("sym" in desc or "task" in desc):
                self.add_read(cursor, desc, value * step, step)

    def conversion(self, cursor):
        """A pointer converted to a pointer to something else: an overlay."""
        t = cursor.type.get_canonical()
        children = operands(cursor)
        if t.kind != T.POINTER or len(children) != 1:
            return
        pointee = t.get_pointee().get_canonical()
        if pointee.kind == T.VOID or children[0].type.get_canonical() == t:
            return
        source_t = children[0].type.get_canonical()
        if source_t.kind not in (T.POINTER, T.CONSTANTARRAY):
            return
        source_pointee = source_t.get_pointee() if source_t.kind == T.POINTER else source_t.element_type
        if unqualified(source_pointee) == unqualified(pointee):
            return  # only const or volatile added or taken: the same thing
        info = self.type_info(pointee)
        if info["need"] <= 0:
            return
        self.twins(cursor, source_pointee, pointee)
        desc = self.storage(children[0])
        # How many pointers an overlay of pointers is used for: list[1] -> 2.
        self.add_site(cursor, "cast", desc, info)


    def twins(self, cursor, source, target):
        """Two structures standing for the same memory (MusicPlayerInfo and
        MP2KPlayerState): their pointers must stay where the other's are on
        arm64 if they did on 32-bit ARM."""
        source, target = source.get_canonical(), target.get_canonical()
        if source.kind != T.RECORD or target.kind != T.RECORD:
            return
        p64s, p64t = pointers64(source), pointers64(target)
        if not p64s and not p64t:
            return
        size64s, size64t = size64(source), size64(target)
        size32s, _, p32s = layout32(source)
        size32t, _, p32t = layout32(target)
        if size64s <= 0 or size64t <= 0:
            return
        common32, common64 = min(size32s, size32t), min(size64s, size64t)
        alike32 = {o for o in p32s if o < common32} == {o for o in p32t if o < common32}
        alike64 = {o for o in p64s if o < common64} == {o for o in p64t if o < common64}
        if alike32 and not alike64:
            file, line = self.where(cursor)
            self.sites.append({"kind": "twin", "type": "%s -> %s" % (source.spelling, target.spelling),
                               "need": 0, "size32": 0, "first": None, "pointer_array": False, "desc": None,
                               "file": file, "line": line, "function": self.function, "unit": self.name,
                               "expr": text(cursor)[:60]})


def check_unit(job: tuple[str, str, str]) -> dict:
    sys.setrecursionlimit(20000)
    unit, root, command = job
    source = subprocess.run(command, shell=True, cwd=root, check=True, capture_output=True, text=True,
                            errors="replace").stdout
    tu = ci.Index.create().parse("unit.i", args=["-target", TRIPLE, "-x", "c", "-std=gnu99", "-fsigned-char", "-w"],
                                 unsaved_files=[("unit.i", source)], options=0)
    walker = Unit(unit, source)
    for cursor in tu.cursor.get_children():
        if cursor.location.file is not None:
            walker.walk(cursor)
    return {"sites": walker.sites, "reads": walker.reads, "facts": walker.facts, "task_facts": walker.task_facts,
            "task_links": walker.task_links, "task_funcs": sorted(walker.task_funcs)}


# ── The whole program ───────────────────────────────────────────────────────
class Program:
    def __init__(self, results: list[dict]):
        self.facts: dict[str, list[dict]] = {}
        self.task_facts: dict[str, set[str]] = {}
        self.sites, self.reads = [], []
        links, funcs = [], set()
        for result in results:
            for fact in result["facts"]:
                self.facts.setdefault(fact["key"], []).append(fact)
            for key, task in result["task_facts"]:
                self.task_facts.setdefault(key, set()).add(task)
            self.sites += result["sites"]
            self.reads += result["reads"]
            links += [tuple(link) for link in result["task_links"]]
            funcs |= set(result["task_funcs"])
        # A task function's id is its task.
        for func in funcs:
            self.task_facts.setdefault("param:%s:0" % func, set()).add("task:" + func)
        self.parent: dict[str, str] = {}
        self._tasks = None
        for a, b in links:
            for x in self.tasks(a):
                for y in self.tasks(b):
                    self.union(x, y)
        self._resolved: dict[str, list[dict]] = {}
        self._tasks: dict[str, set[str]] | None = None

    def find(self, x: str) -> str:
        while self.parent.get(x, x) != x:
            self.parent[x] = self.parent.get(self.parent[x], self.parent[x])
            x = self.parent[x]
        return x

    def union(self, a: str, b: str):
        a, b = self.find(a), self.find(b)
        if a != b:
            self.parent[max(a, b)] = min(a, b)

    def tasks(self, origin: str | None) -> set[str]:
        """The tasks an origin (task:F, param:G:i, a field or global holding
        ids) can be: all of them at once, the first time, to a fixed point."""
        if origin is None or origin == "unknown":
            return set()
        if origin.startswith("task:"):
            return {origin}
        if self._tasks is None:
            known = {k: {t for t in v if t.startswith("task:")} for k, v in self.task_facts.items()}
            changed = True
            while changed:
                changed = False
                for key, parents in self.task_facts.items():
                    for parent in parents:
                        if not parent.startswith("task:") and parent in known and not known[parent] <= known[key]:
                            known[key] |= known[parent]
                            changed = True
            self._tasks = known
        return self._tasks.get(origin, set())

    def resolve(self, desc: dict | None) -> list[dict]:
        """Every concrete storage a description can stand for, each with the
        place that says so (via)."""
        if desc is None:
            return []
        if "sym" not in desc:
            return [desc]
        shift = desc.get("soff", 0)
        out = []
        for concrete in self._symbol(desc["sym"]):
            if shift:
                concrete = dict(concrete)
                ratio = concrete["bytes32"] / concrete["bytes"] if concrete["bytes"] > 0 else 1
                concrete["bytes"] -= shift
                concrete["bytes32"] -= int(shift * ratio)
                concrete["off"] += shift
            out.append(concrete)
        return out

    def _symbol(self, key: str) -> list[dict]:
        if key in self._resolved:
            return self._resolved[key]
        self._resolved[key] = []  # a cycle stops here
        seen, out = set(), []
        for fact in self.facts.get(key, ()):
            for concrete in self.resolve(fact["desc"]):
                concrete = dict(concrete)
                concrete.setdefault("via", "%s:%d" % (fact["file"], fact["line"]))
                mark = (concrete["text"], concrete["bytes"], concrete["off"], concrete.get("task"))
                if mark not in seen:
                    seen.add(mark)
                    out.append(concrete)
        self._resolved[key] = out
        return out

    def group(self, desc: dict) -> set[str]:
        return {self.find(t) for t in self.tasks(desc.get("task"))}


def short(file: str, root: str) -> str:
    return os.path.relpath(file, root + "/..") if file.startswith("/") else file


def findings(program: Program, root: str) -> tuple[list[dict], list[dict]]:
    """(findings, sites that could not be judged)."""
    out, unjudged = [], []

    def report(kind, site, message):
        out.append({"kind": kind, "file": short(site["file"], root), "line": site["line"],
                    "function": site["function"], "what": site["type"] if kind != "stale-read" else site["expr"],
                    "message": message})

    # Each task group: its structures' footprints in its data.
    footprints: dict[str, list[tuple[int, int, dict, str]]] = {}
    for site in program.sites:
        if site["kind"] == "twin":
            report("twin", site, "%s: their pointers line up on 32-bit ARM, not on arm64" % site["type"])
            continue
        concretes = program.resolve(site["desc"])
        if site["desc"] is not None and not concretes:
            unjudged.append(site)
        for c in concretes:
            via = " (via %s)" % c["via"] if "via" in c else ""
            if site["need"] > c["bytes"] and site["size32"] <= c.get("bytes32", c["bytes"]):
                # Too big on arm64 only: on the GBA and the 32-bit build it fit.
                report("overflow", site, "%s (%d bytes on arm64) %s %s, %d bytes left%s"
                       % (site["type"], site["need"], "into" if site.get("side") == "to" else
                          "from" if site.get("side") == "from" else "over", c["text"], c["bytes"], via))
                continue
            if site["kind"] == "copy":
                if site.get("copy32"):
                    report("copy32", site, "copies %d bytes of %s: its 32-bit size, not its arm64 one"
                           % (site["need"], site["copy32"]))
                continue
            if site["first"] is None:
                continue
            if c["marked"]:
                # TASK_STATE: its own block. Its GBA place in the task's data
                # is empty now; reads there are stale.
                for group in program.group(c):
                    footprints.setdefault(group, []).append((0, site["size32"], site, "moved"))
                continue
            if not c["box"] or c["type"] == site["type"]:
                continue
            report("drift", site, "%s holds pointers, over %s%s%s"
                   % (site["type"], c["text"], via,
                      "; a task's own structure goes in TASK_STATE (task.h)" if "task" in c else ""))
            for group in program.group(c):
                end = c["off"] + (c["bytes"] if site["pointer_array"] else site["need"])
                footprints.setdefault(group, []).append((c["off"] + site["first"], end, site, "pointers"))

    for read in program.reads:
        for c in program.resolve(read["desc"]):
            if "task" not in c:
                continue
            start = c["off"] + read["start"]
            for group in program.group(c):
                for begin, end, site, why in footprints.get(group, ()):
                    if start < end and start + read["size"] > begin:
                        what = ("where %s lay on the GBA (%s:%d), kept apart now" if why == "moved" else
                                "where %s's pointers lie on arm64 (%s:%d)")
                        out.append({"kind": "stale-read", "file": short(read["file"], root), "line": read["line"],
                                    "function": read["function"], "what": read["expr"],
                                    "message": "reads %s at byte %d: %s" % (
                                        read["expr"], start, what % (site["type"], short(site["file"], root),
                                                                     site["line"]))})
    return out, unjudged


def load_allow() -> set[tuple[str, str]]:
    allowed = set()
    if ALLOW.exists():
        for line in ALLOW.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                where, _, what = line.partition(" ")
                allowed.add((where, what.strip()))
    return allowed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("port_dir", type=Path)
    parser.add_argument("makefile", type=Path)
    parser.add_argument("stamp", type=Path)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--list", action="store_true", help="also print allowed and unjudged sites")
    args = parser.parse_args()

    commands = unit_commands(args.port_dir, args.makefile)
    if not commands:
        print("check_overlays: no game units found", file=sys.stderr)
        return 1
    root = str(args.port_dir.resolve())
    jobs = [(unit, root, command) for unit, command in sorted(commands.items())]
    with ProcessPoolExecutor(args.jobs) as pool:
        results = list(pool.map(check_unit, jobs))
    program = Program(results)
    found, unjudged = findings(program, root)

    unique = {}
    for f in found:
        unique.setdefault((f["file"], f["line"], f["kind"], f["what"], f["message"]), f)
    allowed = load_allow()
    failures = 0
    for key, f in sorted(unique.items()):
        ok = ("%s:%s" % (f["file"], f["function"]), f["what"]) in allowed
        if ok and not args.list:
            continue
        failures += not ok
        print("%s%s:%d (%s): %s: %s" % ("allowed " if ok else "", f["file"], f["line"], f["function"],
                                         f["kind"], f["message"]))
    if args.list:
        # What the check could not see: storage nothing describes, and task
        # data reads it could not tie to a task.
        for site in sorted({(short(s["file"], root), s["line"], s["type"], s["desc"]["sym"]) for s in unjudged
                            if s["kind"] == "cast" and s["first"] is not None}):
            print("unjudged %s:%d: %s (holds pointers) over %s, which nothing describes" % site)
        tied, lost, unknown = 0, {}, 0
        for read in program.reads:
            concretes = program.resolve(read["desc"])
            task_data = [c for c in concretes if "task" in c]
            if not concretes:
                unknown += 1
            elif any(program.group(c) for c in task_data):
                tied += 1
            elif task_data:
                lost[short(read["file"], root)] = lost.get(short(read["file"], root), 0) + 1
        print("task data reads: %d tied to a task, %d not (by file: %s); %d reads through pointers nothing "
              "describes" % (tied, sum(lost.values()),
                             ", ".join("%s %d" % kv for kv in sorted(lost.items(), key=lambda kv: -kv[1])[:15]),
                             unknown))
    print("check_overlays: %d units, %d overlays and copies, %d task data reads, %d findings, %d not allowed, "
          "%d unjudged" % (len(jobs), len(program.sites), len(program.reads), len(unique), failures,
                           len({(s["file"], s["line"], s["type"]) for s in unjudged})))
    if failures:
        return 1
    args.stamp.write_text(hashlib.sha1(json.dumps(sorted(map(str, unique))).encode()).hexdigest() + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
