# C² on Linux and macOS: the counterpart of make.bat.
#   make          build ./c2, unoptimized with debug symbols (-O0 -g; MIR objects are cached in .build/)
#   make release  build ./c2, optimized without debug symbols (-O2)
#   make clang    build with clang; make clang release, or release clang
#   make example  build and run ./c2_example, the embedding example (example.c + c2.c as a library)
#   make tour     walk through tour/ (one C² feature each): every demo shown, run, then a pause
#   make test     run tests/test.py      make test ARGS="c2 c2-abi"   make test ARGS="-v -k NAME"
#   make bench    run tests/bench.py against cc/clang -O2 when found   make bench ARGS=fib
#   make tidy     remove build products

CC     ?= cc
CFLAGS ?= -O0 -g
ifneq ($(filter release,$(MAKECMDGOALS)),)        # make release [clang]
CFLAGS := -O2
endif
ifneq ($(filter clang,$(MAKECMDGOALS)),)          # make clang [release]
CC := clang
endif
PYTHON ?= python3
ARGS   ?=

# MIR: NDEBUG turns off its internal checks (one fails on valid code); gcc miscompiles mir.c
# without -fno-tree-sra, as MIR's own build notes.  clang needs neither the flag nor accepts it.
IS_CLANG := $(findstring clang,$(shell $(CC) --version 2>/dev/null))
MIRFLAGS := $(CFLAGS) -DNDEBUG -fsigned-char $(if $(IS_CLANG),,-fno-tree-sra -Wno-abi)
LIBS     := -lm $(if $(filter Darwin,$(shell uname -s)),,-ldl -lpthread)
OBJ      := .build/$(notdir $(CC))$(if $(filter release,$(MAKECMDGOALS)),-release)
MIR_SRC  := $(wildcard 3rd/*.c 3rd/*.h)
# switching between debug, release and compilers rebuilds ./c2: .build/config changes with them
CONFIG   := $(CC) $(CFLAGS)
$(shell mkdir -p .build; [ "`cat .build/config 2>/dev/null`" = "$(CONFIG)" ] || echo "$(CONFIG)" > .build/config)

all: c2

c2: c2.c c2.h $(OBJ)/mir.o $(OBJ)/mir-gen.o .build/config
	$(CC) $(CFLAGS) -DNDEBUG -o $@ c2.c $(OBJ)/mir.o $(OBJ)/mir-gen.o $(LIBS)

c2_example: example.c c2.c c2.h $(OBJ)/mir.o $(OBJ)/mir-gen.o
	$(CC) $(CFLAGS) -DNDEBUG -DC2_NO_MAIN -o $@ example.c c2.c $(OBJ)/mir.o $(OBJ)/mir-gen.o $(LIBS)
example: c2_example
	./c2_example

tour: c2
	@for f in tour/*.c2; do clear 2>/dev/null; printf '// %s\n// ---\n' $$f; cat $$f; echo; ./c2 $$f || true; echo; \
	  printf 'Press Enter to continue . . . '; { read k < /dev/tty; } 2>/dev/null; done

$(OBJ)/%.o: 3rd/%.c $(MIR_SRC)
	@mkdir -p $(OBJ)
	$(CC) $(MIRFLAGS) -c -o $@ $<

clang release: c2
	@true

test: c2
	$(PYTHON) tests/test.py $(ARGS)

BENCH_CC := $(if $(shell command -v cc),--cc "cc -O2=cc -O2 -w -o {exe} {src} -lm") \
            $(if $(IS_CLANG),,$(if $(shell command -v clang),--cc "clang -O2=clang -O2 -w -o {exe} {src} -lm"))
bench: c2
	$(PYTHON) tests/bench.py $(BENCH_CC) $(ARGS)

tidy:
	rm -rf .build .build-clang c2 c2_example

.PHONY: all clang release example tour test bench tidy
