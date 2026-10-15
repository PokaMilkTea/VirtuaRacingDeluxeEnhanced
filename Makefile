# Builds our programs on top of the PicoDrive emulator core.
#
#   make          build everything into build/
#   make win      vrde.exe for Windows, cross-compiled (build/win/; see "Windows" below)
#   make test     self-checks of logic the -d checks do not cover
#   make clean    remove build/
#
# PicoDrive's own build system is not used: its front ends need SDL 1.2. Instead we
# compile the core's source files directly. The list below is copied from upstream's
# platform/common/common.mak, without the parts only other platforms need.

PICO = third_party/picodrive

CORE_SOURCES = $(addprefix $(PICO)/, \
	pico/pico.c pico/cart.c pico/memory.c pico/state.c pico/sek.c pico/z80if.c \
	pico/videoport.c pico/draw2.c pico/draw.c pico/mode4.c pico/misc.c pico/eeprom.c \
	pico/patch.c pico/debug.c pico/media.c pico/sms.c \
	pico/cd/mcd.c pico/cd/memory.c pico/cd/sek.c pico/cd/cdc.c pico/cd/cdd.c \
	pico/cd/cd_image.c pico/cd/cd_parse.c pico/cd/gfx.c pico/cd/gfx_dma.c pico/cd/misc.c \
	pico/cd/pcm.c pico/cd/megasd.c \
	pico/32x/32x.c pico/32x/memory.c pico/32x/draw.c pico/32x/sh2soc.c pico/32x/pwm.c \
	pico/pico/pico.c pico/pico/memory.c pico/pico/xpcm.c \
	pico/carthw/carthw.c pico/carthw/eeprom_spi.c \
	pico/carthw/svp/svp.c pico/carthw/svp/memory.c pico/carthw/svp/ssp16.c \
	pico/sound/sound.c pico/sound/resampler.c pico/sound/sn76496.c pico/sound/ym2612.c \
	pico/sound/ym2413.c pico/sound/vgm.c pico/sound/mix.c \
	cpu/fame/famec.c cpu/cz80/cz80.c cpu/drc/cmn.c cpu/sh2/sh2.c cpu/sh2/mame/sh2pico.c \
	unzip/unzip.c)
CORE_OBJECTS = $(CORE_SOURCES:%.c=build/%.o)

# CPU emulators: FAME for the 68000, CZ80 for the Z80 (both plain C).
# The SH2s use the interpreter, because the SH2 dynarec would need MAP_JIT on macOS arm64.
# EL_LOGMASK: the core's status messages only. PicoDrive also logs anomalies on x86_64
# (pico_int.h), which on Windows filled the console with a line a tick in races.
DEFINES = -DEMU_F68K -D_USE_CZ80 -DEL_LOGMASK=EL_STATUS

CFLAGS = -O2 -g -Wall
# -MMD -MP: the compiler records which headers each object includes (build/**/*.d),
# so changing a header rebuilds everything that uses it.
CORE_CFLAGS = $(CFLAGS) -I$(PICO) $(DEFINES) -Wno-unused -fno-strict-aliasing -MMD -MP
OUR_CFLAGS = $(CFLAGS) -I$(PICO) $(DEFINES)

# Our programs: core.c plus the program's own file.
OUR_COMMON = src/core.c src/core.h $(PICO)/pico/pico.h

all: build/vrde build/vrdetrace build/sh2dis

build/$(PICO)/%.o: $(PICO)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CORE_CFLAGS) -c $< -o $@

# OpenGL: a framework on macOS, libGL elsewhere (Linux).
# On macOS, also run on every macOS for Apple Silicon (11 and later), not only the one
# that built it (the compiler's default).
ifeq ($(shell uname -s),Darwin)
GL_LIBS = -framework OpenGL
CFLAGS += -mmacosx-version-min=11.0
else
GL_LIBS = -lGL
endif

# The race's 3D from the scene (HLE), the SH2s' own polygon fill and the comparison
# with the SH2s' picture, to check it.
HLE_SOURCES = src/hle.c src/hle.h src/sh2raster.c src/sh2raster.h src/hlecheck.c src/hlecheck.h \
	src/hud.c src/hud.h

# SDL2 + OpenGL front end.
VRDE_SOURCES = src/vrde.c src/layers.c src/settings.c
VRDE_HEADERS = src/layers.h src/settings.h src/gl.h src/version.h
build/vrde: $(VRDE_SOURCES) $(VRDE_HEADERS) $(HLE_SOURCES) $(OUR_COMMON) $(CORE_OBJECTS)
	$(CC) $(OUR_CFLAGS) $$(sdl2-config --cflags) $(filter %.c %.o,$^) $$(sdl2-config --libs) $(GL_LIBS) -lz -lm -o $@

# Headless trace tool: per-frame hashes and screenshots.
build/vrdetrace: src/vrdetrace.c $(HLE_SOURCES) $(OUR_COMMON) $(CORE_OBJECTS)
	$(CC) $(OUR_CFLAGS) $(filter %.c %.o,$^) -lz -lm -o $@

# SH2 disassembler for research (tools/).
build/sh2dis: tools/sh2dis.c $(PICO)/cpu/sh2/mame/sh2dasm.c
	$(CC) $(CFLAGS) -I$(PICO) $^ -o $@

# ---- Windows: vrde.exe cross-compiled with llvm-mingw against SDL2's MinGW development
# files, as the MD version is; zlib from its own source (the core reads zipped ROMs and
# writes compressed states with it). All three are downloaded into deps/ by win-deps.
LLVM_MINGW = llvm-mingw-20260922-ucrt-macos-universal
SDL2_MINGW = SDL2-devel-2.32.10-mingw
ZLIB       = zlib-1.3.1
WIN_CC     = $(CURDIR)/deps/llvm-mingw/bin/x86_64-w64-mingw32-clang
WIN_SDL    = $(CURDIR)/deps/SDL2/x86_64-w64-mingw32
WIN_CFLAGS = -O2 -Wall -Ideps/zlib

WIN_CORE_OBJECTS = $(CORE_SOURCES:%.c=build/win/%.o)
WIN_ZLIB_OBJECTS = $(addprefix build/win/zlib/, adler32.o compress.o crc32.o deflate.o gzclose.o \
	gzlib.o gzread.o gzwrite.o infback.o inffast.o inflate.o inftrees.o trees.o uncompr.o zutil.o)

win: build/win/vrde.exe

win-deps:
	@mkdir -p deps
	[ -d deps/llvm-mingw ] || (curl -L https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/$(LLVM_MINGW).tar.xz \
		| tar xJ -C deps && mv deps/$(LLVM_MINGW) deps/llvm-mingw)
	[ -d deps/SDL2 ] || (curl -L https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/$(SDL2_MINGW).tar.gz \
		| tar xz -C deps && mv deps/SDL2-2.32.10 deps/SDL2)
	[ -d deps/zlib ] || (curl -L https://github.com/madler/zlib/releases/download/v1.3.1/$(ZLIB).tar.gz \
		| tar xz -C deps && mv deps/$(ZLIB) deps/zlib)

build/win/$(PICO)/%.o: $(PICO)/%.c | win-deps
	@mkdir -p $(dir $@)
	$(WIN_CC) $(WIN_CFLAGS) -I$(PICO) $(DEFINES) -Wno-unused -fno-strict-aliasing -MMD -MP -c $< -o $@

build/win/zlib/%.o: deps/zlib/%.c | win-deps
	@mkdir -p $(dir $@)
	$(WIN_CC) -O2 -c $< -o $@

build/win/vrde_rc.o: src/vrde.rc src/vrde.manifest src/version.h | win-deps   # manifest and version information
	@mkdir -p build/win
	$(CURDIR)/deps/llvm-mingw/bin/x86_64-w64-mingw32-windres -I src src/vrde.rc -o $@

build/win/vrde.exe: $(VRDE_SOURCES) $(VRDE_HEADERS) $(HLE_SOURCES) $(OUR_COMMON) $(WIN_CORE_OBJECTS) \
		$(WIN_ZLIB_OBJECTS) build/win/vrde_rc.o
	$(WIN_CC) $(WIN_CFLAGS) -I$(PICO) $(DEFINES) -I$(WIN_SDL)/include/SDL2 $(filter %.c %.o,$^) \
		-L$(WIN_SDL)/lib -lmingw32 -lSDL2main -lSDL2 -lopengl32 -o $@
	cp $(WIN_SDL)/bin/SDL2.dll build/win/
	cp tools/race_inputs.txt tools/check_windows.bat build/win/   # the -d checks, as on the Mac

# Self-checks of logic the -d checks do not cover
test: src/settings.c src/settings.h
	@mkdir -p build
	$(CC) $(CFLAGS) -DSETTINGS_TEST src/settings.c -o build/settings_test && build/settings_test

clean:
	rm -rf build

-include $(CORE_OBJECTS:.o=.d) $(WIN_CORE_OBJECTS:.o=.d)

.PHONY: all clean test win win-deps
