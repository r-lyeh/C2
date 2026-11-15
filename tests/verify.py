#!/usr/bin/env python3
"""C² test runner.

    python verify.py [--c2 PATH] [--modes api,text,interp,lazy,c] [--cc CMD] [-v] [-k FILTER] [DIR_OR_FILE...]

Each test is a .c file (C) or a .c2 file (C with the C² extensions).  Expected results, next to it (the convention of MIR's c-tests):
    t.c.expectrc  or  t.expectrc   expected exit code      (default 0)
    t.c.expect    or  t.expect     expected stdout          (optional)
Every test is run once per mode: api (`c2 t.c`) and text (`c2 -T t.c`) by default; also
interp (`c2 -i t.c`), lazy (`c2 -lazy t.c`) and c (`c2 -C t.c`, compiled with c2_rt.c (`c2 -Crt`) by --cc, and run).
"""
import argparse, glob, os, re, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))   # tests/
ROOT = os.path.dirname(HERE)                        # repo root (c2.exe)

def suite_of(t, base):
    """the suite a test belongs to: its first directory below the path it was found from"""
    rel = os.path.relpath(t, base)
    parts = rel.replace('\\', '/').split('/')
    return parts[0] if len(parts) > 1 else os.path.basename(os.path.abspath(base))

def find_tests(paths, flt):
    out = []   # (suite, test)
    for p in paths:
        if os.path.isdir(p):
            for root, dirs, files in os.walk(p):
                dirs[:] = [d for d in dirs if d not in ('benchmarks', 'abi')]   # bench.py; run_abi below
                out += [(suite_of(os.path.join(root, f), p), os.path.join(root, f))
                        for f in files if f.endswith(('.c', '.c2'))]
        else:
            out.append((os.path.basename(os.path.dirname(os.path.abspath(p))), p))
    return sorted(st for st in out if not flt or flt in st[1])

def expected(t):
    base = os.path.splitext(t)[0]
    rc, out = 0, None
    for c in (t + '.expectrc', base + '.expectrc'):
        if os.path.exists(c):
            rc = int(open(c).read().split()[0])
            break
    for c in (t + '.expect', base + '.expect'):
        if os.path.exists(c):
            out = open(c, 'rb').read().replace(b'\r\n', b'\n')
            break
    return rc, out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('paths', nargs='*', default=[HERE], help='tests, folders, or suite names (c2, lacc, abi...)')
    ap.add_argument('--c2', default=os.path.join(ROOT, 'c2.exe' if os.name == 'nt' else 'c2'),
                    help='compiler to test (default: ../c2[.exe])')
    ap.add_argument('--modes', default='api,text')
    ap.add_argument('--timeout', type=float, default=30)
    ap.add_argument('--cc', default='cl /nologo /O1 /w /Fe{exe} {src}' if os.name == 'nt' else 'cc -O1 -w -o {exe} {src} -lm',
                    help='C compiler for the c mode: {src} and {exe} are replaced')
    ap.add_argument('--abi-cc', default='cl /nologo /O1 /DNDEBUG /MT /w /Fe{exe} {src}' if os.name == 'nt'
                    else 'cc -O1 -DNDEBUG -fno-tree-sra -w -o {exe} {src} -lm -ldl -lpthread',
                    help='C compiler for the abi host program: {src} and {exe} are replaced')
    ap.add_argument('--abi-lib-cc', default='cl /nologo /O1 /MT /LD /w /Fe{exe} {src}' if os.name == 'nt'
                    else 'cc -O1 -shared -fPIC -w -o {exe} {src}',
                    help='C compiler for the C2_load test library: {src} and {exe} are replaced')
    ap.add_argument('-k', dest='filter', default='')
    ap.add_argument('-v', action='store_true', help='(kept for old scripts: failures always say why now)')
    a = ap.parse_args()
    # `verify.py c2 lacc` (or `make test c2`): a bare suite name means that folder of tests/
    a.paths = [os.path.join(HERE, p) if os.path.isdir(os.path.join(HERE, p)) and not os.path.isdir(p)
               and not p.endswith(('.c', '.c2')) else p for p in a.paths]
    flags = {'api': [], 'text': ['-T'], 'interp': ['-i'], 'lazy': ['-lazy']}
    tmp = tempfile.mkdtemp(prefix='c2_c_')
    rt = os.path.join(tmp, 'c2_rt.c')
    if 'c' in a.modes.split(','):
        open(rt, 'wb').write(subprocess.run([a.c2, '-Crt'], capture_output=True).stdout)
    modes = a.modes.split(',')
    abi_dir = os.path.join(HERE, 'abi')
    tests = find_tests([p for p in a.paths if os.path.abspath(p) != abi_dir], a.filter)
    passed, failed, t0 = 0, [], time.time()
    suites = []
    for s, _ in tests:
        if s not in suites: suites.append(s)
    for s in suites:
        sp, sf, st0 = passed, len(failed), time.time()
        print('%-18s' % s, end='', flush=True)
        for t in [t for su, t in tests if su == s]:
            run_test(a, t, modes, flags, tmp, rt, failed)
        n_fail = len(failed) - sf
        n_total = len([t for su, t in tests if su == s]) * len(modes)
        passed = sp + n_total - n_fail
        print('%4d/%-4d passed  %5.1fs' % (n_total - n_fail, n_total, time.time() - st0), flush=True)
    want_abi = (not a.filter or a.filter in 'abi') and any(
        os.path.abspath(p) in (HERE, os.path.join(HERE, 'abi')) for p in a.paths)
    if want_abi and os.path.isdir(os.path.join(HERE, 'abi')):
        st0, sf = time.time(), len(failed)
        r = run_abi(a, tmp, failed)
        ok, n = r
        passed += ok
        failed_extra = n - ok - (len(failed) - sf)       # failed checks not itemised: count them anyway
        for _ in range(max(0, failed_extra)): failed.append((os.path.join(HERE, 'abi'), 'abi', 'failed'))
        print('%-18s%4d/%-4d passed  %5.1fs' % ('abi', ok, n, time.time() - st0), flush=True)
    total = passed + len(failed)
    for t, m, why in failed:
        print('FAIL [%s] %s: %s' % (m, t, why))
    print('\n%d/%d passed (%.0f%%) in %.1fs' % (passed, total, 100.0 * passed / max(total, 1), time.time() - t0))
    return 1 if failed else 0

def run_abi(a, tmp, failed):
    """tests/abi: a host program built by the platform's C compiler (abi.c, embedding C²) calls
    scripts with structs of awkward sizes and is called back by them.  Built into .build/ when
    missing or older than its sources; returns (passed, total) or None when it cannot be built."""
    d = os.path.join(HERE, 'abi')
    exe = os.path.join(ROOT, '.build', 'abi_host' + ('.exe' if os.name == 'nt' else ''))
    deps = [os.path.join(ROOT, 'c2.c'), os.path.join(ROOT, 'c2.h')] + glob.glob(os.path.join(d, '*.[ch]')) \
         + glob.glob(os.path.join(ROOT, 'mir', '*.[ch]'))
    if not os.path.exists(exe) or os.path.getmtime(exe) < max(os.path.getmtime(p) for p in deps):
        os.makedirs(os.path.dirname(exe), exist_ok=True)
        src = ' '.join('"%s"' % p for p in [os.path.join(d, 'abi.c'), os.path.join(ROOT, 'mir', 'mir.c'),
                                              os.path.join(ROOT, 'mir', 'mir-gen.c')])
        print('%-18s(building the host: %s)' % ('abi', a.abi_cc.split()[0]), flush=True)
        b = subprocess.run(a.abi_cc.format(exe='"%s"' % exe, src=src), shell=True, cwd=tmp, capture_output=True)
        if b.returncode != 0 or not os.path.exists(exe):
            msg = (b.stdout + b.stderr).decode(errors='replace').strip().split('\n')
            failed.append((os.path.join(d, 'abi.c'), 'abi', 'build: ' + next((l for l in msg if 'error' in l), msg[0] if msg else '?')))
            return 0, 1
    # the C2_load test library: .build/abi_lib.dll / libabi_lib.so / libabi_lib.dylib
    ext = '.dll' if os.name == 'nt' else '.dylib' if sys.platform == 'darwin' else '.so'
    lib = os.path.join(ROOT, '.build', ('' if os.name == 'nt' else 'lib') + 'abi_lib' + ext)
    lsrc = os.path.join(d, 'abi_lib.c')
    if not os.path.exists(lib) or os.path.getmtime(lib) < os.path.getmtime(lsrc):
        b = subprocess.run(a.abi_lib_cc.format(exe='"%s"' % lib, src='"%s"' % lsrc), shell=True, cwd=tmp, capture_output=True)
        if b.returncode != 0 or not os.path.exists(lib):
            msg = (b.stdout + b.stderr).decode(errors='replace').strip().split('\n')
            failed.append((lsrc, 'abi', 'build: ' + next((l for l in msg if 'error' in l), msg[0] if msg else '?')))
    extra = [os.path.join(d, 'abi_lib.h'), os.path.join(ROOT, '.build', 'abi_lib')] if os.path.exists(lib) else []
    p = subprocess.run([exe, os.path.join(d, 'abi_script.c')] + extra, cwd=ROOT, capture_output=True, timeout=a.timeout * 4)
    out = p.stdout.decode(errors='replace')
    m = re.search(r'abi: (\d+) checks, (\d+) failed', out)
    if not m:
        failed.append((os.path.join(d, 'abi.c'), 'abi', 'crashed (exit code %d)' % p.returncode))
        return 0, 1
    total, bad = int(m.group(1)), int(m.group(2))
    for line in out.splitlines():
        if line.startswith('  '): failed.append((os.path.join(d, 'abi_script.c'), 'abi', line.strip()))
    return total - bad, total

def run_test(a, t, modes, flags, tmp, rt, failed):
    """run one test in every mode; failures are appended to failed"""
    want_rc, want_out = expected(t)
    for m in modes:
        cmd = [a.c2] + flags.get(m, []) + [os.path.basename(t)]
        try:
            if m == 'c':                                   # translate to C, compile, run
                name = os.path.splitext(os.path.basename(t))[0].replace('-', '_')
                src = os.path.join(tmp, name + '_gen.c')
                exe = os.path.join(tmp, name + ('.exe' if os.name == 'nt' else ''))
                g = subprocess.run([a.c2, '-C', os.path.basename(t)], cwd=os.path.dirname(t) or '.',
                                   capture_output=True, timeout=a.timeout)
                if g.returncode != 0:
                    failed.append((t, m, 'c2 -C: ' + g.stderr.decode(errors='replace').strip().split('\n')[0]))
                    continue
                open(src, 'wb').write(g.stdout)
                cc = subprocess.run(a.cc.format(src=src + ' ' + rt, exe=exe), shell=True, cwd=tmp, capture_output=True)
                if cc.returncode != 0 or not os.path.exists(exe):
                    msg = (cc.stdout + cc.stderr).decode(errors='replace').strip().split('\n')
                    failed.append((t, m, 'cc: ' + next((l for l in msg if 'error' in l), msg[0] if msg else '?')))
                    continue
                cmd = [exe]
            p = subprocess.run(cmd, cwd=os.path.dirname(t) or '.', capture_output=True, timeout=a.timeout)
            rc = p.returncode & 0xFF                       # Windows exit codes are 32-bit
            out = p.stdout.replace(b'\r\n', b'\n')
            why = None
            errlines = [l for l in p.stderr.decode(errors='replace').splitlines() if 'error' in l]
            if errlines:
                why = errlines[0]
            elif rc != want_rc & 0xFF:
                why = 'exit code %d, expected %d' % (rc, want_rc)
            elif want_out is not None and out != want_out:
                got_l, want_l = out.split(b'\n'), want_out.split(b'\n')
                k = next((i for i in range(max(len(got_l), len(want_l)))
                          if (got_l[i] if i < len(got_l) else None) != (want_l[i] if i < len(want_l) else None)), 0)
                show = lambda ls: ls[k].decode(errors='replace') if k < len(ls) else '<missing>'
                why = 'stdout differs at line %d: got "%s", expected "%s"' % (k + 1, show(got_l), show(want_l))
        except subprocess.TimeoutExpired:
            why = 'timeout'
        if why:
            failed.append((t, m, why))

if __name__ == '__main__':
    sys.exit(main())
