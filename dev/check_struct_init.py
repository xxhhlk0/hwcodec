#!/usr/bin/env python3
"""Detect struct initializers missing declared fields (rustc E0063)."""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")

STRUCT_DECL = re.compile(r"\bstruct\s+(\w+)\s*(?:<[^<>]*(?:<[^<>]*>)?[^<>]*>)?\s*\{")
IDENT_BRACE = re.compile(r"(\w+)\s*\{")
FIELD = re.compile(r"(?:pub(?:\([^)]*\))?\s+)?(\w+)\s*:(?![:\w])")
SHORTHAND = re.compile(r"(\w+)\s*(?=[,}]|$)")
SKIP_HEAD = re.compile(r"\b(impl|struct|enum|union|trait|fn|for|in)\b|->")


def strip_comments(text):
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == '"':
                    break
                j += 1
            out.append(" " * (j - i + 1))
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            depth = 1
            j = i + 2
            while j < n and depth:
                if text.startswith("/*", j):
                    depth += 1
                    j += 2
                elif text.startswith("*/", j):
                    depth -= 1
                    j += 2
                else:
                    j += 1
            out.append(" " * (j - i))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def match_block(text, brace):
    depth = 0
    i = brace
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def top_level_fields(body):
    fields = []
    spread = False
    depth = 0
    i = 0
    n = len(body)
    while i < n:
        c = body[i]
        if c in "([{":
            depth += 1
            i += 1
            continue
        if c in ")]}":
            if depth == 0:
                break
            depth -= 1
            i += 1
            continue
        if depth == 0:
            if body.startswith("..", i):
                spread = True
                i += 2
                continue
            if c in ",;":
                i += 1
                continue
            m = FIELD.match(body, i)
            if m:
                fields.append(m.group(1))
                i = m.end()
                d = 0
                while i < n:
                    ch = body[i]
                    if ch in "([{":
                        d += 1
                    elif ch in ")]}":
                        if d == 0:
                            break
                        d -= 1
                    elif ch == "," and d == 0:
                        break
                    i += 1
                continue
            m = SHORTHAND.match(body, i)
            if m:
                fields.append(m.group(1))
                i = m.end()
                continue
        i += 1
    return fields, spread


def collect_structs(text):
    structs = {}
    for m in STRUCT_DECL.finditer(text):
        end = match_block(text, m.end() - 1)
        if end < 0:
            continue
        fields, _ = top_level_fields(text[m.end():end])
        structs.setdefault(m.group(1), []).append(fields)
    return structs


def check_file(path, local, candidates, problems):
    raw = open(path, "r", encoding="utf-8", errors="replace").read()
    text = strip_comments(raw)
    for m in IDENT_BRACE.finditer(text):
        name = m.group(1)
        cands = local.get(name) or candidates.get(name)
        if not cands:
            continue
        line_start = text.rfind("\n", 0, m.start()) + 1
        if SKIP_HEAD.search(text[line_start:m.start()]):
            continue
        end = match_block(text, m.end() - 1)
        if end < 0:
            continue
        present, spread = top_level_fields(text[m.end():end])
        if spread:
            continue
        best = None
        for cand in cands:
            missing = [f for f in cand if f not in present]
            if not missing:
                best = None
                break
            if best is None or len(missing) < len(best):
                best = missing
        if best:
            line = raw[: m.start()].count("\n") + 1
            rel = os.path.relpath(path, ROOT).replace("\\", "/")
            problems.append("%s:%d: %s 缺字段 %s" % (rel, line, name, ", ".join(best)))


def main():
    files = []
    for dirpath, _, filenames in os.walk(SRC):
        for fn in filenames:
            if fn.endswith(".rs"):
                files.append(os.path.join(dirpath, fn))
    files.sort()
    local = {}
    candidates = {}
    for path in files:
        text = strip_comments(open(path, "r", encoding="utf-8", errors="replace").read())
        local[path] = collect_structs(text)
        for name, entries in local[path].items():
            candidates.setdefault(name, []).extend(entries)
    problems = []
    for path in files:
        check_file(path, local[path], candidates, problems)
    for p in problems:
        print(p)
    if problems:
        print("\n%d 处初始化缺字段（rustc 会报 E0063）" % len(problems))
        return 1
    print("struct 初始化字段一致（%d 个结构体 / %d 个文件）" % (len(candidates), len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
