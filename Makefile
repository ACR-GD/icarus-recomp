# Icarus / Droiyan static recompilation build.
CC      ?= clang
OPT     ?= -O2
SDL_CFLAGS ?= $(shell pkg-config --cflags sdl3 2>/dev/null || echo -I/opt/homebrew/opt/sdl3/include)
SDL_LIBS   ?= $(shell pkg-config --libs sdl3 2>/dev/null || echo -L/opt/homebrew/opt/sdl3/lib -lSDL3)
BUILD   ?= build
BIN     ?= icarus
EXTRA   ?=
CFLAGS  = $(OPT) $(EXTRA) -g -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -D_GNU_SOURCE -Wno-unused-label -Wno-unused-variable \
          -Wno-parentheses-equality -Wno-unused-value -Wno-misleading-indentation -fno-strict-aliasing -fwrapv $(SDL_CFLAGS)
LDLIBS  = $(SDL_LIBS) -lm -lpthread
ifeq ($(OS),Windows_NT)
# guest calls nest as host C calls; Windows' default 1 MB main-thread stack is too small
LDLIBS += -Wl,--stack,33554432
endif

GEN_SRC = $(wildcard gen/*.c)
RT_SRC  = $(wildcard runtime/*.c)
OBJ     = $(patsubst %.c,$(BUILD)/%.o,$(GEN_SRC) $(RT_SRC))
PY      ?= build/venv/bin/python

$(BIN): $(OBJ)
	$(CC) $(EXTRA) -o $@ $(OBJ) $(LDLIBS) $(LDEXTRA)

$(BUILD)/%.o: %.c runtime/rt.h runtime/win32.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

gen: tools/recomp.py tools/pe.py
	$(PY) -I tools/recomp.py data/ICARUS.EXE gen

# CI: build the runtime against a stand-in gen/ that contains no game code
ci-stub:
	mkdir -p gen && cp ci/stub-gen/* gen/

clean:
	rm -rf build/gen build/runtime icarus
.PHONY: gen clean ci-stub

build/is3unpack: tools/is3unpack.c
	@mkdir -p build
	$(CC) -O2 -o $@ $<

# Build data/ from the two disc images (see README)
data: build/is3unpack
	python3 -I tools/prepare_data.py "$(CD1)" "$(CD2)" data
.PHONY: data
