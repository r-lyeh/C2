MIR by Vladimir Makarov, MIT license (see LICENSE): https://github.com/vnmakarov/mir

Vendored at commit dc2c763f1c8bc746102ce87933d866335f9e6c06 (2026-10-23): only the files mycc
compiles (mir.c and mir-gen.c, plus the headers and per-target files they #include).  c2mir,
the tools and the tests are left out.  To update, copy the same files from a newer checkout.
