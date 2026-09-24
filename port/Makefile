# Mario Party 4 -- native port build.
#
#   port/build-ppc.sh -j8            cross-build for the Power Mac G4 (Docker)
#   make -C port TARGET=host -j8     build and run on this Mac
#   make -C port list                show what the port compiles
#
# Two targets, one set of sources.  `ppc-darwin` is the real one: GCC 14.2 for
# powerpc-apple-darwin8 against the MacOSX10.4u SDK, inside
# ghcr.io/variantxyz/gcc-powerpc-apple-darwin8.  `host` is the development
# build -- the Mac's own clang, same mirror, same port layer -- so the port can
# be run and compared against Dolphin without touching the G4.
#
# The decomp's sources are never edited.  They are copied into $(GEN) by
# tools/mirror_src.py, which applies patches.txt and drops the Metrowerks
# assembly; see port/docs/PLAN.md.

TARGET ?= host
.DEFAULT_GOAL := all
ROOT   := ..
PYTHON ?= python3

# ---- toolchain ---------------------------------------------------------------
ifeq ($(TARGET),ppc-darwin)
  BUILD    := build-ppc-darwin
  CROSS    := /usr/local/bin/powerpc-apple-darwin8-
  SYSROOT  := /usr/local/MacOSX10.4u.sdk
  CC       := $(CROSS)gcc
  AS       := $(CROSS)as
  NM       := $(CROSS)nm
  TARGET_CFLAGS  := -isysroot $(SYSROOT) -mmacosx-version-min=10.4 -malign-natural \
                    -mone-byte-bool
  TARGET_LDFLAGS := -isysroot $(SYSROOT) -mmacosx-version-min=10.4 -static-libgcc
  GAME_UBSAFE := -fno-aggressive-loop-optimizations
  # M17: schedule for the G4's own core.  -mcpu=7450 would also switch AltiVec
  # on for GCC's vectoriser, whose fused multiply-adds round differently from
  # the scalar FPU's, so it is named off; the port's AltiVec is opt-in per
  # object (GX_DRAW_CFLAGS, PSMTX_CFLAGS).  `TUNE=` builds the pre-M17 way.
  TUNE ?= -mcpu=7450 -mtune=7450 -mno-altivec
  CTX_ASM  := src/os/jmp_ppc_darwin.s
  MIRROR_TARGET := ppc
  REL_EXT  := bundle
  SDL2_PREFIX ?= /work/sdl2
  SDL_CFLAGS  := -I$(SDL2_PREFIX)/include/SDL2 -D_THREAD_SAFE
  SDL_LIBS    := -L$(SDL2_PREFIX)/lib -lSDL2 -framework OpenGL -framework Cocoa \
                 -framework Carbon -framework IOKit -framework AGL -framework QuickTime \
                 -framework AudioToolbox -framework AudioUnit
else ifeq ($(TARGET),host)
  BUILD    := build-host
  CC       := clang
  AS       := clang -c -x assembler
  NM       := nm
  TARGET_CFLAGS  :=
  TARGET_LDFLAGS :=
  GAME_UBSAFE :=   # clang does not have the option, and does not do this
  CTX_ASM  := src/os/jmp_arm64.s
  MIRROR_TARGET := host
  REL_EXT  := dylib
  SDL_CFLAGS  := $(shell sdl2-config --cflags)
  SDL_LIBS    := $(shell sdl2-config --libs | sed 's/-lSDL2main//') -framework OpenGL
else
  $(error unknown TARGET $(TARGET) -- use host or ppc-darwin)
endif

GEN := $(BUILD)/gen
EXE := $(BUILD)/marioparty4

# ---- sources -----------------------------------------------------------------
# Exactly what configure.py compiles into the DOL from `Game`, `libhu` and
# `msm`, plus the pure-C half of the SDK's matrix library.  The mirror decides
# the list (tools/mirror_src.py: SRC_GLOBS, MTX_SRCS); we glob its output so the
# two can never drift.
GAME_C   := $(patsubst $(ROOT)/%,%,$(wildcard $(ROOT)/src/game/*.c $(ROOT)/src/game/board/*.c \
                        $(ROOT)/src/msm/*.c $(ROOT)/src/libhu/*.c)) \
            src/dolphin/mtx/mtx.c src/dolphin/mtx/mtxvec.c src/dolphin/mtx/mtx44.c \
            src/dolphin/mtx/vec.c src/dolphin/mtx/quat.c src/dolphin/thp/THPAudio.c
PORT_C   := $(wildcard src/platform/*.c src/os/*.c src/gx/*.c src/dvd/*.c \
                       src/card/*.c src/pad/*.c src/audio/*.c src/debug/*.c src/thp/*.c)
# ---- MusyX (extern/musyx, MIT) -----------------------------------------------
# AxioDL's MusyX reimplementation, compiled from the decomp's own checkout with
# MUSY_TARGET=MUSY_TARGET_PC, which GAME_DEFS already sets.  Everything above
# the SAL is portable exactly as it stands -- the sequencer, the voice and ADSR
# model, the 3-D emitters, the streaming layer and the four aux effects -- so
# these are built unmirrored and unpatched, straight out of extern/.
#
# Two files in that directory are deliberately not here:
#   hw_pc.c      the skeleton PC SAL: every AI call commented out and
#                salAiGetDest returning NULL.  port/src/audio/musyx_sal.c
#                replaces it (and defines the same thirteen symbols).
#   hw_aramdma.c its PC arm is eleven empty bodies -- aramStoreData returns
#                NULL, aramAllocateStreamBuffer returns 0 -- which is exactly
#                the path sndStreamAllocEx/ARAMUpdate/hwGetStreamPlayBuffer
#                take, so the music would have nowhere to live.
#                port/src/audio/musyx_aram.c is the Dolphin arm ported onto
#                the port's flat 16 MB ARAM.
#   profile.c    not in MusyX's own CMakeLists either; needs dolphin/PPCArch.h.
# hw_dolphin.c, hw_lib_dolphin.c and dsp_import.c are guarded down to nothing on
# this target, so they are dropped rather than compiled empty.
MUSYX_DIR := $(ROOT)/extern/musyx/src/musyx/runtime
MUSYX_C   := $(filter-out %/hw_pc.c %/hw_aramdma.c %/profile.c %/hw_dolphin.c \
                          %/hw_lib_dolphin.c %/dsp_import.c, \
               $(wildcard $(MUSYX_DIR)/*.c $(MUSYX_DIR)/*/*.c))
# src/relmod/ is the other side of the fence: compiled into every REL bundle,
# never into the main binary.
RELMOD_C := src/relmod/rel_runtime.c src/relmod/rel_placeholder.c
# ---- the 99 REL modules ------------------------------------------------------
# One Mach-O bundle per relocatable module (PLAN.md §2.4b).  Which translation
# units go in which module is not a list the port maintains: gen_rels.py reads
# it out of the decomp's own config/GMPE01_00/rels/*/splits.txt, so an upstream
# commit that splits a new file into a module is picked up on the next build.
RELS_MK  := $(BUILD)/rels.mk
REL_DIR  := $(BUILD)/rels

$(RELS_MK): tools/gen_rels.py $(wildcard $(ROOT)/config/GMPE01_00/rels/*/splits.txt)
	@mkdir -p $(BUILD)
	$(PYTHON) tools/gen_rels.py --out $@

-include $(RELS_MK)

REL_ALL_SRCS := $(sort $(foreach m,$(REL_MODULES),$(REL_SRCS_$(m))))
REL_OBJS     := $(patsubst %.c,$(BUILD)/rel/%.o,$(REL_ALL_SRCS))
REL_RT_OBJ   := $(BUILD)/rel/relmod/rel_runtime.o
REL_PH_OBJ   := $(BUILD)/rel/relmod/rel_placeholder.o
REL_LIBS     := $(foreach m,$(REL_MODULES),$(REL_DIR)/$(m).$(REL_EXT))
GEN_REL_C    := $(patsubst %,$(GEN)/%,$(REL_ALL_SRCS))


STUBS_C  := $(BUILD)/sdk_stubs.c
STUBS_U_C := $(BUILD)/sdk_stubs_untyped.c

GAME_OBJS := $(patsubst %.c,$(BUILD)/game/%.o,$(GAME_C))
MUSYX_OBJS := $(patsubst $(MUSYX_DIR)/%.c,$(BUILD)/musyx/%.o,$(MUSYX_C))
PORT_OBJS := $(patsubst %.c,$(BUILD)/port/%.o,$(PORT_C)) $(BUILD)/port/jmp.o \
             $(BUILD)/port/kerent_tramp.o $(MUSYX_OBJS)
STUBS_OBJ := $(BUILD)/port/sdk_stubs.o $(BUILD)/port/sdk_stubs_untyped.o

# ---- flags -------------------------------------------------------------------
# TARGET_PC is the decomp's own porting switch.  It matters for three reasons:
# it makes the fixed-width types actually fixed width (`u32` is `unsigned long`
# otherwise, which is 64 bits on the host), it turns the GXPosition*/GXColor*
# vertex writers from write-gather-pipe stores at 0xCC008000 into ordinary
# function calls, and it widens a few pointer parameters to `const void*`.
VERSION_NUM ?= 1
GAME_DEFS := -DTARGET_PC -DVERSION=$(VERSION_NUM) -DNDEBUG=1 -DMTX_USE_C \
             -DMUSY_TARGET=MUSY_TARGET_PC \
             -DPORT_PRC_STACK_MUL=2 -DPORT_PRC_STACK_GUARD=64
GAME_INC  := -I$(GEN)/include -I$(ROOT)/extern/musyx/include \
             -I$(ROOT)/build/GMPE01_01/include -Iinclude
# -fcommon: <dolphin/os.h> makes a tentative definition of __OSBusClock and
#  __OSCoreClock in every translation unit (on the console they are fixed
#  addresses in low memory); the port fills them in from OSInit.
# The warnings that are switched off are all things Metrowerks accepted and
# C99 does not; none of them changes code generation.
GAME_WARN := -w -Wno-implicit-function-declaration -Wno-return-mismatch \
             -Wno-int-conversion -Wno-incompatible-pointer-types \
             -Wno-incompatible-function-pointer-types -Wno-unknown-pragmas \
             -Wno-implicit-int
# -fno-aggressive-loop-optimizations: the safety net under the whole decomp.
# GCC takes an out-of-bounds array access as proof that the iteration doing it
# is unreachable, and will then DELETE a loop's exit test as redundant.  In a
# decomp every struct array length is a reconstruction, and several of them are
# one element short of what the code actually indexes -- so the proof is false
# and what is left is a loop with no bound.  That is not hypothetical: it is
# the m425dll crash (see patches.txt, PLAN.md 18.2), where the six-iteration
# loop at thwomp.c:1943 lost its `var_r29 < 6` test and walked 16 MB past the
# top of MEM1 writing as it went.  `port/tools/ubaudit.sh` lists 39 more sites
# in 20 modules with the same shape.  The individual declarations are worth
# fixing one at a time in patches.txt; this flag is what makes the other 38
# not be a crash in the meantime.  It only changes code where it fires.
# Debug info the G4's gdb can read.  Xcode 3.0's gdb-768 (2007) rejects the
# DWARF 5 that GCC 14 emits by default ("Cannot handle DW_FORM_<unknown>"),
# so every object is built -gdwarf-2 -gstrict-dwarf.  The debug map points at
# the .o files under /work/mp4/port/build-ppc-darwin, which on the G4 is a
# symlink to ~/mp4-work (port/tools/g4_debug_sync.sh keeps it current).
# -fno-shrink-wrap: gdb-768 unwinds by prologue analysis, and a shrink-wrapped
# function looks to it like one that never set up a frame ("No stack frame
# has been allocated", backtrace ends at two frames inside draw_run).
DEBUG_G = -gdwarf-2 -gstrict-dwarf -fno-shrink-wrap
# M34: include/msl_math.h routes the game's sqrtf through MSL's semantics
# (sqrtf(x <= 0) = x, not NaN: m417's water); the port's own files keep libm.
GAME_CFLAGS = -std=gnu11 -O2 $(DEBUG_G) -fno-strict-aliasing -fwrapv -fcommon \
              $(GAME_UBSAFE) $(TUNE) \
              $(GAME_WARN) $(GAME_DEFS) $(GAME_INC) $(TARGET_CFLAGS) \
              -include include/msl_math.h
PORT_CFLAGS = -std=gnu11 -O2 $(DEBUG_G) -fno-strict-aliasing -fcommon \
              -Wall -Wno-unused-parameter -Wno-unknown-pragmas $(TUNE) \
              $(GAME_DEFS) $(GAME_INC) $(SDL_CFLAGS) $(TARGET_CFLAGS)
LDFLAGS   = $(TARGET_LDFLAGS) -lm $(SDL_LIBS) \
            -Wl,-exported_symbols_list,$(BUILD)/exports.txt

# Only these families are stubbed.  Everything else left undefined is the
# host's libc (memcpy, sinf, __stack_chk_fail) and resolves at link time; if
# something genuinely missing slips through, the link says so.
SDK_PREFIXES := ^(GX|OS|VI|DVD|CARD|PAD|AI|AR|ARQ|SI|DSP|THP|DEMO|PS|MTX|VEC|QUAT|C_|snd|DC|IC|LC|PPC|__GX|__OS|__AI|__DVD)

# ---- rules -------------------------------------------------------------------
.PHONY: all clean list mirror stubs rels mtxtest thptest
.SECONDARY:

all: $(EXE) rels

# One mirror for the whole tree; a stamp because it writes hundreds of files.
MIRROR_STAMP := $(BUILD)/mirror.stamp
$(MIRROR_STAMP): tools/mirror_src.py patches.txt $(wildcard include/override/*/*/*.h)
	@mkdir -p $(BUILD)
	$(PYTHON) tools/mirror_src.py --out $(GEN) --target $(MIRROR_TARGET) --quiet
	$(PYTHON) tools/widen_ptr_casts.py --gen $(GEN) --cc "$(CC)" \
	    --cflags "$(GAME_DEFS) $(GAME_INC) $(TARGET_CFLAGS) -std=gnu11 -fcommon"
	@touch $@

mirror: $(MIRROR_STAMP)

# The game's entry point is called main(); keep it out of the host's way.
$(BUILD)/game/src/game/main.o: GAME_CFLAGS += -Dmain=mp4_game_main

# The mirror writes every game source in one go; tell make so, or it will look
# for a rule per file.
GEN_C := $(patsubst %,$(GEN)/%,$(GAME_C))
$(GEN_C): $(MIRROR_STAMP)
	@true

# -MMD -MP writes a .d beside every .o naming the headers it included, and the
# -include at the bottom of this file feeds them back to make.  Without it a
# change to port/include/port.h rebuilt *nothing*: the objects kept the old
# PortOptions layout while main.c got the new one, so `--gxwarn` set the field
# that used to be there and the run went headless.  A struct that two
# translation units disagree about is the worst kind of stale build, because
# every symptom points somewhere else.
$(BUILD)/game/%.o: $(GEN)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(GAME_CFLAGS) -MMD -MP -c -o $@ $<

# the Xbox One driver includes the 10.4 SDK USB headers through a patched copy
# (compat/): GCC 14 rejects the 10.4u SDK's own IOKit/usb/USB.h, whose three
# pack(1)/align=reset pairs are unbalanced. Same fix as the Snowboard Kids
# ports' port/src/platform/compat/. Only ppc-darwin needs it -- the host
# build's own (much newer) system SDK already compiles IOUSBLib.h as-is, and
# the patched copy's older struct definitions collide with it there.
# PAD_XONE_REAL selects the real IOUSBLib implementation; the host target
# leaves it undefined and pad_xone.c falls back to its stub (no IOKit symbols
# to link), because SDL_LIBS on TARGET=host does not carry -framework IOKit
# and this rule may not change link flags -- only ppc-darwin's real Xbox One
# support needs it, host is compile-checked with SDL/keyboard input instead.
ifeq ($(TARGET),ppc-darwin)
PAD_XONE_CFLAGS := -Isrc/pad/compat -DPAD_XONE_REAL
else
PAD_XONE_CFLAGS :=
endif
$(BUILD)/port/src/pad/pad_xone.o: src/pad/pad_xone.c $(MIRROR_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(PORT_CFLAGS) $(PAD_XONE_CFLAGS) -MMD -MP -c -o $@ $<

# The vertex path's phase 2 (src/gx/gx_draw.c finish_vertices) has an AltiVec
# batch behind `#ifdef __ALTIVEC__`.  Only this one object is compiled with
# -maltivec, deliberately: switching it on for the whole tree would let GCC
# emit vector code in the game's own translation units too, which is a much
# larger change to justify than "the hottest loop in the port went vector".
# The scalar path is what every other object and the host build use.
# Off by default, and PLAN.md 15.6 is why: the batch is measurably no faster
# than the scalar loop on a 7450, and leaving it off keeps the reference md5s
# (vec_madd is fused, vec_rsqrte is a different reciprocal square root).
#   port/build-ppc.sh ALTIVEC=1 -j8      build the vector path instead
ALTIVEC ?= 0
ifeq ($(TARGET)$(ALTIVEC),ppc-darwin1)
GX_DRAW_CFLAGS := -maltivec -mabi=altivec
else
GX_DRAW_CFLAGS :=
endif
# M28 (e): the compiler on the port's own GX and decode sources alone, never
# the game's.  GX_OPT="-O3" or GX_OPT="-O3 -ffast-math" into a separate tree:
#   port/build-ppc.sh -j8 BUILD=build-ppc-o3 GX_OPT=-O3
# -O3 changes no float semantics (exact by construction, the md5s confirm);
# -ffast-math does (the CPU transform, the texgens, the lighting fold) and
# needs the ppmdiff argument.  Measured in PLAN.md 43.
GX_OPT ?=
$(BUILD)/port/src/gx/%.o: PORT_CFLAGS += $(GX_OPT)
$(BUILD)/port/src/gx/gx_draw.o: src/gx/gx_draw.c $(MIRROR_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(PORT_CFLAGS) $(GX_DRAW_CFLAGS) -MMD -MP -c -o $@ $<

# M17: the matrix library's AltiVec bodies (PSMTXROMultVecArray), behind
# --noaltivec at run time.  -maltivec here only lets the intrinsics compile;
# GCC's own vectoriser is not turned on by it (that needs -ftree-vectorize,
# which -O2 has with the very-cheap model -- and the scalar bodies in this
# file are plain loops it does not touch; the md5s say so).
ifeq ($(TARGET),ppc-darwin)
PSMTX_CFLAGS := -maltivec -mabi=altivec
else
PSMTX_CFLAGS :=
endif
$(BUILD)/port/src/os/psmtx_c.o: src/os/psmtx_c.c $(MIRROR_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(PORT_CFLAGS) $(PSMTX_CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/port/%.o: %.c $(MIRROR_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(PORT_CFLAGS) -MMD -MP -c -o $@ $<

# MusyX, straight out of extern/ -- no mirror, no patches, so no $(MIRROR_STAMP)
# dependency and no $(GEN)/include on the path.  `-w` for the same reason the
# game's own sources get it: this is upstream code compiled as it is written,
# and the port's -Wall is a statement about the port's own files.
MUSYX_CFLAGS = -std=gnu11 -O2 $(DEBUG_G) -fno-strict-aliasing -fwrapv -fcommon -w $(TUNE) \
               $(GAME_DEFS) -I$(ROOT)/extern/musyx/include -Iinclude $(TARGET_CFLAGS)
$(BUILD)/musyx/%.o: $(MUSYX_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(MUSYX_CFLAGS) -MMD -MP -c -o $@ $<
# M19 (PLAN.md 34.4): hardware.c's hwSaveSample is `#if MUSY_TARGET ==
# MUSY_TARGET_DOLPHIN` -- an empty body here -- so no sample was ever copied
# out of main memory into the sample heap, and every voice read the scratch
# buffer msm loads a group through, long after the game had reused it.  The
# two empty bodies are renamed out of the way in this one translation unit;
# port/src/audio/musyx_aram.c defines the real ones over aramStoreData /
# aramRemoveData, as the Dolphin arm does.  extern/ stays untouched.
# M24 (PLAN.md 39.2): the same trick for snd_handle_irq's one call to
# salHandleAuxProcessing, so the aux effects can be a step of the mixer's job
# on the second core; port/src/audio/musyx_sal.c's hook runs the same loop
# (hw_dspctrl.c:2112) from a capture, inline or on the worker.  And for
# hwSetAUXProcessingCallbacks, the one writer of a studio's effect handlers:
# the game swaps its reverb and delay at every scene change (audio.c "Change
# AUX": the callbacks cleared, the effects freed and re-created), and the
# port's version finishes the pending job before the handlers change under it.
# (The Makefile is a prerequisite: the renames live here, and M24 lost an
# afternoon's A/B to a hardware.o built before one of them was added.)
$(BUILD)/musyx/hardware.o: $(MUSYX_DIR)/hardware.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(MUSYX_CFLAGS) -DhwSaveSample=musyx_pc_hwSaveSample_empty \
	      -DhwRemoveSample=musyx_pc_hwRemoveSample_empty \
	      -DsalHandleAuxProcessing=port_sal_aux_hook \
	      -DhwSetAUXProcessingCallbacks=musyx_pc_hwSetAUXProcessingCallbacks_unused -MMD -MP -c -o $@ $<

$(BUILD)/port/jmp.o: $(CTX_ASM)
	@mkdir -p $(dir $@)
	$(AS) -o $@ $<

# kerent.c's 1,011-entry export-thunk table, which the mirror drops with the
# rest of the Metrowerks assembly, regenerated as `b` instructions that
# assemble identically on arm64 and on 32-bit PowerPC.  See tools/gen_kerent.py.
$(BUILD)/kerent_tramp.s: tools/gen_kerent.py $(ROOT)/src/game/kerent.c
	@mkdir -p $(BUILD)
	$(PYTHON) tools/gen_kerent.py --out $@ --version $(VERSION_NUM)

$(BUILD)/port/kerent_tramp.o: $(BUILD)/kerent_tramp.s
	@mkdir -p $(dir $@)
	$(AS) -o $@ $<

# The stub list is the link's own undefined-symbol set, so nothing is missed by
# hand and nothing dead is generated.
$(BUILD)/missing.txt: $(GAME_OBJS) $(PORT_OBJS) $(REL_OBJS) $(REL_RT_OBJ)
	@mkdir -p $(dir $@)
	@$(NM) -g $(GAME_OBJS) $(PORT_OBJS) $(REL_OBJS) $(REL_RT_OBJ) | awk 'NF==2 && $$1=="U"{print substr($$2,2)}' \
	    | sort -u > $(BUILD)/undef.txt
	@$(NM) -g $(GAME_OBJS) $(PORT_OBJS) $(REL_OBJS) $(REL_RT_OBJ) | awk 'NF==3{print substr($$3,2)}' \
	    | sort -u > $(BUILD)/defined.txt
	@comm -23 $(BUILD)/undef.txt $(BUILD)/defined.txt > $(BUILD)/undefined-all.txt
	@grep -E '$(SDK_PREFIXES)' $(BUILD)/undefined-all.txt > $@ || true
	@grep -vE '$(SDK_PREFIXES)' $(BUILD)/undefined-all.txt > $(BUILD)/left-to-libc.txt || true
	@echo "link: $$(wc -l < $@) SDK symbols to stub, $$(wc -l < $(BUILD)/left-to-libc.txt) left to libc"

$(STUBS_C) $(STUBS_U_C): $(BUILD)/missing.txt tools/gen_stubs.py $(MIRROR_STAMP)
	$(PYTHON) tools/gen_stubs.py --missing $(BUILD)/missing.txt --headers $(GEN)/include \
	    --out $(STUBS_C) --out-untyped $(STUBS_U_C)

$(BUILD)/port/sdk_stubs.o: $(STUBS_C)
	@mkdir -p $(dir $@)
	$(CC) $(PORT_CFLAGS) -c -o $@ $<

$(BUILD)/port/sdk_stubs_untyped.o: $(STUBS_U_C)
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 $(DEBUG_G) -Iinclude $(TARGET_CFLAGS) -c -o $@ $<

stubs: $(STUBS_C)

# Everything the main binary defines is exported, because a REL bundle may
# import any of it: partyboard needed a hand-maintained 1,106-symbol dol.def,
# but the link's own symbol table is both exact and free.  Without this the
# executable exports nothing and every bundle fails to link.
$(BUILD)/exports.txt: $(GAME_OBJS) $(PORT_OBJS) $(STUBS_OBJ)
	@$(NM) -g $^ | awk 'NF==3 && $$2 != "U" {print $$3}' | sort -u > $@
	@echo "link: $$(wc -l < $@) symbols exported to the REL bundles"

# -map: the link map is what tells a snapshot which writable globals belong to
# the game and which belong to the port (PLAN.md 24.2).  It is written next to
# the binary as <exe>.snapmap by tools/gen_snapmap.py, as a sidecar rather than
# a generated object, because a table compiled into the binary would move the
# addresses it describes.
$(EXE): $(GAME_OBJS) $(PORT_OBJS) $(STUBS_OBJ) $(BUILD)/exports.txt
	$(CC) -o $@ $(GAME_OBJS) $(PORT_OBJS) $(STUBS_OBJ) $(LDFLAGS) \
	    -Wl,-map,$(EXE).map
	@$(PYTHON) tools/gen_snapmap.py --map $(EXE).map --exe $@ --out $(EXE).snapmap
	@echo "built $@ ($$(wc -c < $@) bytes)"

$(BUILD)/rel/%.o: $(GEN)/%.c $(MIRROR_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(GAME_CFLAGS) -c -o $@ $<

$(BUILD)/rel/relmod/%.o: src/relmod/%.c $(MIRROR_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(GAME_CFLAGS) -c -o $@ $<

$(GEN_REL_C): $(MIRROR_STAMP)
	@true

# -bundle_loader resolves each module's imports against the main binary at
# build time, so a module that calls something the port has not implemented is
# a link error here rather than a crash later; -exported_symbols_list hides
# everything but the three entry points, which is what gives each module the
# private namespace the 899 colliding names need.
REL_LDFLAGS = -bundle -bundle_loader $(EXE) $(TARGET_LDFLAGS) \
              -Wl,-exported_symbols_list,resources/rel_exports.txt

define REL_MODULE_RULE
$$(REL_DIR)/$(1).$$(REL_EXT): $$(patsubst %.c,$$(BUILD)/rel/%.o,$$(REL_SRCS_$(1))) \
        $$(REL_RT_OBJ) $$(if $$(REL_SRCS_$(1)),,$$(REL_PH_OBJ)) $$(EXE) resources/rel_exports.txt
	@mkdir -p $$(REL_DIR)
	@$$(CC) $$(REL_LDFLAGS) -Wl,-seg1addr,$$(REL_SEG1_$(1)) -o $$@ \
	    $$(patsubst %.c,$$(BUILD)/rel/%.o,$$(REL_SRCS_$(1))) $$(REL_RT_OBJ) \
	    $$(if $$(REL_SRCS_$(1)),,$$(REL_PH_OBJ))
endef
$(foreach m,$(REL_MODULES),$(eval $(call REL_MODULE_RULE,$(m))))

rels: $(REL_LIBS)
	@echo "built $(words $(REL_LIBS)) REL bundles in $(REL_DIR)"

reltest: all
	$(EXE) --reltest

# The matrix library, checked against itself: every constructor writes all
# twelve elements, every in-place form matches the out-of-place answer, every
# PS* replacement matches the C_* body it stands in for.  See tests/mtx_test.c;
# this is the test that would have found PLAN.md 15.1 in one run.
MTXTEST := $(BUILD)/mtx_test
$(MTXTEST): tests/mtx_test.c $(BUILD)/game/src/dolphin/mtx/mtx.o \
        $(BUILD)/game/src/dolphin/mtx/mtxvec.o $(BUILD)/game/src/dolphin/mtx/vec.o \
        $(BUILD)/game/src/dolphin/mtx/quat.o $(BUILD)/port/src/os/psmtx_c.o \
        $(BUILD)/port/src/os/fast_sincos.o
	@mkdir -p $(dir $@)
	$(CC) $(PORT_CFLAGS) -o $@ $^ -lm

# M38: the THP decoder alone, on a movie file (tests/thp_test.c, PLAN.md 53)
thptest: $(BUILD)/thp_test
$(BUILD)/thp_test: tests/thp_test.c src/thp/thp_jpeg.c src/thp/thp_jpeg.h
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O2 -Wall $(TUNE) $(TARGET_CFLAGS) -o $@ tests/thp_test.c src/thp/thp_jpeg.c $(TARGET_LDFLAGS)

# M42: the fast sin/cos against libm over its whole domain (PLAN.md 57.3)
sincostest: $(BUILD)/sincos_test
$(BUILD)/sincos_test: tests/sincos_test.c $(BUILD)/port/src/os/fast_sincos.o
	@mkdir -p $(BUILD)
	$(CC) $(filter-out $(DEBUG_G),$(PORT_CFLAGS)) -o $@ $^ $(TARGET_LDFLAGS) -lpthread -lm

# M42: does the driver run a vertex program's ARL on the card (PLAN.md 57.4)
vparltest: $(BUILD)/vp_arl_test
$(BUILD)/vp_arl_test: tests/vp_arl_test.c
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -O2 -Wall $(TUNE) $(TARGET_CFLAGS) -o $@ tests/vp_arl_test.c $(TARGET_LDFLAGS) -framework OpenGL

mtxtest: $(MTXTEST)
	$(MTXTEST)

list: $(MIRROR_STAMP)
	@echo "GAME_C ($(words $(GAME_C))):"; printf '  %s\n' $(GAME_C) | head -20; echo "  ..."
	@echo "PORT_C ($(words $(PORT_C))):"; printf '  %s\n' $(PORT_C)

clean:
	rm -rf $(BUILD)

# Header dependencies written by -MMD above (see the compile rules).
DEPFILES := $(shell find $(BUILD) -name '*.d' 2>/dev/null)
-include $(DEPFILES)
