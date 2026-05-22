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
	rm -f synth synth.packed synth.upx synth.test synth.orig \
	      synth.unpacked synth.xz synth.lto.o synth_xz.h \
	      start.c stub.c \
	      $(GENS) $(HEADERS) *.o

size: synth
	@SIZE=$$(stat -c%s synth); \
	echo "Stripped binary size: $$SIZE bytes (target: $(STRIP_TARGET))"; \
	if [ $$SIZE -gt $(STRIP_TARGET) ]; then \
		echo "WARNING: exceeds $(STRIP_TARGET) byte target"; \
	fi

test: synth
	./tests/test_bitexact.sh

golden: synth
	@mkdir -p golden
	./synth --render 16 /tmp/golden_render.wav
	@sha256sum /tmp/golden_render.wav | awk '{print $$1}' > golden/regression_16s.sha256
	@echo "golden/regression_16s.sha256 updated:"
	@cat golden/regression_16s.sha256

play: synth
	./synth

.PHONY: all clean size pack test golden play
