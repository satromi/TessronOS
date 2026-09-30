#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_bt2.py -- 設計書が実身化の原則(設計書 18.19)を守っているかの検査

    check_bt2.py [設計書のディレクトリ]

次を確かめ、違反があれば一覧を出して 1 で終わる。

- 「実身化の確認」の表(見出し行が 資源 | 実身の種類とマネージャ | … の 9 列)は、
  どの行も 9 列すべてが埋まっている。18.19 の書式の見本(行の無い表)は除く。
- 段階の表(18.18)の 8f 以降の行は「実身化: 節番号」を含み、その節に確認の表がある。
"""
import os
import re
import sys

COLUMNS = ['資源', '実身の種類とマネージャ', '名前とUUID', '操作', '保護', '事象',
           '関係(仮身)', '定義', '実身にしないものと理由']
HEADING = re.compile(r'^(#{2,4})\s+([0-9A-Z]+(?:\.[0-9]+)*)\s')
STAGE_ROW = re.compile(r'^\|\s*([0-9]+[a-z]?(?:-[0-9]+)?)\s*\|')
REF = re.compile(r'実身化:\s*([0-9]+(?:\.[0-9]+)+)')


def cells(line):
    return [c.strip() for c in line.strip().strip('|').split('|')]


def stage_key(s):
    m = re.match(r'([0-9]+)([a-z]?)', s)
    return (int(m.group(1)), m.group(2)) if m else (0, '')


def scan(path, tables, errors):
    """Every check table in one file: {section: rows}, with row errors recorded."""
    name = os.path.basename(path)
    section = None
    lines = open(path, encoding='utf-8').read().split('\n')
    i = 0
    while i < len(lines):
        m = HEADING.match(lines[i])
        if m:
            section = m.group(2)
        if lines[i].startswith('|') and cells(lines[i]) == COLUMNS:
            rows = []
            j = i + 2                       # past the header and the rule
            while j < len(lines) and lines[j].startswith('|'):
                rows.append((j + 1, cells(lines[j])))
                j += 1
            if rows or section != '18.19':
                tables.setdefault(section, 0)
                tables[section] += len(rows)
                if not rows:
                    errors.append('%s:%d: 実身化の確認の表に行が無い(%s)' % (name, i + 1, section))
            for ln, c in rows:
                if len(c) != len(COLUMNS):
                    errors.append('%s:%d: 列の数が %d(9 のはず)' % (name, ln, len(c)))
                    continue
                for k, v in enumerate(c):
                    if v == '' or v in ('-', '—', '?'):
                        errors.append('%s:%d: 「%s」が空(%s)' % (name, ln, COLUMNS[k], c[0]))
            i = j
            continue
        i += 1


def check_stages(path, tables, errors):
    name = os.path.basename(path)
    in_stages = False
    for n, line in enumerate(open(path, encoding='utf-8').read().split('\n'), 1):
        m = HEADING.match(line)
        if m:
            in_stages = ( m.group(2) == '18.18' )
            continue
        if not in_stages:
            continue
        m = STAGE_ROW.match(line)
        if not m or stage_key(m.group(1)) < (8, 'f'):
            continue
        r = REF.search(line)
        if r is None:
            errors.append('%s:%d: 段階 %s に「実身化: 節番号」が無い' % (name, n, m.group(1)))
        elif r.group(1) not in tables:
            errors.append('%s:%d: 段階 %s の指す %s に実身化の確認の表が無い'
                          % (name, n, m.group(1), r.group(1)))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    top = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, '..', 'docs', 'private', 'tessronos-design')
    files = sorted(os.path.join(top, f) for f in os.listdir(top) if f.endswith('.md'))
    tables, errors = {}, []
    for f in files:
        scan(f, tables, errors)
    for f in files:
        if os.path.basename(f).startswith('18-'):
            check_stages(f, tables, errors)
    for e in errors:
        print(e)
    print('check_bt2: 確認の表 %d 節、違反 %d 件' % (len(tables), len(errors)))
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
