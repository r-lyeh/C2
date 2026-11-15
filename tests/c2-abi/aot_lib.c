/* C² as a library, for aot.c2 compiled ahead of time: `c2 -C aot.c2 > aot_c2.c`, then build
   aot_c2.c + this file + 3rd/mir.c + 3rd/mir-gen.c.  c2.c's c2_rt() gives the program its runtime
   (no c2_rt.c from c2 -Crt).  test.py does this in the c2-abi suite. */
#define C2_NO_MAIN
#include "../../c2.c"
