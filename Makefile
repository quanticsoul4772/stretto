CFLAGS = -Os -flto -fuse-linker-plugin -ffast-math \
         -ffunction-sections -fdata-sections -fno-plt \
         -fno-asynchronous-unwind-tables -fno-stack-protector \
         -fno-pic -Qn
LDFLAGS = -Wl,--gc-sections -Wl,-z,norelro \
          -Wl,--hash-style=sysv -no-pie

# UPX_BIN avoids the literal name "UPX" because the upx binary reads
# its own UPX environment variable for default options.
UPX_BIN   ?= upx
UPX_FLAGS ?= --ultra-brute

HEADERS = sin_table.h env_table.h note_table.h euclid_table.h
GENS    = gen_sin_table gen_env_table gen_note_table gen_euclid_table
# Shared synth + UI + WAV + mixer + key dispatch.
# audio backend is platform-specific; see OBJS / WIN_OBJS below.
COMMON_OBJS = arena.o effects.o voice.o gen.o lsystem.o \
              chord_progression.o section.o mixer.o wav.o \
              ui.o keys.o main.o

OBJS     = $(COMMON_OBJS) audio_pulse.o
WIN_OBJS = $(COMMON_OBJS:.o=.win.o) audio_winmm.win.o

# Pure-synth subset (no main / no UI / no audio / no wav) - this is
# what unit tests link against.
OBJS_NO_MAIN = arena.o effects.o voice.o gen.o lsystem.o \
               chord_progression.o section.o

# Size targets (bytes).
STRIP_TARGET = 24576
PACK_TARGET  = 12288

all: synth

# Windows cross-compile target.
# Size-optimized flags mirror the Linux build: -Os + LTO + section
# splitting + gc-sections drop unreferenced code/data. After link
# we strip and (optionally) UPX-pack.
WIN_CC      = x86_64-w64-mingw32-gcc
WIN_STRIP   = x86_64-w64-mingw32-strip
WIN_CFLAGS  = -Os -flto -fuse-linker-plugin -ffunction-sections \
              -fdata-sections -fno-asynchronous-unwind-tables \
              -fno-stack-protector -Qn -DWIN32_LEAN_AND_MEAN
WIN_LDFLAGS = -Wl,--gc-sections -s -no-pie

# Pattern rules with auto-generated header dependencies via -MMD -MP.
# Each compile emits a .d file alongside its .o listing every header
# the .c included. The -include at the bottom of the file feeds those
# back to make so editing any header triggers the right rebuilds.
%.o: %.c $(HEADERS)
	gcc $(CFLAGS) -MMD -MP -c $< -o $@

%.win.o: %.c $(HEADERS)
	$(WIN_CC) $(WIN_CFLAGS) -MMD -MP -MF $*.win.d -c $< -o $@

stretto.exe: $(WIN_OBJS)
	$(WIN_CC) $(WIN_CFLAGS) $(WIN_LDFLAGS) $(WIN_OBJS) -lwinmm -o stretto.exe
	$(WIN_STRIP) -s -R .comment stretto.exe

win: stretto.exe
	@echo "Built: stretto.exe (native Windows binary, live + render mode)"
	@file stretto.exe
	@stat -c "  size: %s bytes" stretto.exe

# UPX-pack the Windows binary. Run after `make win`.
stretto.packed.exe: stretto.exe
	$(UPX_BIN) $(UPX_FLAGS) stretto.exe -o stretto.packed.exe

winpack: stretto.packed.exe
	@echo "Packed: stretto.packed.exe"
	@stat -c "  unpacked: %s bytes" stretto.exe
	@stat -c "  packed:   %s bytes" stretto.packed.exe

# Build-time table generators. Each runs once at build to emit a
# fixed .rodata header that voice.c / gen.c consume.
gen_sin_table: gen_sin_table.c
	gcc -O2 gen_sin_table.c -o gen_sin_table -lm
gen_env_table: gen_env_table.c
	gcc -O2 gen_env_table.c -o gen_env_table -lm
gen_note_table: gen_note_table.c
	gcc -O2 gen_note_table.c -o gen_note_table -lm
gen_euclid_table: gen_euclid_table.c
	gcc -O2 gen_euclid_table.c -o gen_euclid_table

sin_table.h: gen_sin_table
	./gen_sin_table > sin_table.h
env_table.h: gen_env_table
	./gen_env_table > env_table.h
note_table.h: gen_note_table
	./gen_note_table > note_table.h
euclid_table.h: gen_euclid_table
	./gen_euclid_table > euclid_table.h

synth: $(OBJS)
	gcc $(CFLAGS) $(LDFLAGS) $(OBJS) -lpulse -o synth
	strip -s -R .comment synth

synth.packed: synth
	$(UPX_BIN) $(UPX_FLAGS) synth -o synth.packed

pack: synth.packed
	@SIZE=$$(stat -c%s synth.packed); \
	echo "Packed binary size: $$SIZE bytes (target: $(PACK_TARGET))"; \
	if [ $$SIZE -gt $(PACK_TARGET) ]; then \
		echo "WARNING: exceeds $(PACK_TARGET) byte target"; \
	fi

clean:
	rm -rf synth synth.packed stretto.exe stretto.packed.exe \
	       $(BUILD_COV) *.d \
	       tests/unit/test_arena tests/unit/test_effects \
	       tests/unit/test_voice tests/unit/test_gen \
	       tests/unit/test_lsystem tests/unit/test_chord_progression \
	       tests/unit/test_section \
	       $(GENS) $(HEADERS) *.o *.win.o

size: synth
	@SIZE=$$(stat -c%s synth); \
	echo "Stripped binary size: $$SIZE bytes (target: $(STRIP_TARGET))"; \
	if [ $$SIZE -gt $(STRIP_TARGET) ]; then \
		echo "WARNING: exceeds $(STRIP_TARGET) byte target"; \
	fi

test: synth
	./tests/test_bitexact.sh

# Unit tests. Each tests/unit/test_*.c links against OBJS_NO_MAIN
# (all modules except main.o) and runs as a standalone binary.
UNIT_TEST_SRCS = $(wildcard tests/unit/test_*.c)
UNIT_TEST_BINS = $(UNIT_TEST_SRCS:.c=)

tests/unit/test_%: tests/unit/test_%.c tests/unit/test.h $(OBJS_NO_MAIN)
	gcc -O2 -Wall -no-pie -Itests/unit $< $(OBJS_NO_MAIN) -o $@ -lm

test-unit: $(UNIT_TEST_BINS)
	@echo "=== unit tests ==="
	@fail=0; for t in $(UNIT_TEST_BINS); do \
		echo; echo "[$$t]"; \
		./$$t || fail=1; \
	done; \
	exit $$fail

# Multi-seed integration test - renders 4 seeds, checks determinism +
# audio sanity bounds + golden hashes. Catches runaway-state bugs.
test-multiseed: synth
	chmod +x tests/test_multi_seed.sh
	./tests/test_multi_seed.sh

# Live-mode smoke test (Linux only, auto-skips if no PulseAudio).
test-smoke: synth
	chmod +x tests/test_smoke_live.sh
	./tests/test_smoke_live.sh

# Capture golden hashes for all multi-seed renders.
golden-multiseed: synth
	@mkdir -p golden
	@for s in 0 1 42 12345; do \
		./synth --render 4 /tmp/golden_seed_$$s.wav --seed $$s >/dev/null 2>&1; \
		h=$$(sha256sum /tmp/golden_seed_$$s.wav | awk '{print $$1}'); \
		echo "seed_$$s: $$h"; \
	done > golden/regression_multiseed.sha256.txt
	@echo "golden/regression_multiseed.sha256.txt updated:"
	@cat golden/regression_multiseed.sha256.txt

# Coverage build: instrumented compile of every .c into a dedicated
# build_cov/ directory so the instrumented artifacts do not collide
# with the normal build. Runs the regression render + each unit test
# from inside build_cov/, then invokes gcov pointing at the
# instrumented objects. Lets you alternate `make coverage` and
# `make` / `make test-unit` without an intervening `make clean`.
BUILD_COV = build_cov
COV_FLAGS = -O0 -g -fprofile-arcs -ftest-coverage

# Source files split into two groups:
#   MEASURED       - line coverage is reported and CI-gated.
#   INTERACTIVE    - compiled+linked so synth_cov runs, but excluded
#                    from the coverage report because exercising them
#                    requires a TTY and a live audio device (live
#                    audio loop, terminal raw mode, key handler).
COV_SRCS_MEASURED    = arena.c effects.c voice.c gen.c lsystem.c \
                       chord_progression.c section.c mixer.c wav.c \
                       main.c
COV_SRCS_INTERACTIVE = ui.c keys.c audio_pulse.c
COV_SRCS             = $(COV_SRCS_MEASURED) $(COV_SRCS_INTERACTIVE)
COV_OBJS             = $(addprefix $(BUILD_COV)/,$(COV_SRCS:.c=.o))
# Pure-synth subset of instrumented .o files - what unit tests link.
COV_TEST_OBJS        = $(addprefix $(BUILD_COV)/,$(OBJS_NO_MAIN))

$(BUILD_COV):
	@mkdir -p $(BUILD_COV) $(BUILD_COV)/tests/unit

$(BUILD_COV)/%.o: %.c | $(BUILD_COV)
	gcc $(COV_FLAGS) -c $< -o $@

coverage: $(COV_OBJS)
	gcc $(COV_FLAGS) $(COV_OBJS) -lpulse -o $(BUILD_COV)/synth_cov
	@echo "=== render-mode regression ==="
	./$(BUILD_COV)/synth_cov --render 16 /tmp/cov_render.wav --seed 0 >/dev/null
	@echo "=== unit suite ==="
	@for t in $(UNIT_TEST_SRCS); do \
		base=$${t%.c}; \
		out=$(BUILD_COV)/$$base.cov; \
		gcc $(COV_FLAGS) -no-pie -Itests/unit $$t $(COV_TEST_OBJS) \
		    -o $$out -lm; \
		./$$out >/dev/null || true; \
	done
	@echo "=== per-file line coverage (measured set only) ==="
	@cd $(BUILD_COV) && gcov -n -o . $(addprefix ../,$(COV_SRCS_MEASURED)) 2>/dev/null | \
		awk '/^File/ {sub(/[\x27]/,"",$$2); sub(/[\x27]/,"",$$2); f=$$2} \
		     /^Lines/ {sub(/Lines executed:/,""); print f": "$$0}' | \
		grep "\.c"
	@echo "(interactive modules ui.c keys.c audio_pulse.c excluded - require TTY + audio device)"

golden: synth
	@mkdir -p golden
	./synth --render 16 /tmp/golden_render.wav --seed 0
	@sha256sum /tmp/golden_render.wav | awk '{print $$1}' > golden/regression_16s.sha256
	@echo "golden/regression_16s.sha256 updated:"
	@cat golden/regression_16s.sha256

play: synth
	./synth

.PHONY: all clean size pack test test-unit test-multiseed test-smoke \
        coverage golden golden-multiseed play win winpack

# Pick up the auto-generated header dependencies. The leading '-'
# silences "no such file" when these have not been generated yet
# (first build).
-include $(OBJS:.o=.d)
-include $(WIN_OBJS:.o=.d)
