# Android (Ayn Thor) build of the port: the same game tree and bridge as the
# 3DS build (3ds_port/full.mk: sources, per-unit staging flags, data
# pipeline), compiled with the NDK's clang for armeabi-v7a - a 32-bit ARM
# process keeps the port's 4-byte pointer layout (bundles, script bytecode,
# songs) - and linked into libemerald.so with the Android backend in
# android_port/src in place of the libctru one.
#
# Run from a bootstrapped tree's 3ds_port directory:
#   make -C build/upstream/3ds_port -f $PWD/android_port/android.mk -j16
#
# Game units still see PLATFORM_3DS: it names the dual-screen port's
# behaviour (staging, bottom screen, hooks), which Android keeps. Android
# differences are under PLATFORM_ANDROID.

SHELL := bash
.SHELLFLAGS := -o pipefail -c
.DEFAULT_GOAL := all
.SUFFIXES:

ANDROID_PORT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
ANDROID_HOME ?= $(HOME)/Android/Sdk
NDK ?= $(lastword $(sort $(wildcard $(ANDROID_HOME)/ndk/*)))
NDK_BIN := $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin
API := 33
CC := $(NDK_BIN)/armv7a-linux-androideabi$(API)-clang
OBJCOPY := $(NDK_BIN)/llvm-objcopy
READELF := $(NDK_BIN)/llvm-readelf
# The data units use GNU as like the 3DS build (full.mk: $(DEVKITARM)/bin/...).
DEVKITARM ?= /usr

ARCH := -marm
CPPFLAGS := -DPORTABLE -DPLATFORM_3DS -DPLATFORM_ANDROID -Iinclude -I$(ANDROID_PORT)/include
CFLAGS := $(ARCH) -O2 -g -std=gnu99 -fPIC -fvisibility=hidden -Wall -Wextra -ffunction-sections -fdata-sections
NATIVE_CFLAGS := $(CFLAGS) -Werror
LDFLAGS := $(ARCH) -shared -fuse-ld=lld -Wl,--gc-sections -Wl,-z,max-page-size=16384 -Wl,--no-undefined
LIBS := -landroid -lnativewindow -lEGL -lGLESv3 -llog -lOpenSLES -laaudio -lm

TARGET := emerald3ds
# The version mirrors Emerald 3DS's (3ds_port/Makefile, APP_VERSION), whose
# updates this port follows; this port's own additions on top of one are its
# subversion (subversion.txt): 0.5.0, then 0.5.0.1, 0.5.0.2... (build_apk.sh
# derives the APK's version code from the same numbers).
UPSTREAM_VERSION := $(shell sed -n 's/^APP_VERSION := //p' $(ANDROID_PORT)/../3ds_port/Makefile)
ANDROID_SUBVERSION := $(strip $(file <$(ANDROID_PORT)/subversion.txt))
APP_VERSION := $(UPSTREAM_VERSION)$(if $(filter-out 0,$(ANDROID_SUBVERSION)),.$(ANDROID_SUBVERSION))
PYTHON ?= $(abspath $(ANDROID_PORT)/../build/venv/bin/python)
export CTR_BUNDLE_LOCALS := 1
HOSTCC ?= gcc
ROOT := ..
SCRIPTS := ../tools/port_common
EXESUFFIX :=
VOXEL_LIGHTING ?= 1
SHOW_FPS ?= 1
PORT_LANG ?= en
CTR_LANG_PT_BR := $(if $(filter pt_br,$(PORT_LANG)),1,0)
# 0: chosen at runtime (3ds_data.c) - a development install's pushed romfs/
# carries data.embedded and is read as it is; a release APK's engine files do
# not, and its game data is the pack built on the device (Setup.java).
DATA_BACKEND ?= 0

GAMEFLAGS := -DPORTABLE -DPLATFORM_3DS -DPLATFORM_ANDROID -DMODERN=1 -DUBFIX -D__INTELLISENSE__
GAMEFLAGS += -iquote include -iquote ../include -iquote ../include/constants -iquote ..
GAMEFLAGS += -DCTR_VOXEL_LIGHTING=$(VOXEL_LIGHTING) -DCTR_SHOW_FPS=$(SHOW_FPS) -DCTR_LANG_PT_BR=$(CTR_LANG_PT_BR)
CPPFLAGS += -DCTR_VOXEL_LIGHTING=$(VOXEL_LIGHTING) -DCTR_SHOW_FPS=$(SHOW_FPS) -DCTR_LANG_PT_BR=$(CTR_LANG_PT_BR)
CPPFLAGS += -DCTR_DATA_BACKEND=$(DATA_BACKEND)
# Legacy code has existing warnings; clang turns some of GCC's warnings into
# errors by default, so those are warnings again here.
GAMECFLAGS := $(CFLAGS) -fsigned-char -fno-strict-aliasing -w
GAMECFLAGS += -Wno-error=incompatible-pointer-types -Wno-error=int-conversion \
	-Wno-error=implicit-function-declaration -Wno-error=implicit-int \
	-Wno-error=incompatible-function-pointer-types -Wno-error=return-type

.PHONY: FORCE
build/config: FORCE
	@mkdir -p build
	@printf '%s\n' '$(CPPFLAGS) $(CFLAGS) $(GAMEFLAGS) $(GAMECFLAGS) $(LDFLAGS) $(LIBS)' > build/config.new
	@cmp -s build/config.new $@ || cp build/config.new $@
	@rm -f build/config.new

# The Thor's bottom display is 1240x1080: a 320x280 canvas covers it
# (CTR_BOTTOM_HEIGHT, include/3ds_bottom.h), its backdrops drawn as tall.
BOTTOM_ART_FLAGS := --height 280

include full.mk

# ── Android native backend ─────────────────────────────────────────────────
ANDROID_SRCS := $(wildcard $(ANDROID_PORT)/src/*.c)
ANDROID_OBJS := $(patsubst $(ANDROID_PORT)/src/%.c,build/android/%.o,$(ANDROID_SRCS))
# Portable native units of the 3DS backend that need no SDK.
SHARED_NATIVE_SRCS := src/3ds_input_decode.c src/3ds_pak.c src/3ds_daynight.c src/3ds_clock_store.c \
	src/3ds_nuzlocke_core.c src/3ds_nuzlocke_store.c src/3ds_encounters.c src/3ds_field_view.c \
	src/3ds_corner_art.c src/3ds_icons_ptbr.c src/3ds_video_decode.c
SHARED_NATIVE_OBJS := $(patsubst src/%.c,build/%.o,$(SHARED_NATIVE_SRCS))

build/android/%.o: $(ANDROID_PORT)/src/%.c $(wildcard $(ANDROID_PORT)/include/*.h)
	@mkdir -p $(dir $@)
	"$(CC)" $(CPPFLAGS) -Isrc/voxel $(NATIVE_CFLAGS) -MMD -MP -c $< -o $@

build/%.o: src/%.c
	@mkdir -p $(dir $@)
	"$(CC)" $(CPPFLAGS) $(NATIVE_CFLAGS) -MMD -MP -c $< -o $@

GAME_OBJS := $(FULL_C_OBJS) $(FULL_DATA_OBJS) $(BACKEND_OBJS)

# 3DS native units reused through the libctru shim (android_port/include/3ds.h).
SHIMMED_NATIVE_SRCS := src/3ds_data.c src/3ds_settings.c src/3ds_scenario.c
SHIMMED_NATIVE_OBJS := $(patsubst src/%.c,build/%.o,$(SHIMMED_NATIVE_SRCS))
NATIVE_OBJS := $(ANDROID_OBJS) $(SHARED_NATIVE_OBJS) $(SHIMMED_NATIVE_OBJS)

$(NATIVE_OBJS) $(GAME_OBJS): build/config

# Paths: "romfs:/" and "sdmc:/" are translated by these wrappers (android_fs.c).
FS_WRAPPED := fopen rename remove unlink mkdir stat access open
LDFLAGS += $(foreach f,$(FS_WRAPPED),-Wl,--wrap=$(f))
# -Bsymbolic: the library's own symbols bind inside it, so a pointer to one in
# the game's read-only data is a plain link-time address (R_ARM_RELATIVE), not
# a name the dynamic linker resolves: what the gamedata bundle carries must be
# the address itself. Nothing else links against this library but its JNI.
LDFLAGS += -Wl,--build-id=sha1 -Wl,-Bsymbolic

build/buildinfo.c: FORCE
	@mkdir -p build
	@sha=$$(git -C $(ANDROID_PORT) rev-parse HEAD 2>/dev/null || echo unknown); \
	{ printf 'const char gCtrBuildVersion[] = "%s";\n' '$(APP_VERSION)'; \
	  printf 'const char gCtrBuildCommit[] = "%s";\n' "$$sha"; \
	  printf 'const char gCtrBuildBranch[] = "android";\n'; \
	  printf 'const unsigned gCtrBuildDirty = 0;\n'; \
	  printf 'const char gCtrBuildDiff[] = "none";\n'; \
	  printf 'const char gCtrBuildLibs[] = "ndk";\n'; \
	  printf 'const char gCtrBuildDefines[] = "";\n'; \
	  printf 'const char gCtrBuildCompiler[] = __VERSION__;\n'; \
	  printf 'const char gCtrLinkId[] = "E3ID-android";\n'; } > $@.new
	@cmp -s $@.new $@ || cp $@.new $@
	@rm -f $@.new
build/buildinfo.o: build/buildinfo.c
	"$(CC)" $(CFLAGS) -c $< -o $@

# Linked asm data: writable copies of the pointer tables (tools/rw_data.sh).
# The copies keep "build/root/..." in their path for the linker script.
RW_DATA_OBJS := $(patsubst %,build/rw/%,$(FULL_DATA_OBJS))
build/rw/%.o: %.o $(ANDROID_PORT)/tools/rw_data.sh
	bash $(ANDROID_PORT)/tools/rw_data.sh "$(OBJCOPY)" "$(READELF)" $< $@

LINK_OBJS := $(filter-out $(FULL_DATA_OBJS),$(GAME_OBJS)) $(RW_DATA_OBJS) $(NATIVE_OBJS) build/buildinfo.o

# Two links of the same objects (emerald_android.ld): the shipped library with
# the game's read-only data reserved NOLOAD, and the image that keeps it, with
# its relocations (--emit-relocs), for scripts/gen_gamedata_bundle.py
# (full.mk, ROMFS_GAMEDATA_OUTS): romfs/gamedata/gamedata.bin is game data,
# gamedata.rel an engine file. The library carries no game content.
# A game unit's writable data: .data and .data.<name>, .data.rel and
# .data.rel.local.* (writable pointers), but not .data.rel.ro.* - which
# a bare .data.* would take, its tables staying in the library.
LD_WRITABLE_DATA := .data .data.[!r]* .data.r[!e]* .data.re[!l]* .data.rel[!.]* .data.rel .data.rel.[!r]* .data.rel.r[!o]*
build/emerald_android_release.ld: $(ANDROID_PORT)/emerald_android.ld $(ANDROID_PORT)/android.mk
	@mkdir -p build
	sed -e 's/@GAMEDATA_TYPE@/(NOLOAD)/' -e 's/CTR_WRITABLE_DATA/$(LD_WRITABLE_DATA)/' $< > $@
build/emerald_android_image.ld: $(ANDROID_PORT)/emerald_android.ld $(ANDROID_PORT)/android.mk
	@mkdir -p build
	sed -e 's/@GAMEDATA_TYPE@//' -e 's/CTR_WRITABLE_DATA/$(LD_WRITABLE_DATA)/' $< > $@
build/libemerald.so: $(LINK_OBJS) build/emerald_android_release.ld
	$(file >build/link.rsp,$(LINK_OBJS))
	"$(CC)" $(LDFLAGS) -Wl,-T,build/emerald_android_release.ld -Wl,-Map,build/libemerald.lld.map @build/link.rsp $(LIBS) -o $@
build/gamedata_image.elf: $(LINK_OBJS) build/emerald_android_image.ld build/libemerald.so
	$(file >build/link.rsp,$(LINK_OBJS))
	"$(CC)" $(LDFLAGS) -Wl,-T,build/emerald_android_image.ld -Wl,--emit-relocs @build/link.rsp $(LIBS) -o $@

# The data generators read the executable as $(TARGET).elf and its map in
# GNU ld's shape (full.mk: asset table, script and song bundles).
$(TARGET).elf: build/libemerald.so
	cp $< $@
build/$(TARGET).map: build/libemerald.so $(ANDROID_PORT)/tools/lld_map_to_gnu.py
	"$(PYTHON)" $(ANDROID_PORT)/tools/lld_map_to_gnu.py build/libemerald.lld.map $@
$(ROMFS_ASSET_OUTS): build/$(TARGET).map

# Game data for the romfs backend (CTR_DATA_BACKEND=1): what the 3DSX's own
# RomFS carries in a development build, the voxel world's included.
ROMFS_ANDROID := $(ROMFS_ASSET_OUTS) $(ROMFS_MAP_OUT) $(ROMFS_SCRIPT_OUTS) $(ROMFS_SOUND_OUTS) \
	romfs/bottom/battle.bin romfs/bottom/menu.bin romfs/battle/hud.bin romfs/stage/leaves.bin \
	$(ROMFS_SHADER_OUTS) $(ROMFS_GAMEDATA_OUTS)
# The voxel world's vertex programs are GLSL in the Citro3D shim
# (android_port/src/android_c3d.c): its "shbin" names them, in the order the
# 3DS's has its DVLEs (voxel.v.pica, then voxel_weather.v.pica).
romfs/shaders/voxel.shbin: $(ANDROID_PORT)/android.mk
	@mkdir -p $(dir $@)
	printf 'GLES\nvoxel\nweather\n' > $@
romfs/data.embedded:
	printf 'embedded\n' > $@
romfs/engine/abi.bin: $(ROMFS_ANDROID) $(SCRIPTS)/staging.py
	"$(PYTHON)" $(SCRIPTS)/staging.py abi --romfs romfs

.PHONY: all game-objs lib romfs-data
game-objs: $(GAME_OBJS)
lib: build/libemerald.so
romfs-data: $(ROMFS_ANDROID) romfs/engine/abi.bin romfs/data.embedded
all: lib romfs-data

# ── APK ────────────────────────────────────────────────────────────────────
build/libemerald-stripped.so: build/libemerald.so
	"$(NDK_BIN)/llvm-strip" --strip-debug -o $@ $<
.PHONY: apk install
apk: build/libemerald-stripped.so
	bash $(ANDROID_PORT)/build_apk.sh $(abspath build/libemerald-stripped.so)
install: apk
	adb install -r $(ANDROID_PORT)/build/emerald-thor.apk

# The data that records this link's addresses (asset index, script and song
# relocations, ABI) goes to the device with every install; the rest of
# romfs/ only changes with the game's data (push-data).
DEVICE_FILES := /sdcard/Android/data/com.cer0.emerald.dev/files
LINKED_DATA := romfs/assets romfs/scripts romfs/sound romfs/gamedata romfs/engine romfs/bottom
.PHONY: push-linked push-data
push-linked: romfs-data
	tar -cf - $(LINKED_DATA) | adb exec-in "mkdir -p $(DEVICE_FILES) && tar -xf - -C $(DEVICE_FILES)"
push-data: romfs-data
	tar -cf - romfs | adb exec-in "mkdir -p $(DEVICE_FILES) && tar -xf - -C $(DEVICE_FILES)"
install: push-linked
