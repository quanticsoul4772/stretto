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
OBJS    = arena.o voice.o gen.o main.o

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
WIN_OBJS    = arena.win.o voice.win.o gen.win.o main.win.o

arena.win.o: arena.c arena.h
	$(WIN_CC) $(WIN_CFLAGS) -c arena.c -o arena.win.o
voice.win.o: voice.c voice.h arena.h $(HEADERS)
	$(WIN_CC) $(WIN_CFLAGS) -c voice.c -o voice.win.o
gen.win.o: gen.c gen.h voice.h euclid_table.h
	$(WIN_CC) $(WIN_CFLAGS) -c gen.c -o gen.win.o
main.win.o: main.c arena.h voice.h gen.h
	$(WIN_CC) $(WIN_CFLAGS) -c main.c -o main.win.o

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

arena.o: arena.c arena.h
	gcc $(CFLAGS) -c arena.c -o arena.o
voice.o: voice.c voice.h arena.h $(HEADERS)
	gcc $(CFLAGS) -c voice.c -o voice.o
gen.o: gen.c gen.h voice.h euclid_table.h
	gcc $(CFLAGS) -c gen.c -o gen.o
main.o: main.c arena.h voice.h gen.h
	gcc $(CFLAGS) -c main.c -o main.o

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
	rm -f synth synth.packed stretto.exe stretto.packed.exe \
	      synth_cov cov_*.o *.gcda *.gcno *.gcov \
	      tests/unit/test_arena tests/unit/test_voice \
	      tests/unit/test_gen tests/unit/test_effects \
	      tests/unit/*.cov \
	      $(GENS) $(HEADERS) *.o *.win.o

size: synth
	@SIZE=$$(stat -c%s synth); \
	echo "Stripped binary size: $$SIZE bytes (target: $(STRIP_TARGET))"; \
	if [ $$SIZE -gt $(STRIP_TARGET) ]; then \
		echo "WARNING: exceeds $(STRIP_TARGET) byte target"; \
	fi

test: synth
	./tests/test_bitexact.sh

# Unit tests. Each tests/unit/test_*.c links against the existing
# arena.o + voice.o + gen.o and runs as a standalone binary.
UNIT_TEST_SRCS = $(wildcard tests/unit/test_*.c)
UNIT_TEST_BINS = $(UNIT_TEST_SRCS:.c=)

tests/unit/test_%: tests/unit/test_%.c tests/unit/test.h $(HEADERS) arena.o voice.o gen.o
	gcc -O2 -Wall -no-pie -Itests/unit $< arena.o voice.o gen.o -o $@ -lm

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

# Coverage build: instrumented compile of the synth + each unit test,
# run them, then print per-file line-coverage percentages. gcov
# expects the .gcno files to be named after the source (arena.gcno
# for arena.c) which only happens when the .o is also named that way.
# So the coverage build uses the same .o names as the regular build;
# `make clean` first if you mix them.
COV_FLAGS = -O0 -g -fprofile-arcs -ftest-coverage
coverage:
	@echo "=== rebuilding instrumented ==="
	@rm -f *.gcda *.gcno *.gcov *.o synth_cov tests/unit/*.cov
	gcc $(COV_FLAGS) -c arena.c -o arena.o
	gcc $(COV_FLAGS) -c voice.c -o voice.o
	gcc $(COV_FLAGS) -c gen.c   -o gen.o
	gcc $(COV_FLAGS) -c main.c  -o main.o
	gcc $(COV_FLAGS) arena.o voice.o gen.o main.o -lpulse -o synth_cov
	@echo "=== render-mode regression ==="
	./synth_cov --render 16 /tmp/cov_render.wav --seed 0 >/dev/null
	@echo "=== unit suite ==="
	@for t in $(UNIT_TEST_SRCS); do \
		base=$${t%.c}; \
		gcc $(COV_FLAGS) -no-pie -Itests/unit $$t arena.o voice.o gen.o \
		    -o $$base.cov -lm; \
		./$$base.cov >/dev/null || true; \
	done
	@echo "=== per-file line coverage ==="
	@gcov -n arena.c voice.c gen.c main.c 2>/dev/null | \
		awk '/^File/ {sub(/[\x27]/,"",$$2); sub(/[\x27]/,"",$$2); f=$$2} \
		     /^Lines/ {sub(/Lines executed:/,""); print f": "$$0}' | \
		grep "\.c"

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
