CFLAGS = -Os -flto -fuse-linker-plugin -ffast-math \
         -ffunction-sections -fdata-sections -fno-plt \
         -fno-asynchronous-unwind-tables -fno-stack-protector \
         -fno-pic -Qn \
         -pthread -latomic
LDFLAGS = -Wl,--gc-sections -Wl,-z,norelro \
          -Wl,--hash-style=sysv -no-pie \
          -pthread -latomic

# libasound link flag, dragged in only when the system has
# libasound2-dev installed (provides alsa.pc). Since T022 the
# real ALSA worker in audio_midi_linux.c references libasound
# symbols, every link that includes audio_midi_linux.o needs
# this - the synth target, the synth_debug target, the
# coverage-instrumented synth_cov target, the coverage unit-
# test loop, and the per-test unit-test rule. CI installs
# libasound2-dev (ci.yml apt-get line) so all link lines
# resolve; dev boxes without it still build partial artifacts
# (audio_midi_linux.o fails to link, surfaced at the synth
# target). Detection uses pkg-config rather than ldconfig so
# we honor the alsa.pc-installed contract that libasound2-dev
# provides.
LIBASOUND = $(shell pkg-config --exists alsa && echo -lasound)

# UPX_BIN avoids the literal name "UPX" because the upx binary reads
# its own UPX environment variable for default options.
UPX_BIN   ?= upx
UPX_FLAGS ?= --ultra-brute

# Version identity for --version: git describe when building from a
# checkout (leading 'v' stripped for GNU-format output; --dirty marks
# locally-modified builds), "dev" for tarball builds with no git.
# Subshell-parenthesized so the `|| echo dev` fallback fires on git
# failure BEFORE the sed (a bare `git | sed || echo` would take sed's
# exit status and yield an empty version outside a checkout).
# version.h is deliberately NOT in $(HEADERS): a version change must
# rebuild only main.o (the sole consumer). The version.h rule itself
# lives BELOW `all:` - a rule up here would become the Makefile's
# default goal and `make` would build nothing but version.h.
STRETTO_VERSION := $(shell (git describe --tags --always --dirty 2>/dev/null || echo dev) | sed 's/^v//')

HEADERS = sin_table.h env_table.h note_table.h euclid_table.h wavetable.h
GENS    = gen_sin_table gen_env_table gen_note_table gen_euclid_table gen_wavetable
# Shared synth + UI + WAV + mixer + key dispatch.
# audio backend is platform-specific; see OBJS / WIN_OBJS below.
COMMON_OBJS = arena.o effects.o voice.o gen.o lsystem.o \
              chord_progression.o section.o density.o motif.o \
              mixer.o wav.o ui.o keys.o main.o

OBJS     = $(COMMON_OBJS) audio_pulse.o audio_midi.o audio_midi_linux.o
WIN_OBJS = $(COMMON_OBJS:.o=.win.o) audio_winmm.win.o audio_midi.win.o audio_midi_winmm.win.o

# Everything except main.o and audio backends - this is what unit
# tests link against. Includes ui.o and keys.o so test_keys can
# invoke the live-mode key dispatcher with ui_set_no_ui(1) keeping
# the terminal output side quiet. Also includes mixer.o and wav.o
# so test_mixer and test_wav can exercise the master-bus chain
# and the WAV writer directly.
OBJS_NO_MAIN = arena.o effects.o voice.o gen.o lsystem.o \
               chord_progression.o section.o density.o motif.o \
               mixer.o wav.o ui.o keys.o \
               audio_midi.o audio_midi_linux.o

# Size targets (bytes). STRIP_TARGET bumped 24576 -> 51200 (~50 KB) on
# 2026-07-08 per Constitution v1.1.0 Principle I amendment (PR #19): the
# 003 MIDI chain grew synth stripped from ~24 KB (pre-#109 baseline) to
# 43 880 B (post-#113, measured via `make size` artifact on PR #115). The
# ~+19 KB cost is the principled cost of cross-platform MIDI input per
# Principle III + IX + X. 51 200 leaves 14% headroom (7 320 B) over the
# post-#113 measurement; future growth should reset the budget rather
# than defer required functionality.
#
# PACK_TARGET bumped 12288 -> 30720 (~30 KB) on 2026-07-08 per
# Constitution v1.2.0 amendment (this PR, 021-upx-remeasure). The actual
# Linux UPX-packed synth size is 25 460 B per PR #117 `binary-sizes`
# CI artifact on current `main` -- ~107 % over the prior 12 KB target
# and ~59 % over the pre-#109 (~16 KB) ARCHITECTURE.md hedge. The 003
# chain added the proportional ~13 KB UPX-packed growth on top of the
# 19 KB stripped growth. 30 720 leaves ~21 % headroom (5 260 B) over
# the post-#117 measurement; the headroom matches STRIP_TARGET's 14 %
# pattern (slightly wider for UPX ratio variability). PACK_TARGET now
# lives in the Constitution Principle I line explicitly as `≤30 KB
# UPX-packed Linux binary`, closing the pre-#118 implicit-cap loophole.
#
# WIN_PACK_BUDGET introduced 2026-07-08 per Constitution v1.1.0
# (Principle I line explicitly enumerates the Windows UPX cap; the
# ci.yml `Binary size budget gate` step previously hardcoded the
# 49152 B value inline). 48 KB = 49152 B is post-#117 measurement
# baseline (~38 KB packed actual, last measured PR #117 binary-sizes
# artifact); ~10 KB headroom under 48 KB. The 3 size-budget
# variables (STRIP_TARGET / PACK_TARGET / WIN_PACK_BUDGET) are
# enforced equal to the Constitution Principle I paragraph via
# tools/spec-budget-check.sh, which runs as a pre-flight in
# .github/workflows/ci.yml BEFORE the inline Binary size budget
# gate step catches drift vs binary-sizes.txt measurements.
STRIP_TARGET    = 51200
PACK_TARGET     = 30720
WIN_PACK_BUDGET = 49152

all: synth

# FORCE (not .PHONY on version.h - that would force main.o rebuilds
# every run): the recipe runs on every make, but the cmp-swap leaves
# version.h's mtime untouched when the version is unchanged, so
# nothing recompiles. Placed after `all:` so it cannot become the
# default goal.
version.h: FORCE
	@printf '#define STRETTO_VERSION "%s"\n' '$(STRETTO_VERSION)' > version.h.tmp
	@if cmp -s version.h.tmp version.h; then rm -f version.h.tmp; else mv version.h.tmp version.h; fi
FORCE:

# First-build ordering for the main.o variants (-MMD records the
# include thereafter). BUILD_COV's main.o dep lives next to the
# coverage rules - $(BUILD_COV) is defined further down and
# target/prereq lists expand immediately.
main.o main.win.o main.dbg.o: version.h

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
gen_wavetable: gen_wavetable.c
	gcc -O2 gen_wavetable.c -o gen_wavetable -lm

sin_table.h: gen_sin_table
	./gen_sin_table > sin_table.h
env_table.h: gen_env_table
	./gen_env_table > env_table.h
note_table.h: gen_note_table
	./gen_note_table > note_table.h
euclid_table.h: gen_euclid_table
	./gen_euclid_table > euclid_table.h
wavetable.h: gen_wavetable
	./gen_wavetable > wavetable.h

synth: $(OBJS)
# -lasound (via $(LIBASOUND)) is dragged in only when the system
# has libasound2-dev installed; see the LIBASOUND definition
# above for the rationale. Since T022 the real ALSA worker in
# audio_midi_linux.c references libasound symbols, $(LIBASOUND)
# resolves to -lasound on any link that includes audio_midi_linux.o
# - the synth target, synth_debug, the coverage-instrumented
# synth_cov, the coverage unit-test loop, and the per-test
# unit-test rule. CI installs libasound2-dev (ci.yml apt-get line)
# so all link lines resolve; dev boxes without it still build
# partial artifacts (audio_midi_linux.o fails to link, surfaced
# at this target).
	gcc $(CFLAGS) $(LDFLAGS) $(OBJS) -lpulse $(LIBASOUND) -o synth
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
	rm -rf synth synth.packed synth_debug stretto.exe stretto.packed.exe \
	       $(BUILD_COV) *.d *.dbg.o *.dbg.d \
	       tests/unit/test_arena tests/unit/test_effects \
	       tests/unit/test_voice tests/unit/test_gen \
	       tests/unit/test_lsystem tests/unit/test_chord_progression \
	       tests/unit/test_section tests/unit/test_density \
	       tests/unit/test_motif tests/unit/test_midi \
	       tests/unit/test_mixer tests/unit/test_wav tests/unit/test_keys \
	       $(GENS) $(HEADERS) version.h version.h.tmp *.o *.win.o

# Size report: builds every binary whose toolchain is locally available
# (gcc + libpulse + libasound for synth; upx for *.packed; mingw for
# stretto.exe variants) and prints byte counts for all 4 as key=value
# (rows not built locally print "missing"), plus the 3 budget_* rows
# echoing STRIP_TARGET / PACK_TARGET / WIN_PACK_BUDGET so the ci.yml
# `Binary size budget gate` step reads measurements AND budgets from
# the same binary-sizes.txt artifact (single source of truth: this
# Makefile; no inline budget constants in ci.yml).
# Use this for ARCHITECTURE.md
# binary-size hedge refresh: local dev hosts without upx OR mingw still
# get the rows they CAN build, so future docs refreshes don't need
# WSL toolchain debugging. CI has the full toolchain so all 4 rows are
# populated; the matching ci.yml step uploads the report as the
# `binary-sizes` artifact for any PR.
size:
	@$(MAKE) -s synth
# if/then/else, NOT `cmd && build || echo`: the && || form routed a
# FAILED build into the harmless "skipping" message, so CI (which has
# the full toolchain) could green-light a broken packed/cross build.
	@if command -v $(UPX_BIN) >/dev/null 2>&1; then $(MAKE) -s synth.packed; else echo "info: synth.packed: UPX ($(UPX_BIN)) not on PATH -- skipping"; fi
	@if command -v $(WIN_CC) >/dev/null 2>&1; then $(MAKE) -s stretto.exe; else echo "info: stretto.exe: Windows cross-compiler ($(WIN_CC)) not on PATH -- skipping"; fi
	@if command -v $(UPX_BIN) >/dev/null 2>&1 && command -v $(WIN_CC) >/dev/null 2>&1; then $(MAKE) -s stretto.packed.exe; else echo "info: stretto.packed.exe: UPX OR Windows toolchain missing -- skipping"; fi
	@printf '\n=== binary sizes (key=value; "missing" if not built locally) ===\n'
	@printf 'linux_synth_stripped=%s\n' "$$(stat -c%s synth 2>/dev/null || echo missing)"
	@printf 'linux_synth_packed=%s\n' "$$(stat -c%s synth.packed 2>/dev/null || echo missing)"
	@printf 'windows_stretto_exe_stripped=%s\n' "$$(stat -c%s stretto.exe 2>/dev/null || echo missing)"
	@printf 'windows_stretto_exe_packed=%s\n' "$$(stat -c%s stretto.packed.exe 2>/dev/null || echo missing)"
	@printf 'budget_linux_synth_stripped=%s\n' '$(STRIP_TARGET)'
	@printf 'budget_linux_synth_packed=%s\n' '$(PACK_TARGET)'
	@printf 'budget_windows_stretto_exe_packed=%s\n' '$(WIN_PACK_BUDGET)'
	@echo
	@echo "Constitution Principle I targets:"
	@printf '  STRIP_TARGET  (Linux synth stripped)   : %s bytes\n' '$(STRIP_TARGET)'
	@printf '  PACK_TARGET   (Linux synth UPX-packed) : %s bytes\n' '$(PACK_TARGET)'
	@printf '  WIN_PACK_BUDGET (Windows UPX-packed)  : %s bytes (%s KB)\n' '$(WIN_PACK_BUDGET)' "$$(( $(WIN_PACK_BUDGET) / 1024 ))"
	@SIZE=$$(stat -c%s synth 2>/dev/null || echo 0); \
	if [ "$$SIZE" != "0" ] && [ "$$SIZE" -gt "$(STRIP_TARGET)" ]; then \
		echo "WARNING: synth $$SIZE > STRIP_TARGET $(STRIP_TARGET) -- exceeds Linux stripped budget"; \
	fi

# Bit-exact regression test + spec<>build size-budget bridge regression
# test + spec<>build size-budget amend helper regression test. The
# bridge regression (tests/test_spec_budget_check.sh) exercises the
# 5-case suite for tools/spec-budget-check.sh introduced by PR #122 /
# 025-spec-to-make-bridge; the amend regression
# (tests/test_spec_budget_amend.sh) exercises the 6-scenario / 21-sub-check
# suite for tools/spec-budget-amend.sh introduced by PR #127 /
# 032-spec-budget-amend. Both were previously standalone scripts.
# Wiring them into `make test` ensures any future contributor who
# breaks the Constitution<->Makefile triple-budget alignment (via the
# bridge) or the amendment workflow (via the amend helper) catches
# it at `make test` time on their dev box, not just at CI. The ci.yml
# `Bridge regression test (Constitution<->Makefile)` + `Amend helper
# regression test (Constitution<->Makefile)` steps mirror this as
# dedicated CI checks.
# No chmod here: the test scripts are committed mode 100755. The old
# `chmod +x` flipped tracked mode bits on Linux (core.fileMode=true),
# which made `git describe --dirty` report every post-`make test`
# build as -dirty (the #129/#130 "chmod trap", now retired at the
# root by committing the executable bits).
test: synth
	./tests/test_cli.sh
	./tests/test_bitexact.sh
	./tests/test_spec_budget_check.sh
	./tests/test_spec_budget_amend.sh

# Unit tests. Each tests/unit/test_*.c links against OBJS_NO_MAIN
# (all modules except main.o) and runs as a standalone binary.
UNIT_TEST_SRCS = $(wildcard tests/unit/test_*.c)
UNIT_TEST_BINS = $(UNIT_TEST_SRCS:.c=)

tests/unit/test_%: tests/unit/test_%.c tests/unit/test.h $(OBJS_NO_MAIN)
# -pthread -latomic is REQUIRED here (no flag bag, e.g. CFLAGS/LDFLAGS,
# propagates into this rule): $(OBJS_NO_MAIN) transitively links
# audio_midi.o (uses __atomic_*) and audio_midi_linux.o (uses
# pthread_create / pthread_join / __atomic_*). Without these flags
# the link fails on architectures where libatomic is a separate
# library and on any host where libpthread symbols are not already
# pulled in by libc itself. $(LIBASOUND) covers audio_midi_linux.o's
# libasound references.
	gcc -O2 -Wall -no-pie -Itests/unit $< $(OBJS_NO_MAIN) -o $@ -lm -pthread -latomic $(LIBASOUND)

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

# One-command local dev check for the Constitution<->Makefile bridge.
# Bundles the 3 spec<->build verification artifacts in order, exiting
# on first failure with a clear per-step status. Equivalent to running
# the 3 dedicated ci.yml steps (Bridge regression test + Amend helper
# regression test + the inline Binary size budget gate's pre-flight)
# on a dev box. Use this before opening a PR to catch spec<->build
# drift + amend helper regressions locally instead of waiting for CI.
# See tools/verify-bridge.sh for the wrapper's full design + exit
# codes + per-step failure-recovery hints.
verify:
	@bash tools/verify-bridge.sh

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
#                    from the coverage report AND the per-file gate.
#                    Three reasons qualify a module as INTERACTIVE:
#                      (a) requires a TTY + a live audio device
#                          (ui.c keys.c audio_pulse.c) - the live loop
#                          + raw-mode terminal raw + key handler.
#                      (b) platform backend whose code paths need a
#                          reachable ALSA sequencer + real controller
#                          hardware (audio_midi_linux.c) - CI runners
#                          have no /dev/snd/seq, so every path past
#                          snd_seq_open() is untestable there.
#                      (c) entry-point whose non-default argv branches
#                          are only reachable via direct process
#                          invocation that CI's `make coverage` render
#                          path does not pass (main.c) - the 5-flag
#                          --midi* argv pre-scan is only exercised when
#                          the user runs `synth --midi*`. The default
#                          `make coverage` invocation is `--render 110`
#                          only, so 4 of 5 --midi* branches sit dormant.
#                          Lifting main.c back to MEASURED requires
#                          either extracting the pre-scan into a
#                          callable helper that test_midi.c can invoke,
#                          or adding a fork+exec integration test -
#                          both are explicit follow-ups rather than
#                          gate-dodging. The spec-kit principled move
#                          is to declare the limitation rather than
#                          inflate coverage metrics.
COV_SRCS_MEASURED    = arena.c effects.c voice.c gen.c lsystem.c \
                       chord_progression.c section.c density.c motif.c \
                       mixer.c wav.c audio_midi.c
COV_SRCS_INTERACTIVE = ui.c keys.c audio_pulse.c audio_midi_linux.c main.c
COV_SRCS             = $(COV_SRCS_MEASURED) $(COV_SRCS_INTERACTIVE)
COV_OBJS             = $(addprefix $(BUILD_COV)/,$(COV_SRCS:.c=.o))
# Pure-synth subset of instrumented .o files - what unit tests link.
COV_TEST_OBJS        = $(addprefix $(BUILD_COV)/,$(OBJS_NO_MAIN))

$(BUILD_COV):
	@mkdir -p $(BUILD_COV) $(BUILD_COV)/tests/unit

# Coverage main.o needs version.h to exist on first build (the
# coverage pattern rule has no $(HEADERS) prereq; see the version.h
# rule near the top of this file).
$(BUILD_COV)/main.o: version.h

$(BUILD_COV)/%.o: %.c | $(BUILD_COV)
	gcc $(COV_FLAGS) -c $< -o $@

coverage: $(COV_OBJS)
	gcc $(COV_FLAGS) $(COV_OBJS) -lpulse $(LIBASOUND) -o $(BUILD_COV)/synth_cov
	@echo "=== render-mode regression ==="
	# 110 s (~55 bars at 2.00 s/bar) covers INTRO (bars 0-23), BODY (24-47)
	# and TENSION (48+) so section-gated branches in gen.c - chord
	# arpeggio, TENSION kick pattern, density swings - all execute under
	# the render binary's coverage measurement.
	./$(BUILD_COV)/synth_cov --render 110 /tmp/cov_render.wav --seed 0 >/dev/null
	# 1-second stdout render exercises wav.c's to_stdout branch
	# (added with `--render N -`) so the file stays over its >=90%
	# coverage gate; WAV bytes discarded, diagnostics are stderr-only.
	./$(BUILD_COV)/synth_cov --render 1 - --seed 0 >/dev/null
	@echo "=== unit suite ==="
	@for t in $(UNIT_TEST_SRCS); do \
		base=$${t%.c}; \
		out=$(BUILD_COV)/$$base.cov; \
		gcc $(COV_FLAGS) -no-pie -Itests/unit $$t $(COV_TEST_OBJS) \
		    -o $$out -lm $(LIBASOUND); \
		./$$out >/dev/null || true; \
	done
	@echo "=== per-file line coverage (measured set only) ==="
	@cd $(BUILD_COV) && gcov -n -o . $(addprefix ../,$(COV_SRCS_MEASURED)) 2>/dev/null | \
		awk '/^File/ {sub(/[\x27]/,"",$$2); sub(/[\x27]/,"",$$2); f=$$2} \
		     /^Lines/ {sub(/Lines executed:/,""); print f": "$$0}' | \
		grep "\.c"
	@echo "(interactive modules ui.c keys.c audio_pulse.c audio_midi_linux.c main.c excluded - require TTY/audio device or process invocation)"

golden: synth
	@mkdir -p golden
	./synth --render 16 /tmp/golden_render.wav --seed 0
	@sha256sum /tmp/golden_render.wav | awk '{print $$1}' > golden/regression_16s.sha256
	@echo "golden/regression_16s.sha256 updated:"
	@cat golden/regression_16s.sha256

play: synth
	./synth

# Install as the canonical name "stretto" (the Linux build artifact is
# ./synth; USAGE and the man page use the canonical name). Deliberately
# NO build prerequisite: the version.h FORCE recipe runs on every make,
# and under `sudo make install` git fails the safe.directory ownership
# check, so STRETTO_VERSION would fall back to "dev", main.o would
# relink, and the INSTALLED binary would report `stretto dev` (plus
# root-owned .o litter). Build first as your user: make && sudo make install.
PREFIX ?= /usr/local

install:
	@test -x synth || { echo "install: ./synth not built; run 'make' first (as your user, not root)"; exit 1; }
	install -Dm755 synth $(DESTDIR)$(PREFIX)/bin/stretto
	install -Dm644 stretto.1 $(DESTDIR)$(PREFIX)/share/man/man1/stretto.1
	@echo "installed: $(DESTDIR)$(PREFIX)/bin/stretto + man1/stretto.1"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/stretto \
	      $(DESTDIR)$(PREFIX)/share/man/man1/stretto.1

# Debug build: -O0 -g -DDEBUG, assertions enabled, no LTO, no strip.
# For stepping through SVF / envelope / scheduler behaviour in gdb.
# Output: synth_debug. The normal build's .o files are untouched
# (separate output names) so `make` and `make debug` can be alternated.
DEBUG_FLAGS  = -O0 -g -DDEBUG -Wall -Wextra -pthread -latomic
DEBUG_OBJS   = $(OBJS:.o=.dbg.o)

%.dbg.o: %.c
	gcc $(DEBUG_FLAGS) -MMD -MP -MF $*.dbg.d -c $< -o $@

synth_debug: $(DEBUG_OBJS)
# $(DEBUG_FLAGS) already carries -pthread -latomic, so the explicit
# literals that earlier Makefile revisions added were redundant.
# $(LIBASOUND) tacks on -lasound (when libasound2-dev is installed)
# because $(DEBUG_OBJS) includes audio_midi_linux.dbg.o.
	gcc $(DEBUG_FLAGS) $(DEBUG_OBJS) -lpulse $(LIBASOUND) -o synth_debug

debug: synth_debug
	@echo "Built: synth_debug (unoptimized, asserts enabled, gdb-friendly)"
	@file synth_debug

.PHONY: all clean size pack test test-unit test-multiseed test-smoke \
        verify coverage golden golden-multiseed play win winpack debug \
        install uninstall

# Pick up the auto-generated header dependencies. The leading '-'
# silences "no such file" when these have not been generated yet
# (first build).
-include $(OBJS:.o=.d)
-include $(WIN_OBJS:.o=.d)
