CFLAGS = -Os -flto -fuse-linker-plugin -ffast-math \
         -ffunction-sections -fdata-sections -fno-plt \
         -fno-asynchronous-unwind-tables -fno-stack-protector \
         -fno-pic -Qn
LDFLAGS = -Wl,--gc-sections -Wl,--build-id=none -Wl,-z,norelro \
          -Wl,--hash-style=sysv -no-pie

all: synth

sin_table.h: gen_sin_table
	./gen_sin_table > sin_table.h

gen_sin_table: gen_sin_table.c
	gcc -O2 gen_sin_table.c -o gen_sin_table -lm

main.o: main.c sin_table.h
	gcc $(CFLAGS) -c main.c -o main.o

synth: main.o
	gcc $(CFLAGS) $(LDFLAGS) main.o -lasound -o synth
	strip -s -R .comment -R .note* synth

clean:
	rm -f synth gen_sin_table sin_table.h *.o

size: synth
	@SIZE=$$(stat -c%s synth); \
	echo "Stripped binary size: $$SIZE bytes"; \
	if [ $$SIZE -gt 16384 ]; then \
		echo "WARNING: exceeds 16 KB target"; \
	fi

test: synth
	./tests/test_bitexact.sh

play: synth
	./synth

.PHONY: all clean size test play
