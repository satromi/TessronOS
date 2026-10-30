#!/usr/bin/env python3
#
# tools/x64/mesa/hostcc.py — x64 の Mesa の建て(tools/x64/mesa/build.sh の WORK/build)の
# compile_commands.json から、選んだソースをホスト(Linux)のコンパイラで翻訳し直す
# (tools/x64/mesa/igt_shaders.sh が使う。第13章 13.11.14)
#
#   hostcc.py --build <WORK/build> --obj <出力先> --jobs N --list <目的物の一覧> <パスの頭>...
#
# パスの頭(例 src/intel/compiler/brw/)で始まるソースだけを選ぶ。生成物(build の下の src/...)も
# 同じ頭で選ばれる。コンパイラをホストの gcc・g++ に替え、TessronOS のプロセス向けの
# 指定(include/tscxx、include/tsposix、-march など)を外す。DRM の模擬のヘッダ(tools/mesa/shim)は
# ホストの C ライブラリのものと重なるので、--hostshim の場所(libdrm の宣言だけを置く)に替える。
# 同じ建てのヘッダと生成物をそのまま使うので、型の並びは本番の建てと同じになる。変わった
# ソースだけを翻訳し直す。--flags-of <ソース> は、そのソースのホスト向けの指定(include の
# パスは絶対)を出すだけで、自前のプログラムをその指定で翻訳するのに使う。

import argparse
import json
import os
import shlex
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

DROP_PREFIX = ('-march=', '-mtune=', '--sysroot', '-specs=', '-fdiagnostics-color')
DROP_EXACT = ('-Winvalid-pch',)


def host_args(cmd, top, hostshim):
    a = shlex.split(cmd)
    cc = 'g++' if a[0].endswith('g++') else 'gcc'
    out = [cc]
    skip = False
    for i, x in enumerate(a[1:]):
        if skip:
            skip = False
            continue
        if x in ('-o', '-MF', '-MQ', '-MT'):
            skip = True
            continue
        if x in ('-c', '-MD'):
            continue
        if x.startswith(DROP_PREFIX) or x in DROP_EXACT:
            continue
        if x.startswith('-I') and (x.endswith('/include/tscxx') or x.endswith('/include/tsposix')):
            continue
        if x.startswith('-I') and x.endswith('/tools/mesa/shim'):
            if hostshim:
                out.append('-I' + hostshim)
            continue
        if x == '-D__TESSRONOS__=1' or x.startswith('-D__TESSRONOS__'):
            continue
        if not x.startswith('-'):
            continue        # the source; given again below
        out.append(x)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', required=True)
    ap.add_argument('--obj', required=True)
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--list', required=True)
    ap.add_argument('--exclude', action='append', default=[])
    ap.add_argument('--hostshim', default=None)
    ap.add_argument('--flags-of', default=None)
    ap.add_argument('--debug', action='store_true')
    ap.add_argument('prefix', nargs='*')
    o = ap.parse_args()

    db = json.load(open(os.path.join(o.build, 'compile_commands.json')))
    if o.flags_of:
        # the host flags of one source, with its include paths made absolute: for a program of our own
        for e in db:
            if e['file'].endswith(o.flags_of):
                fl = []
                for x in host_args(e['command'], e['directory'], o.hostshim)[1:]:
                    if o.debug and x == '-DNDEBUG':
                        continue
                    if x.startswith('-I') and not x.startswith('-I/'):
                        x = '-I' + os.path.normpath(os.path.join(e['directory'], x[2:]))
                    fl.append(x)
                print(' '.join(shlex.quote(x) for x in fl))
                return 0
        return 1
    os.makedirs(o.obj, exist_ok=True)
    work = []
    for e in db:
        f = os.path.normpath(os.path.join(e['directory'], e['file']))
        rel = None
        for root in (os.path.normpath(os.path.join(o.build, '../src')), os.path.normpath(o.build)):
            if f.startswith(root + '/'):
                rel = f[len(root) + 1:]
                break
        if rel is None or not any(rel.startswith(p) for p in o.prefix):
            continue
        if any(x in rel for x in o.exclude):
            continue
        # named after the build's own object: a source built once for each generation stays apart
        a = shlex.split(e['command'])
        bo = a[a.index('-o') + 1] if '-o' in a else rel
        obj = os.path.join(o.obj, bo.replace('/', '_').replace('.p_', '_') + ('' if bo.endswith('.o') else '.o'))
        work.append((e['directory'], e['command'], f, obj))

    def one(w):
        d, cmd, src, obj = w
        if os.path.exists(obj) and os.path.getmtime(obj) >= os.path.getmtime(src):
            return obj, None
        args = host_args(cmd, d, o.hostshim)
        if o.debug:
            # Mesa's own checks on: its assertions, its validation of the shaders
            args = [x for x in args if x != '-DNDEBUG'] + ['-O0', '-g', '-w', '-fPIC', '-c', src, '-o', obj]
        else:
            args = args + ['-O1', '-g0', '-w', '-fPIC', '-c', src, '-o', obj]
        r = subprocess.run(args, cwd=d, capture_output=True, text=True)
        if r.returncode != 0:
            return None, (src, r.stderr.strip().splitlines()[:3])
        return obj, None

    ok, bad = [], []
    with ThreadPoolExecutor(o.jobs) as ex:
        for obj, err in ex.map(one, work):
            if obj:
                ok.append(obj)
            else:
                bad.append(err)
    with open(o.list, 'w') as f:
        f.write('\n'.join(ok) + '\n')
    print('hostcc: %d of %d compiled' % (len(ok), len(work)))
    for src, lines in bad[:20]:
        print('  %s:' % src)
        for l in lines:
            print('    ' + l)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
