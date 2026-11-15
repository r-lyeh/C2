#!/usr/bin/env python3
"""C² test runner.

    python test.py [--c2 PATH] [--modes api,text,interp,lazy,c] [--cc CMD] [-v] [-k FILTER] [DIR_OR_FILE...]

Each test is a .c file (C) or a .c2 file (C with the C² extensions).  Expected results, next to it (the convention of MIR's c-tests):
    t.c.expectrc  or  t.expectrc   expected exit code      (default 0)
    t.c.expect    or  t.expect     expected stdout          (optional)
    t.c.expecterr or  t.expecterr  lines that must appear, in order, in the compiler's messages
                                   (each a substring, e.g. ":3: error: ..."); when one of them is an
                                   error, the test is meant not to compile and nothing is run
Every test is run once per mode: api (`c2 t.c`) and text (`c2 -T t.c`) by default; also
interp (`c2 -i t.c`), lazy (`c2 -lazy t.c`) and c (`c2 -C t.c`, compiled with c2_rt.c (`c2 -Crt`) by --cc, and run).
"""
import argparse, glob, os, re, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))   # tests/
ROOT = os.path.dirname(HERE)                        # repo root (c2.exe)
TOUR = os.path.join(ROOT, 'tour')                   # tour/: suite c2-tour (each demo must run)
BOOK = os.path.join(ROOT, 'docs', 'LANGUAGE.md')    # its examples: suite c2-book (each must print what it shows)
SUITE_ORDER = ['c2', 'c2-errors', 'c2-debug', 'c2-tour', 'c2-book', 'c2-abi', 'c2-reload',   # ours first, then the others
               'c2m', 'lacc', 'andrewchambers_c']

def suite_of(t, base):
    """the suite a test belongs to: its first directory below the path it was found from"""
    if os.path.abspath(base) == TOUR: return 'c2-tour'
    rel = os.path.relpath(t, base)
    parts = rel.replace('\\', '/').split('/')
    return parts[0] if len(parts) > 1 else os.path.basename(os.path.abspath(base))

def skipped(t):
    """a test whose first lines say `make test: skipped` (it needs a library, a display...), or with a
    NAME.skip file next to it (third-party tests we don't edit: the file says why)"""
    if os.path.exists(t + '.skip') or os.path.exists(os.path.splitext(t)[0] + '.skip'): return True
    try:
        with open(t, encoding='utf-8', errors='replace') as f: return any('make test: skipped' in f.readline() for _ in range(5))
    except OSError: return False

def book_tests(tmp, flt):
    """docs/LANGUAGE.md's examples as tests: each ```c block with a main() is a program (file blocks,
    `// file: NAME` first, are the files it imports), and the ```text block after "Output:" is what it
    must print; lines there like "example.c2:4: warning: ..." are compiler messages instead"""
    if not os.path.exists(BOOK): return []
    text = open(BOOK, encoding='utf-8').read()
    out, files, n = [], {}, 0
    for m in re.finditer(r'```c\n(.*?)```\n(\nOutput:\n\n```text\n(.*?)```\n)?', text, re.S):
        code, shown = m.group(1), m.group(3)
        first = code.split('\n', 1)[0]
        fm = re.match(r'// file: (\S+)', first)
        if fm: files[fm.group(1)] = code.split('\n', 1)[1]; continue
        if shown is None: continue                       # not run (an excerpt)
        n += 1
        d = os.path.join(tmp, 'c2-book', 'example_%02d' % n)
        for name, body in files.items():
            os.makedirs(os.path.dirname(os.path.join(d, name)), exist_ok=True)
            open(os.path.join(d, name), 'w', encoding='utf-8', newline='\n').write(body)
        os.makedirs(d, exist_ok=True)
        t = os.path.join(d, 'example.c2')
        open(t, 'w', encoding='utf-8', newline='\n').write(code)
        msgs = [l for l in shown.splitlines() if re.match(r'example\.c2:\d+: (warning|error)', l)]
        stdout = ''.join(l + '\n' for l in shown.splitlines() if l not in msgs)
        open(t + '.expect', 'w', encoding='utf-8', newline='\n').write(stdout)
        if msgs: open(t + '.expecterr', 'w', encoding='utf-8', newline='\n').write('\n'.join(msgs) + '\n')
        if not flt or flt in t or flt in 'c2-book': out.append(('c2-book', t))
    return out

def find_tests(paths, flt):
    out = []   # (suite, test)
    for p in paths:
        if os.path.isdir(p):
            for root, dirs, files in os.walk(p):
                dirs[:] = [d for d in dirs if d not in ('benchmarks', 'c2-abi', 'c2-reload', 'inc')]   # bench.py; run_abi below; helpers
                out += [(suite_of(os.path.join(root, f), p), os.path.join(root, f))
                        for f in files if f.endswith(('.c', '.c2')) and not skipped(os.path.join(root, f))]
        else:
            out.append((os.path.basename(os.path.dirname(os.path.abspath(p))), p))
    return sorted(st for st in out if not flt or flt in st[1])

def expected(t):
    base = os.path.splitext(t)[0]
    rc, out = 0, None
    for c in (t + '.expectrc', base + '.expectrc'):
        if os.path.exists(c):
            w = open(c).read().split()[0]
            rc = 'crash' if w == 'crash' else int(w)        # crash: any failing exit (signals differ by OS)
            break
    for c in (t + '.expect', base + '.expect'):
        if os.path.exists(c):
            out = open(c, 'rb').read().replace(b'\r\n', b'\n')
            break
    return rc, out

def expected_err(t):
    base = os.path.splitext(t)[0]
    for c in (t + '.expecterr', base + '.expecterr'):
        if os.path.exists(c):
            return [l.rstrip('\r\n') for l in open(c, encoding='utf-8') if l.strip()]
    return None

def match_err(text, want):
    """None if every wanted line is a substring of a message line, in order; else why not"""
    lines, i = text.replace('\r\n', '\n').split('\n'), 0
    for w in want:
        while i < len(lines) and w not in lines[i]: i += 1
        if i == len(lines): return 'expected message missing: "%s"' % w
        i += 1
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('paths', nargs='*', default=[HERE, TOUR],
                    help='tests, folders, or suite names (c2, c2-errors, c2-abi, c2-reload, c2-debug, c2-tour, c2-book, lacc...)')
    ap.add_argument('--c2', default=os.path.join(ROOT, 'c2.exe' if os.name == 'nt' else 'c2'),
                    help='compiler to test (default: ../c2[.exe])')
    ap.add_argument('--modes', default='api,text,debug', help='api text interp lazy c debug (debug: c2 -g, keeping a call stack)')
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
    if os.name == 'nt':                    # crashing tests (c2-debug) exit at once: no Windows Error Reporting
        try:                               # (inherited by the programs started from here; c2 itself unchanged)
            import ctypes
            ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)   # FAILCRITICALERRORS, NOGPFAULTERRORBOX, NOOPENFILEERRORBOX
        except Exception:
            pass
    if not os.path.exists(a.c2):                                 # nothing to test without it
        sys.exit('%s not found: build it first (make)' % a.c2)
    # `test.py c2 lacc` (or `make test c2`): a bare suite name means that folder of tests/
    want_book = a.paths == [HERE, TOUR] or any(p in ('book', 'c2-book') for p in a.paths)
    a.paths = [p for p in a.paths if p not in ('book', 'c2-book')]
    a.paths = [os.path.join(HERE, p) if os.path.isdir(os.path.join(HERE, p)) and not os.path.isdir(p)
               and not p.endswith(('.c', '.c2')) else p for p in a.paths]
    a.paths = [TOUR if p in ('tour', 'c2-tour') and not os.path.isdir(p) else p for p in a.paths]   # tour/: must run
    flags = {'api': [], 'text': ['-T'], 'interp': ['-i'], 'lazy': ['-lazy'], 'debug': ['-g']}   # debug: c2 -g
    tmp = tempfile.mkdtemp(prefix='c2_c_')
    rt = os.path.join(tmp, 'c2_rt.c')
    if 'c' in a.modes.split(','):
        open(rt, 'wb').write(subprocess.run([a.c2, '-Crt'], capture_output=True).stdout)
    modes = ['debug' if m == 'g' else m for m in a.modes.split(',')]
    abi_dir, reload_dir = os.path.join(HERE, 'c2-abi'), os.path.join(HERE, 'c2-reload')
    host_dirs = (abi_dir, reload_dir)                         # host programs: run_abi / run_reload
    tests = find_tests([p for p in a.paths if os.path.abspath(p) not in host_dirs], a.filter)
    if want_book: tests += book_tests(tmp, a.filter)
    passed, failed, t0 = 0, [], time.time()
    suites = []
    for s, _ in tests:
        if s not in suites: suites.append(s)
    for name, d in (('c2-abi', abi_dir), ('c2-reload', reload_dir)):   # the host suites, when asked for
        if (not a.filter or a.filter in name) and os.path.isdir(d) and \
                any(os.path.abspath(p) in (HERE, d) for p in a.paths):
            suites.append(name)
    suites.sort(key=lambda s: (SUITE_ORDER.index(s) if s in SUITE_ORDER else len(SUITE_ORDER), s))
    for s in suites:
        sp, sf, st0 = passed, len(failed), time.time()
        if s == 'c2-abi':
            ok, n = run_abi(a, tmp, failed)
            passed += ok
            for _ in range(max(0, n - ok - (len(failed) - sf))):   # failed checks not itemised: count them anyway
                failed.append((abi_dir, 'c2-abi', 'failed'))
            print('%-18s%4d/%-4d passed  %5.1fs' % (s, ok, n, time.time() - st0), flush=True)
            continue
        if s == 'c2-reload':
            ok, n = run_reload(a, tmp, failed)
            passed += ok
            print('%-18s%4d/%-4d passed  %5.1fs' % (s, ok, n, time.time() - st0), flush=True)
            continue
        print('%-18s' % s, end='', flush=True)
        for t in [t for su, t in tests if su == s]:
            run_test(a, t, modes, flags, tmp, rt, failed)
        n_fail = len(failed) - sf
        n_total = len([t for su, t in tests if su == s]) * len(modes)
        passed = sp + n_total - n_fail
        print('%4d/%-4d passed  %5.1fs' % (n_total - n_fail, n_total, time.time() - st0), flush=True)
    total = passed + len(failed)
    for t, m, why in failed:
        print('FAIL [%s] %s: %s' % (m, t, why))
    print('\n%d/%d passed (%.0f%%) in %.1fs' % (passed, total, 100.0 * passed / max(total, 1), time.time() - t0))
    if len(a.paths) == 2 and a.paths[0] == HERE and not a.filter:   # the whole run: say how to narrow it
        print('one suite: make test SUITE (Linux/macOS: make test ARGS=SUITE), with SUITE one of %s;  '
              'one test: -k NAME' % ' '.join(SUITE_ORDER))
    return 1 if failed else 0

def run_abi(a, tmp, failed):
    """tests/c2-abi: a host program built by the platform's C compiler (abi.c, embedding C²) calls
    scripts with structs of awkward sizes and is called back by them.  Built into .build/ when
    missing or older than its sources; returns (passed, total) or None when it cannot be built."""
    d = os.path.join(HERE, 'c2-abi')
    exe = os.path.join(ROOT, '.build', 'abi_host' + ('.exe' if os.name == 'nt' else ''))
    deps = [os.path.join(ROOT, 'c2.c'), os.path.join(ROOT, 'c2.h')] + glob.glob(os.path.join(d, '*.[ch]')) \
         + glob.glob(os.path.join(ROOT, '3rd', '*.[ch]'))
    if not os.path.exists(exe) or os.path.getmtime(exe) < max(os.path.getmtime(p) for p in deps):
        os.makedirs(os.path.dirname(exe), exist_ok=True)
        src = ' '.join('"%s"' % p for p in [os.path.join(d, 'abi.c'), os.path.join(ROOT, '3rd', 'mir.c'),
                                              os.path.join(ROOT, '3rd', 'mir-gen.c')])
        print('%-18s(building the host: %s)' % ('c2-abi', a.abi_cc.split()[0]), flush=True)
        b = subprocess.run(a.abi_cc.format(exe='"%s"' % exe, src=src), shell=True, cwd=tmp, capture_output=True)
        if b.returncode != 0 or not os.path.exists(exe):
            msg = (b.stdout + b.stderr).decode(errors='replace').strip().split('\n')
            failed.append((os.path.join(d, 'abi.c'), 'c2-abi', 'build: ' + next((l for l in msg if 'error' in l), msg[0] if msg else '?')))
            return 0, 1
    # the C2_load test library: .build/abi_lib.dll / libabi_lib.so / libabi_lib.dylib
    ext = '.dll' if os.name == 'nt' else '.dylib' if sys.platform == 'darwin' else '.so'
    lib = os.path.join(ROOT, '.build', ('' if os.name == 'nt' else 'lib') + 'abi_lib' + ext)
    lsrc = os.path.join(d, 'abi_lib.c')
    if not os.path.exists(lib) or os.path.getmtime(lib) < os.path.getmtime(lsrc):
        b = subprocess.run(a.abi_lib_cc.format(exe='"%s"' % lib, src='"%s"' % lsrc), shell=True, cwd=tmp, capture_output=True)
        if b.returncode != 0 or not os.path.exists(lib):
            msg = (b.stdout + b.stderr).decode(errors='replace').strip().split('\n')
            failed.append((lsrc, 'c2-abi', 'build: ' + next((l for l in msg if 'error' in l), msg[0] if msg else '?')))
    extra = [os.path.join(d, 'abi_lib.h'), os.path.join(ROOT, '.build', 'abi_lib')] if os.path.exists(lib) else []
    p = subprocess.run([exe, os.path.join(d, 'abi_script.c')] + extra, cwd=ROOT, capture_output=True, timeout=a.timeout * 4)
    out = p.stdout.decode(errors='replace')
    m = re.search(r'abi: (\d+) checks, (\d+) failed', out)
    if not m:
        failed.append((os.path.join(d, 'abi.c'), 'c2-abi', 'crashed (exit code %d)' % p.returncode))
        return 0, 1
    total, bad = int(m.group(1)), int(m.group(2))
    for line in out.splitlines():
        if line.startswith('  '): failed.append((os.path.join(d, 'abi_script.c'), 'c2-abi', line.strip()))
    ok = run_aot(a, tmp, failed, deps)
    return total - bad + ok, total + 1

def run_aot(a, tmp, failed, deps):
    """one more c2-abi check: aot.c2 through `c2 -C`, and aot_other.c2 through `c2 -C -prefix other_`,
    linked together with C² as a library (aot_lib.c: c2.c defines c2_rt), must print aot.expect.  Built
    into .build/ when older than c2 or its sources."""
    d = os.path.join(HERE, 'c2-abi')
    script, want = os.path.join(d, 'aot.c2'), os.path.join(d, 'aot.expect')
    exe = os.path.join(ROOT, '.build', 'aot_host' + ('.exe' if os.name == 'nt' else ''))
    gen = os.path.join(ROOT, '.build', 'aot_c2.c')
    other, gen2 = os.path.join(d, 'aot_other.c2'), os.path.join(ROOT, '.build', 'aot_other_c2.c')
    deps = deps + [a.c2, script, other, os.path.join(d, 'aot_lib.c')]
    if not os.path.exists(exe) or os.path.getmtime(exe) < max(os.path.getmtime(p) for p in deps):
        g = subprocess.run([a.c2, '-C', script], capture_output=True, timeout=a.timeout)
        if g.returncode != 0:
            failed.append((script, 'c2-abi', 'c2 -C: ' + g.stderr.decode(errors='replace').strip().split('\n')[0]))
            return 0
        open(gen, 'wb').write(g.stdout)
        g = subprocess.run([a.c2, '-C', '-prefix', 'other_', other], capture_output=True, timeout=a.timeout)
        if g.returncode != 0:
            failed.append((other, 'c2-abi', 'c2 -C -prefix: ' + g.stderr.decode(errors='replace').strip().split('\n')[0]))
            return 0
        open(gen2, 'wb').write(g.stdout)
        src = ' '.join('"%s"' % p for p in [gen, gen2, os.path.join(d, 'aot_lib.c'), os.path.join(ROOT, '3rd', 'mir.c'),
                                              os.path.join(ROOT, '3rd', 'mir-gen.c')])
        print('%-18s(building the ahead-of-time program: %s)' % ('c2-abi', a.abi_cc.split()[0]), flush=True)
        b = subprocess.run(a.abi_cc.format(exe='"%s"' % exe, src=src), shell=True, cwd=tmp, capture_output=True)
        if b.returncode != 0 or not os.path.exists(exe):
            msg = (b.stdout + b.stderr).decode(errors='replace').strip().split('\n')
            failed.append((script, 'c2-abi', 'build: ' + next((l for l in msg if 'error' in l), msg[0] if msg else '?')))
            return 0
    p = subprocess.run([exe], cwd=ROOT, capture_output=True, timeout=a.timeout)
    out = p.stdout.replace(b'\r\n', b'\n').decode(errors='replace')
    if p.returncode != 0 or out != open(want, newline='').read().replace('\r\n', '\n'):
        failed.append((script, 'c2-abi', 'ahead of time: exit code %d, output %r' % (p.returncode, out[:80])))
        return 0
    return 1

def run_reload(a, tmp, failed):
    """tests/c2-reload: a host program (reload.c, embedding C²) that recompiles scripts while it runs.
    Built into .build/ like the abi host; returns (passed, total)."""
    d = os.path.join(HERE, 'c2-reload')
    exe = os.path.join(ROOT, '.build', 'reload_host' + ('.exe' if os.name == 'nt' else ''))
    deps = [os.path.join(ROOT, 'c2.c'), os.path.join(ROOT, 'c2.h'), os.path.join(d, 'reload.c')] \
         + glob.glob(os.path.join(ROOT, '3rd', '*.[ch]'))
    if not os.path.exists(exe) or os.path.getmtime(exe) < max(os.path.getmtime(p) for p in deps):
        os.makedirs(os.path.dirname(exe), exist_ok=True)
        src = ' '.join('"%s"' % p for p in [os.path.join(d, 'reload.c'), os.path.join(ROOT, '3rd', 'mir.c'),
                                              os.path.join(ROOT, '3rd', 'mir-gen.c')])
        print('%-18s(building the host: %s)' % ('c2-reload', a.abi_cc.split()[0]), flush=True)
        b = subprocess.run(a.abi_cc.format(exe='"%s"' % exe, src=src), shell=True, cwd=tmp, capture_output=True)
        if b.returncode != 0 or not os.path.exists(exe):
            msg = (b.stdout + b.stderr).decode(errors='replace').strip().split('\n')
            failed.append((os.path.join(d, 'reload.c'), 'c2-reload', 'build: ' + next((l for l in msg if 'error' in l), msg[0] if msg else '?')))
            return 0, 1
    p = subprocess.run([exe], cwd=ROOT, capture_output=True, timeout=a.timeout * 8)
    out = p.stdout.decode(errors='replace')
    m = re.search(r'reload: (\d+) checks, (\d+) failed', out)
    if not m:
        where = re.search(r'reload: crashed .*', out)
        failed.append((os.path.join(d, 'reload.c'), 'c2-reload', where.group(0) if where else 'crashed (exit code %d)' % p.returncode))
        for line in out.splitlines():
            if line.startswith('  '): failed.append((os.path.join(d, 'reload.c'), 'c2-reload', line.strip()))
        return 0, 1
    total, bad = int(m.group(1)), int(m.group(2))
    for line in out.splitlines():
        if line.startswith('  '): failed.append((os.path.join(d, 'reload.c'), 'c2-reload', line.strip()))
    return total - bad, total

def run_test(a, t, modes, flags, tmp, rt, failed):
    """run one test in every mode; failures are appended to failed"""
    want_rc, want_out = expected(t)
    want_err = expected_err(t)
    try:                                               # "// flags: -g" on the first line: c2 runs with them
        first = open(t, encoding='utf-8', errors='replace').readline()
    except OSError:
        first = ''
    tflags = first.split('flags:', 1)[1].split() if first.startswith('// flags:') else []
    must_fail = want_err is not None and any('error' in w for w in want_err)
    for m in modes:
        if m == 'c' and ('-g' in tflags or '-sandbox' in tflags): continue   # (-g's call stack is the JIT's; c2 -C has #line;
                                                        #  -C output links through c2_rt, which isn't sandboxed)
        cmd = [a.c2] + flags.get(m, []) + [f for f in tflags if f not in flags.get(m, [])] + [os.path.basename(t)]
        compile_msgs = ''                              # c mode: what c2 -C said
        try:
            if m == 'c':                                   # translate to C, compile, run
                name = os.path.splitext(os.path.basename(t))[0].replace('-', '_')
                src = os.path.join(tmp, name + '_gen.c')
                exe = os.path.join(tmp, name + ('.exe' if os.name == 'nt' else ''))
                g = subprocess.run([a.c2, '-C', os.path.basename(t)], cwd=os.path.dirname(t) or '.',
                                   capture_output=True, timeout=a.timeout)
                if must_fail or g.returncode != 0:
                    why = match_err(g.stderr.decode(errors='replace'), want_err) if want_err is not None else None
                    if g.returncode == 0 and must_fail: why = why or 'compiled, but errors were expected'
                    if want_err is None: why = 'c2 -C: ' + g.stderr.decode(errors='replace').strip().split('\n')[0]
                    if want_err is None and want_rc not in (0, 'crash') and want_out is None: why = None   # meant to be rejected
                    if why: failed.append((t, m, why))
                    continue
                compile_msgs = g.stderr.decode(errors='replace')
                open(src, 'wb').write(g.stdout)
                cc = subprocess.run(a.cc.format(src=src + ' ' + rt + ' -I"%s"' % os.path.join(ROOT, '3rd'), exe=exe),   # (minicoro.h)
                                   shell=True, cwd=tmp, capture_output=True)
                if cc.returncode != 0 or not os.path.exists(exe):
                    if want_err is None and want_rc not in (0, 'crash') and want_out is None: continue   # meant to be rejected (at link time)
                    msg = (cc.stdout + cc.stderr).decode(errors='replace').strip().split('\n')
                    failed.append((t, m, 'cc: ' + next((l for l in msg if 'error' in l), msg[0] if msg else '?')))
                    continue
                cmd = [exe]
            p = subprocess.run(cmd, cwd=os.path.dirname(t) or '.', capture_output=True, timeout=a.timeout)
            rc = p.returncode & 0xFF                       # Windows exit codes are 32-bit
            out = p.stdout.replace(b'\r\n', b'\n')
            why = None
            errtext = compile_msgs + p.stderr.decode(errors='replace')
            errlines = [l for l in errtext.splitlines() if 'error' in l]
            if want_err is not None:
                why = match_err(errtext, want_err)
            if why:
                pass
            elif must_fail:
                if not errlines: why = 'compiled, but errors were expected'
            elif errlines and want_rc not in (0, 'crash') and rc != 0 and want_out is None:
                pass                                   # meant to be rejected (c2m: a .expectrc of 1, no .expect)
            elif errlines:
                why = errlines[0]
            elif want_rc == 'crash':
                if rc == 0: why = 'exited with 0, a crash was expected'
            elif rc != want_rc & 0xFF:
                why = 'exit code %d, expected %d' % (rc, want_rc)
            elif want_out is not None and out != want_out:
                got_l, want_l = out.split(b'\n'), want_out.split(b'\n')
                k = next((i for i in range(max(len(got_l), len(want_l)))
                          if (got_l[i] if i < len(got_l) else None) != (want_l[i] if i < len(want_l) else None)), 0)
                show = lambda ls: ls[k].decode(errors='replace').replace('\r', '\\r') if k < len(ls) else '<missing>'
                why = 'stdout differs at line %d: got "%s", expected "%s"' % (k + 1, show(got_l), show(want_l))
        except subprocess.TimeoutExpired:
            why = 'timeout'
        if why:
            failed.append((t, m, why))

if __name__ == '__main__':
    sys.exit(main())
