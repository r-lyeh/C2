#!/usr/bin/env python3
"""Run MIR's C benchmarks with C² and other compilers, check the output, compare times.

    python bench.py                                  # c2 (+ c2m if found) on the default suite
    python bench.py --cc "cl=cl /nologo /O2 /Fe{exe} {src}" --cc "clang=clang-cl /O2 /Fe{exe} {src}"
    python bench.py -k nbody --repeat 3

Each benchmark is NAME.c with NAME.arg (a shell snippet that echoes the arguments) and, usually,
NAME.expect (expected stdout).  JIT runners (c2, c2m) are timed compile+run; --cc compilers are
built first and only the run is timed.  Results: a table on stdout and bench_results.csv.
"""
import argparse, os, re, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = '.exe' if os.name == 'nt' else ''
ROOT = os.path.dirname(HERE)                               # repo root: c2.exe, c2m.exe
DEFAULT_DIR = os.path.join(HERE, 'benchmarks')             # MIR's c-benchmarks
SKIP = {'oggenc', 'except'}   # oggenc: huge, needs input file; except: setjmp/longjmp


def bench_args(path):
    """the .arg files are shell snippets like `echo 5000000`; take the last echoed value"""
    if not os.path.exists(path):
        return []
    found = re.findall(r'echo\s+([^\n;]+)', open(path).read())
    return found[-1].split() if found else []


def find_tool(name):
    for cand in (os.path.join(ROOT, name + EXE), shutil.which(name)):
        if cand and os.path.exists(cand):
            return cand
    return None


def run(cmd, cwd, timeout):
    t0 = time.perf_counter()
    try:
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, timeout=timeout, shell=isinstance(cmd, str))
    except subprocess.TimeoutExpired:
        return None, b'', 'timeout'
    dt = time.perf_counter() - t0
    err = p.stderr.decode(errors='replace').strip().splitlines()
    err = next((l for l in err if 'error' in l.lower()), err[0] if err and p.returncode else '')
    return dt, p.stdout.replace(b'\r\n', b'\n'), (err or ('exit %d' % p.returncode)) if p.returncode else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir', nargs='?', default=DEFAULT_DIR)
    ap.add_argument('--c2', default=find_tool('c2'))
    ap.add_argument('--c2m', default=find_tool('c2m'), help='path to c2m ("" to skip)')
    ap.add_argument('--cc', action='append', default=[],
                    help='NAME=COMMAND with {src} and {exe}, e.g. "gcc=gcc -O2 -o {exe} {src} -lm"')
    ap.add_argument('-k', dest='filter', default='')
    ap.add_argument('--repeat', type=int, default=1, help='best of N runs')
    ap.add_argument('--timeout', type=float, default=300)
    a = ap.parse_args()
    a.dir = os.path.abspath(a.dir)
    if a.c2: a.c2 = os.path.abspath(a.c2)
    if a.c2m: a.c2m = os.path.abspath(a.c2m)

    names = sorted(f[:-2] for f in os.listdir(a.dir) if f.endswith('.c') and f[:-2] not in SKIP and a.filter in f)
    runners = []   # (label, kind, command)
    if a.c2: runners.append(('c2', 'jit', [a.c2]))
    if a.c2m: runners.append(('c2m -eg', 'c2m', [a.c2m, '-I' + a.dir]))
    for spec in a.cc:
        label, _, cmd = spec.partition('=')
        runners.append((label, 'native', cmd))
    if not runners:
        sys.exit('no compilers: build c2 first (make.bat) or pass --cc')

    tmp = tempfile.mkdtemp(prefix='c2-bench-')
    results = {}   # (bench, label) -> seconds or error string
    for i, b in enumerate(names):
        print('[%d/%d] %s %s' % (i + 1, len(names), b, ' '.join(bench_args(os.path.join(a.dir, b + '.arg')))), flush=True)
        src = os.path.join(a.dir, b + '.c')
        args = bench_args(os.path.join(a.dir, b + '.arg'))
        expect = os.path.join(a.dir, b + '.expect')
        expect = open(expect, 'rb').read().replace(b'\r\n', b'\n') if os.path.exists(expect) else None
        for label, kind, cmd in runners:
            if kind == 'native':
                exe = os.path.join(tmp, b + EXE)
                build = cmd.format(src=src, exe=exe)
                _, _, err = run(build, tmp, a.timeout)
                if err:
                    results[(b, label)] = 'build: ' + err
                    continue
                full = [exe] + args
            elif kind == 'c2m':
                full = cmd + [src, '-eg'] + args
            else:
                full = cmd + [src] + args
            best, res = None, None
            for _ in range(a.repeat):
                dt, out, err = run(full, a.dir, a.timeout)
                if err:
                    res = err
                    break
                if expect is not None and out != expect:
                    res = 'wrong output'
                    break
                best = dt if best is None or dt < best else best
            results[(b, label)] = res if res else best
            shown = '%.2fs' % best if not res else res
            print('    %-10s %s' % (label, shown), flush=True)

    labels = [r[0] for r in runners]
    print('\n| benchmark | ' + ' | '.join(labels) + ' |')
    print('|---|' + '---|' * len(labels))
    totals = {l: 0.0 for l in labels}
    complete = {l: True for l in labels}
    for b in names:
        cells = []
        for l in labels:
            v = results.get((b, l))
            if isinstance(v, float):
                cells.append('%.2f' % v)
                totals[l] += v
            else:
                cells.append(str(v)[:30])
                complete[l] = False
        print('| %s | %s |' % (b, ' | '.join(cells)))
    print('| **total** | ' + ' | '.join(('%.2f' % totals[l]) + ('' if complete[l] else ' (incomplete)') for l in labels) + ' |')
    with open(os.path.join(HERE, 'bench_results.csv'), 'w') as f:
        f.write('benchmark,' + ','.join(labels) + '\n')
        for b in names:
            f.write(b + ',' + ','.join(('%.3f' % results[(b, l)]) if isinstance(results.get((b, l)), float)
                                       else str(results.get((b, l))) for l in labels) + '\n')
    shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    main()
