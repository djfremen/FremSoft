# CSTech2Win shim — cross-compile from macOS/Linux with MinGW-w64.
#
# Build:    make            -> build/CSTech2Win.dll
# Regenerate stubs from real DLL:
#           make regen
# Clean:    make clean
#
# On Win10, see scripts/build_msvc.bat for native MSVC build.

CC      := i686-w64-mingw32-gcc
DLLTOOL := i686-w64-mingw32-dlltool

CFLAGS  := -m32 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type \
           -fno-stack-protector -static-libgcc -DUNICODE -D_UNICODE
LDFLAGS := -m32 -shared -static-libgcc -Wl,--enable-stdcall-fixup \
           -lkernel32 -luser32

REAL_DLL := ../../CHIPSOFT_J2534_Pro_Driver/CSTech2Win.dll

SRC := src/dllmain.c src/log.c src/wrappers.c src/forwarders.c
OBJ := $(SRC:src/%.c=build/%.o)

all: build/CSTech2Win.dll

build/%.o: src/%.c src/shim.h src/wrappers.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/CSTech2Win.dll: $(OBJ) src/cstech2win.def | build
	$(CC) $(LDFLAGS) -o $@ $(OBJ) src/cstech2win.def

build:
	mkdir -p build

regen:
	python3 scripts/gen_shim.py $(REAL_DLL)

clean:
	rm -rf build src/forwarders.c src/cstech2win.def src/wrappers.h

inspect: build/CSTech2Win.dll
	@echo "=== shim file info ==="
	@file build/CSTech2Win.dll
	@echo
	@echo "=== shim exports ==="
	@python3 -c "import pefile; pe=pefile.PE('build/CSTech2Win.dll'); \
	  [print(f'  ord={e.ordinal:>3}  {e.name.decode() if e.name else \"\"}') \
	   for e in pe.DIRECTORY_ENTRY_EXPORT.symbols]"

.PHONY: all clean regen inspect
