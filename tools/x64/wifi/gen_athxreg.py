#!/usr/bin/env python3
"""Extract named declarations from the Linux ath10k headers (ISC) into one header.

  gen_athxreg.py SRCDIR SPEC OUT

SPEC lines:  file  kind  name
  kind: struct | union | enum | define (name is a regex matched against the macro name)
        | text (name is a regex: the first line matching it through the next blank line)
"""
import re
import sys

src, spec, out = sys.argv[1], sys.argv[2], sys.argv[3]
cache = {}


def lines_of(f):
    if f not in cache:
        cache[f] = open("%s/%s" % (src, f), encoding="utf-8").read().split("\n")
    return cache[f]


def block(f, kind, name):
    L = lines_of(f)
    pat = re.compile(r"^(static\s+)?(const\s+)?%s\s+%s\s*(=\s*)?\{" % (kind, re.escape(name)))  # TABLE
    for i, l in enumerate(L):
        if pat.match(l):
            # take a leading comment block too
            j = i
            depth = 0
            res = []
            while j < len(L):
                res.append(L[j])
                depth += L[j].count("{") - L[j].count("}")
                if depth == 0 and "}" in L[j] and ";" in L[j]:
                    break
                j += 1
            return "\n".join(res)
    sys.exit("not found: %s %s %s" % (f, kind, name))


def defines(f, rx):
    L = lines_of(f)
    pat = re.compile(r"^#define\s+(%s)\b" % rx)
    res = []
    i = 0
    found = False
    while i < len(L):
        if pat.match(L[i]):
            found = True
            res.append(L[i])
            while L[i].rstrip().endswith("\\"):
                i += 1
                res.append(L[i])
        i += 1
    if not found:
        sys.exit("no define: %s %s" % (f, rx))
    return "\n".join(res)


parts = []
for ln in open(spec, encoding="utf-8"):
    ln = ln.strip()
    if not ln or ln.startswith("#"):
        if ln.startswith("##"):
            parts.append("\n/* " + ln[2:].strip() + " */")
        continue
    f, kind, name = ln.split(None, 2)
    if kind == "define":
        parts.append(defines(f, name))
    else:
        parts.append(block(f, kind, name))
open(out, "w", encoding="utf-8", newline="\n").write("\n\n".join(parts) + "\n")
print("%s: %d parts" % (out, len(parts)))
