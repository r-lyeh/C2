@echo off
rem Benchmarks in tests\benchmarks (MIR's c-benchmarks) with c2.exe, c2m.exe and, when found, cl / clang-cl.
rem   cbench.bat                 all benchmarks
rem   cbench.bat -k nbody        one benchmark;  --repeat 3 for best of 3
setlocal
set CC_ARGS=
where cl >/dev/null 2>/dev/null && set CC_ARGS=%CC_ARGS% --cc "cl /O2=cl /nologo /O2 /w /Fe{exe} {src}"
where clang-cl >/dev/null 2>/dev/null && set CC_ARGS=%CC_ARGS% --cc "clang-cl /O2=clang-cl /O2 /w /Fe{exe} {src}"
python "%~dp0bench.py" %CC_ARGS% %*
