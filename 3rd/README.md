Third-party code compiled into C².

minicoro by Eduardo Bart (minicoro.h, v0.2.0): https://github.com/edubart/minicoro, public domain or
MIT No Attribution (see its end).  Coroutines for rt/coro.c2; c2.c includes it with MINICORO_IMPL.

MIR by Vladimir Makarov, MIT license (see LICENSE): https://github.com/vnmakarov/mir

Vendored at commit dc2c763f1c8bc746102ce87933d866335f9e6c06 (2026-10-23): only the files C²
compiles (mir.c and mir-gen.c, plus the headers and per-target files they #include).  c2mir,
the tools and the tests are left out.  To update, copy the same files from a newer checkout.

Local changes, marked `C²:` in the code: mir-x86_64.c, the Windows lazy-generation stub (patch
offsets and stack alignment; sent upstream as a pull request).  Keep them when updating unless
upstream has the fix.
