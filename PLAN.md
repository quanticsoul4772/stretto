# The 64KB Wotja: A Decision-Ready Plan for a Demoscene-Style Generative Music Player on Linux

## TL;DR

- **Build it in C with the ALSA C API, use Karplus-Strong + a 2-pole state-variable filter for synthesis, and let cellular-automaton + Markov-chain rules drive note generation.** That stack is the only combination with proven track record at <100 KB on Linux (ghostsyn, Sointu, 4klang/Clinkster/Oidos all fit this mold), has a trivial WSLg audio path via `/mnt/wslg/PulseServer`, and your demoscene size targets are realistic — Kwarf's `64k-starter` README confirms verbatim: *"A release build (`cargo build --release`) of this project as is will result in a binary size of 28 672 bytes. Packing it with UPX can then reduce it down to 16 384 bytes."* That's an empty Rust shader-demo with a full WaveSabre player+song embedded.
- **Drop Rust and Zig as the primary language, but keep `zig cc` as the build tool.** Even with every size trick applied, johnthagen's `min-sized-rust` repo warns: *"If you want an executable smaller than 20 kilobytes, Rust's string formatting code, core::fmt must be removed. panic=immediate-abort only removes some usages of this code."* A stripped Rust hello-world on x86_64-unknown-linux-gnu starts at ~449 KB and only reaches ~300 KB after `panic=abort` + LTO + `opt-level='z'`. Zig's audio story is `@cImport <alsa/asoundlib.h>` — i.e., you'd be writing C anyway, with worse documentation. **C99 + `gcc -Os -flto` + `strip` + `smol` + `vondehi` is the path the demoscene actually uses today on Linux.**
- **The realistic MVP is a 4–8 voice polyphonic Karplus-Strong + FM ambient player with Markov-driven melodies and Euclidean rhythms, in a ~24–32 KB stripped binary running under a hard 64 KB heap cap via `prlimit --data=65536`.** Stretch goal: <12 KB after smol+vondehi, 16 voices, state-variable filter per voice, cellular-automaton harmonic field. The "art form" is that everything — sine table, exp envelope curves, voice arenas, ring buffer — lives in one pre-allocated `static uint8_t pool[65536]` and a bump allocator.

---

## Key Findings

1. **The demoscene answer to "generate music in <100 KB" is already settled and Linux-portable.** 4klang and its modern fork **Sointu** (Veikko Sariola, MIT-licensed) compile songs to bytecode for a stack-VM synth; the runtime targets 386/amd64/wasm and runs natively on Linux. Sariola's README puts a concrete floor on what's possible: *"a fairly capable synthesis engine can already be fitted in 600 bytes (386, compressed), with another few hundred bytes for the patch and pattern data."* **Ghostsyn** (Juippi/faemiyah, ia32 assembly + C++ tracker, FreeBSD/Linux) is explicitly built for 4 KB Linux intros. **Oidos** (additive synthesis) and **Clinkster** (monolithic, no filters) round out the canonical list, though both ship as 32-bit Windows VST DLLs requiring "linking voodoo" on Linux. **64klang2** is the only one of the big four "made to work on Linux 'by default'" (per the in4k wiki).

2. **Brian Eno / Wotja's "intermorphic" approach maps cleanly to a small C engine.** Wotja's Intermorphic Music Engine (IME) composes notes through 4 inter-dependent rule types — Scale, Harmony, Next-Note, Rhythm — each represented as **probability-weighted arrays** ("Element values are probability weightings", per the Intermorphic tutorials). That's a probabilistic finite state machine over note-degrees inside a scale — exactly a first-order Markov chain with a hand-tuned transition matrix per scale. The whole rule set fits in a few hundred bytes; the generator is a `rand() % weighted_sum` lookup. Wotja itself is the direct descendant of SSEYO Koan, which Brian Eno used to create *Generative Music 1* (1996) — that lineage is your reference aesthetic.

3. **The .kkrieger lesson translates directly to your project.** Per Wikipedia and the GIGAZINE/Giesen retrospective: *"When .kkrieger became playable, it weighed in at 120 KiB and was subsequently shrunk down to 102 KiB. The team then coded their own code analyzer and parsed through the code to ensure maximum efficiency."* The final game shipped at **97 280 bytes** with everything procedural — textures, meshes, music — from a V2 synthesizer fed continuous MIDI. The lesson isn't optimizer flags; it's architecture (Fabian Giesen, "Metaprogramming for madmen", 2012-04-08): *"making it modular, using the right algorithms, storing data in the right way, and so on."* That kind of bespoke tooling is what makes the size budget work.

4. **WSL2 audio is solved as of Windows 11 + WSLg.** WSLg auto-provisions a PulseAudio socket at `/mnt/wslg/PulseServer` and sets `$PULSE_SERVER` for you. The pattern that works for native ALSA code is to ship a tiny `~/.asoundrc` that routes `pcm.!default` through the `pulse` plugin — confirmed by multiple WSL2 Ubuntu walkthroughs (zenn.dev, microsoft/WSL discussion #9624). You write straight `snd_pcm_*` ALSA code as if on bare Linux; `libasound2-plugins` translates `default → pulse → /mnt/wslg/PulseServer → Windows audio`.

5. **Size-coding linker tooling has narrowed to one realistic path on modern Linux.** From porocyon (author of `smol` and `vondehi`) on pouet.net thread #12948 ("State of art for 64bit EXE/ELF compression"): *"best in town is currently epoqe's cold, but it's not publicly available. The other best option is probably to use smol and oneKpaq … There's no 64k-focussed (de)compressor (like Squishy or kkrunchy) for Linux."* For 16–64 KB binaries the workflow is `smol` (linker) + `vondehi` (memfd_create-based in-memory decompressor) or `xz` + tiny shell wrapper. UPX still works but produces fatter output. **Crucially: 32-bit ELF is dead** ("there are no libraries"); target x86_64 only.

6. **Bytebeat sets the absolute floor for "music from code" but is too primitive for Wotja-style ambient.** Viznut's one-liners like `(t*(t>>5|t>>8))>>(t>>16)` produce 8-bit unsigned PCM at 8 kHz from a `main()` of <100 bytes. It proves the principle but cannot do polyphony, envelopes, or evolving rules. Use it as a **phase-0 sanity check** for your audio pipeline ("does sound come out of the speakers?"), then build a real synth.

7. **Karplus-Strong is the highest sound-quality-per-byte choice for ambient synthesis.** A complete plucked-string voice is ~10 lines of C: noise-initialized circular buffer, average-of-two-samples + feedback coefficient (`0.996`), output the head, write the average back. With one buffer per voice (sized to `SAMPLE_RATE/freq`, ~100–500 bytes per voice depending on pitch), 8-voice polyphony fits in <4 KB of state. It's why many 4k intros use it implicitly.

8. **There is a genuine unfilled niche here.** Across ~20 searches I could not locate a published, Linux-native, sub-100 KB, *generative* (not tracker-driven) synthesizer. Sointu/4klang/WaveSabre play *composed* songs. AMY (shorepine), syndig (mmitch), and ambiantiseur (MrBlueXav) are small synths without generative layers. Celltone, music-of-life, and assorted Markov repos are *generators* without compact synths. **A "tiny Wotja for Linux" doesn't exist** — this project would be the first.

---

## Details

### A) Language choice: pick C, use Zig as the build system

**Recommendation: plain C99, compiled with `gcc -Os -flto`, linked with `smol`, no libc beyond what `libasound` drags in.**

| Axis | C | Rust (no_std, no tokio) | Zig | x86_64 asm |
|------|---|--------------------------|-----|------------|
| Hello-world ELF (vanilla) | ~17 KB (per Lunar Journal walkthrough) → 640 B with custom linker script | ~449 KB stripped on x86_64-unknown-linux-gnu (`min-sized-rust`); ~300 KB with `panic=abort`+LTO+`opt-level=z` | ~6–10 KB stripped (per paiml.com: "300KB Zig vs 2.8MB Rust" HTTP server) | 45 B (Muppetlabs Teensy ELF) |
| Audio library access | `#include <alsa/asoundlib.h>` — done | `alsa` crate exists but pulls libc + bitflags | `@cImport({@cInclude("alsa/asoundlib.h")})` — same as C | inline syscalls |
| Existing demoscene precedent | 4klang, Clinkster, Oidos, ghostsyn, V2 | None known at <64 KB | Almost none in production size-coding | 4klang, ghostsyn audio core |
| Cross-compile from WSL | trivial | trivial | best-in-class | trivial |
| Docs for size optimization | Muppetlabs, in4k wiki, sizecoding.org | sparse | sparse | abundant but x86-only |

**Why not Rust:** even `#![no_std]` + `panic = "abort"` + `lto = "fat"` doesn't escape `core::fmt`. From johnthagen/min-sized-rust: *"If you want an executable smaller than 20 kilobytes, Rust's string formatting code, core::fmt must be removed. panic=immediate-abort only removes some usages of this code."* Building Rust without `core` is possible but unproductive for a 4-week project.

**Why not Zig as primary:** the audio binding story is "use @cImport" — i.e., you're writing C with extra syntax. **Use `zig cc` as your C compiler** (it bundles musl + clang and is a faster install on WSL than a full cross toolchain), but write the source in C.

**Compiler flags (verified from Lunar Journal and the in4k wiki):**

```makefile
CFLAGS = -Os -flto -fuse-linker-plugin -ffast-math \
         -ffunction-sections -fdata-sections \
         -fno-plt -fno-asynchronous-unwind-tables \
         -fno-stack-protector -fno-pic -Qn \
         -nostartfiles
LDFLAGS = -Wl,--gc-sections -Wl,--build-id=none -Wl,-z,norelro \
          -Wl,--hash-style=sysv
```

After `gcc`: `strip -s -R .comment -R .note*`, then `sstrip` (Madore/Bercot's superstrip, *"strip an ELF executable of all unmapped information"*), then `smol` to crunch ELF headers, then `vondehi` for in-memory `memfd_create`-based decompression. The Lunar Journal walkthrough shows hello-world dropping ~17 KB (default gcc) → ~640 bytes (custom linker script, `-static`, `-Os`, `-nostdlib`). With `libasound` linked dynamically (which you want — it's on every Ubuntu box), expect a sub-8 KB *uncompressed* binary for the bare engine, growing to 20–40 KB with a real synth + generative logic.

### B) Audio output: ALSA `libasound` is the only sensible choice

**Recommendation: ALSA `snd_pcm_*` C API, `"default"` device, blocking writes from a single audio thread.**

| Backend | Minimum dep | LOC for hello-tone | Verdict |
|---------|-------------|---------------------|---------|
| **ALSA (`libasound`)** | libasound2 (preinstalled on Ubuntu) | ~60 lines (the ghedo/963382 gist) | **Yes.** Works under WSLg via the pulse plugin. |
| **PipeWire native API** | libpipewire-0.3 | ~150 lines (tutorial4.c, with `pw_main_loop` + `pw_stream` + `spa_pod_builder`) | No — heavier API surface, more `spa_pod` boilerplate, not installed by default on Ubuntu LTS. |
| **JACK** | libjack | ~80 lines | No — assumes a JACK server. |
| **PulseAudio simple API** | libpulse-simple | ~30 lines | Tempting (smallest LOC), but WSLg already proxies ALSA writes to Pulse — no win. |
| **Raw `/dev/dsp`** | OSS | ~15 lines | **Dead.** No OSS kernel module on modern Ubuntu. |

**Minimal viable ALSA in C** (adapted from the ghedo gist and Alex Via's tutorial):

```c
#include <alsa/asoundlib.h>
#define SR 44100
int main(void) {
    snd_pcm_t *pcm;
    snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                       1, SR, 1, 100000);   /* 100 ms latency */
    int16_t buf[1024];
    uint32_t phase = 0;
    for (;;) {
        for (int i = 0; i < 1024; i++) {
            buf[i] = sin_table[phase >> 22];   /* 1024-entry table */
            phase += PHASE_INC_A440;
        }
        snd_pcm_writei(pcm, buf, 1024);
    }
}
```

Link with `-lasound`. On WSL Ubuntu 24.04 ensure `libasound2-plugins` is installed and `~/.asoundrc` contains the pulse passthrough (`pcm.!default { type pulse }`). That single `snd_pcm_writei` call handles all PipeWire/PulseAudio/Pro-Audio routing transparently.

### C) Demoscene synth references to study (ranked for porting)

1. **Sointu** (https://github.com/vsariola/sointu) — **best architectural reference.** The synth core is a **stack-based virtual machine** that executes opcodes (`MUL`, `OSC`, `ENV`, `FILTER`, etc.) on a tiny floating-point stack. The song is bytecode; the player is ~600 bytes (386, compressed) per the README. Sariola's design philosophy in the README is gold: *"Size first, speed second. Speed will only considered if the situation becomes untolerable."* And: *"Instead of prematurely adding %ifdef toggles to optimize away unused features, start with the most advanced featureset and see if you can implement it in a generalized way."* **Port the VM idea**, not the assembly. A C version of a stack-VM synth in 4–8 KB is very achievable.
2. **ghostsyn** (https://github.com/Juippi/ghostsyn) — directly Linux/FreeBSD-native; subtractive synthesis, ia32 assembly audio core, C++17 tracker UI. Read the asm to see exactly what survives at 4 KB.
3. **WaveSabre** (https://github.com/logicomacorp/WaveSabre) — 64K intro synth. Per Kwarf/64k-starter README: *"A release build of this project as is will result in a binary size of 28 672 bytes. Packing it with UPX can then reduce it down to 16 384 bytes."* That includes a full WaveSabre player + song. The ferris TG2013 deck and the pouet thread document the C++ architecture; the "make it fast, then make it fast" philosophy means per-sample work is OK at 64 KB scale.
4. **Farbrausch V2 / .kkrieger** (https://github.com/farbrausch/fr_public) — V2 is MIDI-driven, 16-channel multitimbral, with reverb/delay/comp. Wikipedia: *"The game music and sounds are produced by a multifunctional synthesizer called V2, which is fed a continuous stream of MIDI data. The synthesizer then produces the music in real time."* For your project, take the **MIDI-event-stream interface** between generator and synth — a clean separation that lets you swap synthesis or generation independently. C++ Linux port at https://github.com/murkymark/v2synth.
5. **Oidos** — additive synthesis with ~200 sines per voice; gorgeous but precomputes a frame in ~seconds at startup. Probably too CPU-heavy for your purposes; read for "additive at 4K" insight.
6. **PoroCYon's `4klang-linux`** (https://gitlab.com/PoroCYon/4klang-linux) — concrete glue code for 4klang/Clinkster/Oidos/V2 on Linux; shows the static-binary, embedded-WAV-fallback patterns.

**Not recommended to port:** werkkzeug3 / .kkrieger itself (Windows-DirectX-bound; the value is in the writeups).

### D) Generative algorithms ranked by code size and musical interest

| Algorithm | Code size (C) | Musical interest | Memory | Verdict |
|-----------|---------------|-------------------|--------|---------|
| **Bytebeat one-liners** | <50 B | Glitchy, novelty | 0 | Phase-0 audio sanity check only |
| **Euclidean rhythms (Bjorklund)** | ~50–100 lines, or 0 lines with precomputed lookup (`emcconville/static-euclidean-rhythm`) | Excellent for percussion + ostinatos | <2 KB total ROM | **Yes — rhythm layer** |
| **Markov chains (1st order)** | ~30 lines (matrix + weighted random) | Maps directly to Wotja's "Next Note Rule" probability weights | (N×N) bytes per matrix, 7×7 = 49 B for diatonic | **Yes — primary melody generator** |
| **Cellular automata (Rule 30/110)** | ~10 lines | Good for harmonic fields, evolving chord tones | 1 byte per cell-row | **Yes — chord-progression / drone layer** |
| **L-systems** | ~50 lines (production rules + turtle) | Hierarchical phrases; Prusinkiewicz ICMC '86: *"a lookup table which allows for specifying an arbitrary mapping of y coordinates into note pitches"* | Stack-bounded by recursion | Optional — secondary phrase generator |
| **Probabilistic FSM (Wotja IME-style)** | ~40 lines | Direct mapping of intermorphic rules | <500 B for 4 rule types | **Yes — Wotja's actual model** |
| **Genetic algorithms / NNs** | KB+ | Variable | KB+ | No — out of budget |

**Recommended generative stack:**
1. **Cellular automaton** (Rule 110, 32-bit-wide row, 64 history rows = 256 bytes) drives **chord-tone selection** — picks 1–3 active scale degrees per bar from the current row's population.
2. **Markov chain** over 7 scale degrees (49-byte 7×7 weight table) generates the **melody line** within the chord-allowed degrees.
3. **Euclidean rhythm** E(k,16) for each voice's **rhythmic pattern** (k pulses in 16 steps), with `k` modulated slowly by another CA.
4. **Stochastic mutation** every N bars: re-roll one Markov weight, one CA seed bit, one Euclidean `k`. This is Wotja's "Mutate" feature reduced to 20 lines of C.

This is structurally identical to Wotja's IME (Scale Rule + Harmony Rule + Next Note Rule + Rhythm Rule, each a probability-weighted table) but uses CA-driven mutation for the long-term evolution that makes Eno-style generative music feel alive. Worth & Stepney's review of L-systems for music (CCRMA, 2005) warns: *"At longer derivations, the melodies begin to get dull: the same bit of music is repeating continually, albeit normally transposed in some way. Stochastic L-systems may [help]."* That's why I rank Markov+CA as primary and L-systems as optional.

### E) DSP techniques for size-constrained synthesis

| Method | Code size | Quality | Use for |
|--------|-----------|---------|---------|
| **Karplus-Strong** | ~10 LOC, ~100–500 B per voice state | Excellent for plucked/strings/bells | **Primary voice for ambient texture** |
| **2-op FM (DX7-style)** | ~15 LOC, ~20 B per voice state | Excellent for bells, basses, electric piano | **Secondary voice for chord pads** |
| **Wavetable + linear interp** | ~10 LOC + table size | Good, classic | If you have ROM budget for tables |
| **Subtractive (saw + SVF Chamberlin)** | ~30 LOC (osc + SVF) | Classic analog | If you want filter sweeps |
| **Additive (Oidos-style)** | ~5 LOC per partial × ~200 partials | Beautiful but CPU-heavy | Skip — too expensive |
| **Granular** | KB+ | Texture | Skip — too complex |

**Recommended:** dual-engine — Karplus-Strong for melody/pluck voices, 2-op FM for sustained pads. Both share a single 1024-entry sine table (2 KB), one ADSR envelope LUT (1 KB exponential curve), and the bump-allocator-managed voice arena.

**Fixed-point vs float:** stick with `int16_t` audio samples and `int32_t` intermediate, but **use floats for control rate** (envelopes, LFOs, pitch). On x86_64 SSE2 is mandatory anyway, so float is "free"; using fixed-point would just complicate the code without saving bytes. The 4klang/Sointu lineage uses the x87 FPU stack precisely because per-sample float math is cheap on hardware that hasn't existed since 2007 — on amd64, SSE2 floats are equivalent.

**Sine table:** 1024 entries × 2 bytes = 2 KB, `int16_t`; quarter-wave symmetry can shrink it to 256 entries (512 B) with 2 extra instructions. **Don't** generate at startup — let the linker embed it; `gcc` puts it in `.rodata` which doesn't count against your heap cap.

**Exponential envelope curve:** 256-entry `uint8_t` LUT (256 B), exponential from 0→255 with τ ≈ 64. Use it for ADSR attack/release shape; the envelope state per voice is a single 16.16 fixed-point counter.

### F) Memory architecture under 64 KB heap cap

**Layout (single static arena, all sizes total ≤ 65 536 B):**

```c
#define HEAP_BYTES        65536
#define N_VOICES          8
#define KS_BUF_SAMPLES    512    /* enough for ~86 Hz fundamental at 44.1 kHz */
#define RING_FRAMES       4096   /* ~93 ms at 44.1 kHz, S16 mono */

static uint8_t pool[HEAP_BYTES] __attribute__((aligned(64)));
static uint8_t *bump = pool;

static void *arena_alloc(size_t n) {
    void *p = bump;
    bump += (n + 7) & ~7;        /* 8-byte align */
    return p;
}

typedef struct {
    int16_t   ks_buf[KS_BUF_SAMPLES];   /* 1024 B */
    uint32_t  ks_idx;                    /* 4 B */
    uint16_t  ks_len;                    /* 2 B; tuned per note */
    uint16_t  env_state;                 /* 2 B; 8.8 fixed */
    uint8_t   env_phase;                 /* 1 B; ADSR phase */
    uint8_t   note;                      /* 1 B; MIDI 0..127 */
    int16_t   svf_lp, svf_bp;            /* 4 B SVF state */
    /* total: ~1040 B per voice, rounded to 1048 B */
} Voice;
```

**Budget table:**

| Region | Size | Cumulative |
|--------|------|------------|
| 8 voices × 1 KB | 8 192 B | 8 192 B |
| Sine table (`int16_t[1024]`) | 2 048 B | 10 240 B |
| Env curve LUT (`uint8_t[256]`) | 256 B | 10 496 B |
| Markov transition matrix (7×7 `uint8_t`) | 49 B | 10 545 B |
| Euclidean patterns (4 voices × 16 bits) | 8 B | 10 553 B |
| CA history (64 rows × 4 bytes) | 256 B | 10 809 B |
| ALSA period buffer (1024 frames × 2 ch × 2 B) | 4 096 B | 14 905 B |
| Mix accumulator (1024 × `int32_t`) | 4 096 B | 19 001 B |
| Scratch / temp | 4 096 B | 23 097 B |
| **Headroom for future features** | **~42 KB** | **65 536 B** |

**Polyphony budget:** 8 voices is comfortable; 16 is feasible with voice-stealing (timestamp-based) and shared Karplus-Strong buffers. 32 voices would force you to drop Karplus-Strong for FM only.

**Enforce the cap at runtime** with `prlimit --as=131072 --data=65536 ./synth` (the AS limit must include `.text` + libs mapped in) or set `RLIMIT_DATA` via `setrlimit()` early in `main()`.

**No `malloc`.** The entire program statically allocates `pool[]` and bumps. No `free`. Voice "deallocation" is just clearing an "active" bit.

### G) Build/dev workflow on WSL Ubuntu

**Audio path:** Windows 11 + recent WSL2 ships **WSLg**, which auto-provisions a PulseAudio socket at `/mnt/wslg/PulseServer`. From the WSL2 Voice-Mode docs: *"In WSL2, there are no native ALSA cards; WSLg exposes audio via PulseAudio's RDP devices (RDPSource, RDPSink). ~/.asoundrc makes ALSA's default device a Pulse shim."* Setup once:

```bash
sudo apt install -y libasound2-dev libasound2-plugins alsa-utils pulseaudio-utils
cat > ~/.asoundrc <<'EOF'
pcm.!default { type pulse }
ctl.!default { type pulse }
EOF
paplay /usr/share/sounds/alsa/Front_Left.wav   # sanity check
```

If `/mnt/wslg/PulseServer` is missing, run `wsl --update` from an elevated PowerShell. **Don't** install a Windows PulseAudio server — that's the 2020-era workaround; WSLg made it obsolete on Win11.

**Toolchain:**

```bash
sudo apt install gcc clang make binutils elfkickers   # elfkickers provides sstrip
# Optional but recommended: zig cc as drop-in C compiler with bundled musl
wget https://ziglang.org/download/0.13.0/zig-linux-x86_64-0.13.0.tar.xz
```

**Size-optimization pipeline:**

1. `gcc -Os -flto -ffunction-sections -fdata-sections -Wl,--gc-sections -nostartfiles main.c -lasound -o synth`
2. `strip -s -R .comment -R .note -R .note.gnu.build-id synth`
3. `sstrip synth` (from berney/superstrip or elfkickers)
4. `smol pack synth -o synth.packed` (https://github.com/Shizmob/smol)
5. (For under-16 KB) `vondehi synth.packed`

**Debugging tiny audio:**
- `perf record -F 1000 -g ./synth` for CPU profile.
- `samply record ./synth`, then open `profile.json` in Firefox profiler — works great in WSL.
- For waveform inspection: pipe raw audio to a file (`./synth > out.raw`) and load in Audacity (Import → Raw Data, S16 LE mono 44100). Cheaper than building an oscilloscope.
- **Render-to-WAV mode** (`./synth --render 60s out.wav`) is a one-day investment that pays back 100×: unit tests can diff WAVs against golden files, and you can listen to renders without WSL audio at all.

### H) Comparable existing projects to learn from

- **shorepine/amy** (https://github.com/shorepine/amy) — *"a fast and small music synthesizer library written in C ... can be embedded into almost any program, architecture or microcontroller."* Juno-6 VA + DX7 FM + additive + drum machine + breakpoint synth, MIDI-driven. Not generative on its own but the cleanest "industrial-quality" C synth core you can build a generator on top of.
- **mmitch/syndig** — *"a simple software synthesizer in C"*, 8-sample wavelets + ADSR, MIDI-controlled. Very small codebase, good reading material.
- **MrBlueXav/ambiantiseur** — STM32F4 but builds on Linux; *"a very basic synthesizer which can produce semi-randomly generated musical sequences with a few effects (delay and phaser). The tone generator is an anti-aliased minBLEP sawtooth oscillator with light vibrato."* Closest existing thing to your MVP.
- **andreasjansson/Celltone** — *"programming language for generative music composition using cellular automata"*; reference implementation of CA→music mapping.
- **plhosk/music-of-life** — Game-of-Life → Web Audio. JS, but the algorithm is the point.
- **kstar/markov-music**, **Zunawe/markov-chain-music-generator**, **mnagel/markov** — Markov chain MIDI generators (algorithm, not synth).
- **viznut bytebeat** (http://viznut.fi/texts-en/bytebeat_deep_analysis.html) + **IBNIZ** — absolute-minimum baseline.
- **Juippi/ghostsyn** + **vsariola/sointu** — the demoscene anchors.
- **poetaster/supercollider-lsystems** — L-system composition in SuperCollider (large dep, but illustrative).

**The unfilled niche:** a published, Linux-native, sub-100 KB, *generative* synthesizer doesn't exist. Building one is genuinely novel.

### I) Realistic project scope — three-phase plan

**Phase 1 (Week 1–2): "Make sound come out."** ~8 KB binary.
- ALSA `snd_pcm_writei` loop, single sine oscillator via 1024-entry table, hard-coded A440 forever.
- `prlimit --data=65536 ./synth` works.
- WAV-render mode (`--render`) added on day 2.
- **Threshold:** a 30-second test tone matches a reference WAV bit-exactly.

**Phase 2 (Week 3–4): "Make it a synth."** ~16 KB binary.
- Voice struct, 4-voice polyphony, Karplus-Strong + 2-op FM voices.
- ADSR envelope from the exp LUT.
- Hard-coded C-major arpeggio for testing; verify polyphony budget under cap.
- **Threshold:** 4-voice polyphony, binary ≤ 24 KB, runtime resident set ≤ 80 KB total (`/proc/PID/status` VmRSS).

**Phase 3 (Week 5–6): "Make it generative."** ~24–32 KB binary — **this is the MVP.**
- Markov chain melody (7 scale degrees, weighted transition matrix).
- Euclidean rhythm per voice (precomputed lookup table).
- Rule-110 CA chooses active scale degrees per bar.
- Stochastic mutation every 16 bars.
- **Threshold:** plays evolving music for ≥ 1 hour without obvious repetition; binary ≤ 32 KB; heap cap holds.

**Phase 4 (Stretch, Week 7–8): "Make it small."** Target <16 KB after smol+vondehi.
- Drop libasound dynamic linking → call `ioctl(SNDRV_PCM_IOCTL_WRITEI_FRAMES)` directly (saves a few KB, requires reading the kernel ABI).
- 8 voices, state-variable filter per voice, second CA layer for harmonic field.
- Custom code-coverage analyzer (kkrieger-style) to eliminate dead branches.
- **Threshold to call it done:** final packed binary ≤ 12 KB, runs from `memfd_create` decompression.

**Phase 5 (Long stretch): UI.**
- ASCII oscilloscope in the terminal (`printf("\x1b[H"); print_block_chars(buf, 80, 24);`).
- Keyboard control: space = mutate now, +/- = tempo, q = quit.
- **Don't** use ncurses (~150 KB). Hand-roll VT100 escape sequences (~50 LOC).

### J) UI considerations

1. **No UI for v1.** CLI args (`--tempo 90 --scale dorian --seed 0xCAFE --voices 8`) + Ctrl-C. Demoscene-honest; saves bytes for synthesis.
2. **Phase 4 minimal UI:** raw stdin one-key controls + ANSI escape oscilloscope. ~200 bytes of code.
3. **Hard "no":**
   - **ncurses** (~150 KB shared lib) — out of budget.
   - **Embedded HTTP server** — libmicrohttpd or hand-rolled `socket()` code (~5 KB minimum); fun but blows the spirit of the constraint.
   - **ImGui** — fine project, wrong project; minimum binary with imgui+SDL is ~500 KB.
   - **Framebuffer drawing** — requires opening `/dev/fb0`, root or `video` group, no WSLg support.

If you absolutely want graphics, render directly to a PPM file every N samples and `xdg-open` it; or pipe to `ffplay` via stdout. Both cost zero binary bytes.

---

## Recommendations (staged, with thresholds that change the plan)

### Week 1: prove the audio pipeline.
- Install `libasound2-dev`, `libasound2-plugins`, write `~/.asoundrc` with the pulse passthrough.
- Compile and run the ghedo/963382 gist or Alex Via's tutorial, modified to play a sine wave from your own 1024-entry LUT.
- **Threshold to advance:** sine plays cleanly on speakers; binary ≤ 16 KB stripped; `prlimit --data=65536` works.

### Week 2–3: implement Voice arena, Karplus-Strong, FM, ADSR.
- Static `pool[65536]` + bump allocator. No `malloc`.
- 4 voices, hard-coded arpeggio.
- WAV render mode for regression testing.
- **Threshold to advance:** 4-voice polyphony, binary ≤ 24 KB, runtime VmRSS ≤ 80 KB total.

### Week 4–5: implement generative layer.
- Markov 7×7 transition table for each of 3 scales (Dorian, Lydian, Phrygian).
- Bjorklund-precomputed Euclidean rhythm table for E(1..15, 16).
- Rule 110 CA, 32-bit-wide, history of 64 rows.
- Mutation every 16 bars: re-roll one Markov weight, one CA bit.
- **Threshold to advance:** plays evolving music for ≥ 1 hour without obvious repetition.

### Week 6–8: size optimization (only if you enjoy this).
- Switch from libasound dynamic link to a tiny dlopen-style stub that resolves only the 5 ALSA symbols you actually use.
- Pack with `smol`, then `vondehi`.
- Hand-write a linker script.
- **Threshold to call it done:** final packed binary ≤ 12 KB, runs from `memfd_create` decompression.

### Long-term thresholds that change the recommendation:
- **If generative output too predictable after 30 min:** add a second-order Markov chain (state = pair of last two notes) at cost of ~350 bytes; or layer an L-system on top to produce hierarchical phrase structure (Worth & Stepney note this is the standard fix for "long derivations get dull").
- **If 64 KB heap proves trivially easy:** drop to 32 KB. Real demosceners would.
- **If 64 KB too tight for reverb:** keep reverb out — your "ambient feel" should come from slow Karplus-Strong decay + voice overlap, not from a Schroeder reverb (~4 KB of state for a halfway-decent one).
- **If you want to push to a 4K intro:** drop Karplus-Strong (the buffers are the largest line item), switch to pure FM, port Sointu's stack-VM design verbatim, target <4 KB compressed.

---

## Caveats

1. **Binary size numbers in published demoscene productions are usually *compressed* sizes after Crinkler/kkrunchy/smol+vondehi.** WaveSabre's "64K intro" means 65 536 B *compressed* on disk; the decompressed working set is larger. When you set your own target, decide whether you're benchmarking compressed-on-disk or RSS-at-runtime.
2. **WSLg audio adds ~10–30 ms of latency** over the Windows audio stack. Fine for an ambient generator (you set ALSA buffer to 100 ms anyway), unacceptable for live performance.
3. **The `prlimit --data=65536` cap counts only the `brk()`/`sbrk()` heap.** If you use `mmap` (which `malloc` does for large allocations, and which `libasound` may use internally for the PCM ring buffer), you need `--as` (address space) limits — and those must include `.text` + every shared library mapped. Measure carefully; the cap that matches "demoscene 64KB heap" is `RLIMIT_DATA`, and you must use static allocation to honor its spirit.
4. **32-bit ELF is dead on modern Linux.** Per porocyon: *"32-bit ELFs are a dead end, as there are no libraries."* Several demoscene synths (Clinkster, Oidos, 4klang DLLs) are 32-bit only and require multilib + linking workarounds. Target x86_64 exclusively.
5. **No measured Zig+ALSA binary-size data exists publicly.** My recommendation against Zig as primary is based on the Rust comparison + Zig audio binding situation (which is `@cImport`, i.e. C with extra syntax). If you're already comfortable in Zig, binary size will be within 10% of equivalent C. Don't pick Rust.
6. **Karplus-Strong sounds gorgeous on plucks but cannot do brass, vocals, or any sustained timbre that isn't string-like.** This is why you also need FM voices. If your aesthetic target is purely ambient drones, 2-op FM with very slow attacks + lots of detune across 8 voices does most of the work; you can drop Karplus-Strong and save ~1 KB per voice.
7. **The Worth & Stepney critique of L-systems for music is real:** *"at longer derivations, the melodies begin to get dull: the same bit of music is repeating continually, albeit normally transposed in some way."* Use L-systems for *phrase structure* (10–30 second loops), not for the master generator. Stochastic L-systems or Markov-driven rule selection are the standard workarounds.
8. **The Sointu "600 bytes" figure is for the compiled bytecode VM core in 386 assembly, compressed.** A C reimplementation will be larger (perhaps 2–4 KB uncompressed) but still well within your budget. Don't anchor on 600 B as a C target.
9. **WaveSabre Linux support is limited.** The 64k-starter and most published 64K intros target Windows. Porting the synth core to Linux is feasible (Tunefish 4, ghostsyn, sointu prove it) but not a one-evening exercise.
10. **The "tiny Wotja for Linux" gap may exist because the audience is tiny.** Wotja itself is a commercial mobile/desktop app from a 30-year-old company. Your project is an art piece, not a market opportunity — set expectations accordingly. The reward is the craft.