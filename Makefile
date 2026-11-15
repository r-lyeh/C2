# C² on Linux and macOS: the counterpart of make.bat.
#   make          build ./c2 (MIR objects are cached in .build/, so later builds only redo c2.c)
#   make clang    build with clang
#   make demo     build and run ./demo_embed, the embedding example (demo_embed.c + c2.c as a library)
#   make tour     run demo_tour.c2, a tour of the C² additions
#   make test     run tests/verify.py      make test ARGS="c2 lacc"   make test ARGS="-v -k NAME"
#   make bench    run tests/bench.py against cc/clang -O2 when found   make bench ARGS="-k fib"
#   make tidy     remove build products

CC     ?= cc
CFLAGS ?= -O2
PYTHON ?= python3
ARGS   ?=

# MIR: NDEBUG turns off its internal checks (one fails on valid code); gcc miscompiles mir.c
# without -fno-tree-sra, as MIR's own build notes.  clang needs neither the flag nor accepts it.
IS_CLANG := $(findstring clang,$(shell $(CC) --version 2>/dev/null))
MIRFLAGS := $(CFLAGS) -DNDEBUG -fsigned-char $(if $(IS_CLANG),,-fno-tree-sra -Wno-abi)
LIBS     := -lm $(if $(filter Darwin,$(shell uname -s)),,-ldl -lpthread)
OBJ      := .build/$(notdir $(CC))
MIR_SRC  := $(wildcard mir/*.c mir/*.h)

all: c2

c2: c2.c c2.h $(OBJ)/mir.o $(OBJ)/mir-gen.o
	$(CC) $(CFLAGS) -DNDEBUG -o $@ c2.c $(OBJ)/mir.o $(OBJ)/mir-gen.o $(LIBS)

demo_embed: demo_embed.c c2.c c2.h $(OBJ)/mir.o $(OBJ)/mir-gen.o
	$(CC) $(CFLAGS) -DNDEBUG -DC2_NO_MAIN -o $@ demo_embed.c c2.c $(OBJ)/mir.o $(OBJ)/mir-gen.o $(LIBS)
demo: demo_embed
	./demo_embed

tour: c2
	./c2 demo_tour.c2

$(OBJ)/%.o: mir/%.c $(MIR_SRC)
	@mkdir -p $(OBJ)
	$(CC) $(MIRFLAGS) -c -o $@ $<

clang:
	$(MAKE) CC=clang c2

test: c2
	$(PYTHON) tests/verify.py $(ARGS)

BENCH_CC := $(if $(shell command -v cc),--cc "cc -O2=cc -O2 -w -o {exe} {src} -lm") \
            $(if $(IS_CLANG),,$(if $(shell command -v clang),--cc "clang -O2=clang -O2 -w -o {exe} {src} -lm"))
bench: c2
	$(PYTHON) tests/bench.py $(BENCH_CC) $(ARGS)

tidy:
	rm -rf .build .build-clang c2 demo_embed demo c2_demo

.PHONY: all clang demo tour test bench tidy
