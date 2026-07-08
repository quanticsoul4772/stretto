# stretto Constitution

Stretto is a tiny native generative ambient music synthesizer in C99. These principles encode the architectural and process commitments the project has already enforced through ~80 PRs; subsequent specs, plans, and tasks must comply.

## Core Principles

### I. Tiny Native Binary (NON-NEGOTIABLE)
Hard size budget: ≤48 KB UPX-packed Windows `.exe` (current 32 KB; last re-measured 2026-06 pre-PR #109, deferred followup), ≤50 KB stripped Linux binary (bumped 2026-07-08 from prior ≤24 KB target; current 43 880 B / ~43 KB measured post-#109/#113 per PR #115 `make size` artifact). CI gates the Windows budget on every PR. Choose minimal-dependency designs; prefer one-file modules over libraries. Features that would push past the budget must justify themselves explicitly or be deferred.

The 24 KB → 50 KB amendment acknowledges that the 003 MIDI-input chain (FR-001..FR-054, ~209 lines of int32_t SPSC ring + CC dispatch + libasound sequencer worker + opt-out + 23 unit tests in `tests/unit/test_midi.c`) is the principled cost of supporting Principle III (Deterministic) + Principle IX (Cross-Platform From Day One) + Principle X (Generative > Random). The ~19 KB growth was the foreseeable cost of cross-platform MIDI input compensation that the PR #108→PR #109→PR #113 chain chose to eat on principle rather than defer.

### II. C99 Only
No C++, no external runtime dependencies beyond libc + libpulse (Linux) / winmm (Windows). Build-time tools (`gen_*_table.c`) are also C99. No code generators outside what already exists. No build system beyond GNU Make.

### III. Deterministic (NON-NEGOTIABLE)
Given `--seed N`, audio output is byte-identical across runs and across the supported build targets (Linux glibc + Windows winmm, both little-endian x86). The runtime engine is integer-only (int16 / int32 / int64) — no `double` or `float` in any synth / voice / mixer / effects module. The build-time table generators (`gen_*_table.c`) use `pow()` / `sin()` / `double`, but their outputs are rounded with `(int)(x + 0.5)` per the deterministic IEEE-754 round-half-to-even contract and committed to headers, so the source-of-truth bytes for every constant are identical regardless of which platform produced the committed `.h`. The WAV writer emits native-endian RIFF (`fwrite(&uint16/uint32, ...)`); little-endian on both supported targets. No clock reads inside the synth, no untracked PRNG sources, no thread-induced ordering. A bit-exact 16-second SHA-256 regression test gates every PR on Linux; a multi-seed integration test catches drift across seeds. Intentional output changes require regenerating goldens in the same PR. (A Windows-side WAV byte-identity runner is not currently in CI — the cross-platform invariant holds by code construction, not by automated cross-platform test.)

### IV. Ambient + Algorithmic Aesthetic
Targeted at long-form listening (10+ minutes). Per-bar variation is fine; per-second jarring change is not. Voices should sound composed, not random. Features earn their place by adding perceptible structure on top of stochastic note selection — the song-section state machine, chord progressions, L-system phrasing, adaptive density, inter-voice listening, and long-term motifs all exist to push the output away from "random noise" toward "intentional music."

### V. Cleanly Modular
Each concern lives in one `.c`/`.h` pair with a one-way dependency direction documented in `ARCHITECTURE.md`. No `extern` declarations across module boundaries, no weak-symbol workarounds, no circular includes. Current shape:

```
main → {wav, audio, ui, gen, effects, voice, arena}
audio_* → {mixer, ui, keys, arena}
wav → {mixer, arena}
mixer → {gen, voice, effects}
keys → {ui, gen, voice, effects}
ui → {voice, gen, effects}
gen → {voice, lsystem, chord_progression, section, density, motif, effects}
voice → {arena, effects}
```

### VI. Test Discipline (NON-NEGOTIABLE)
Every pure-synth module has unit tests and a per-file coverage gate in CI (typically ≥90–95%). Interactive modules (`ui.c`, `keys.c`, `audio_pulse.c`) are explicitly excluded from measurement with rationale (require TTY + audio device). Three integration layers gate audio regressions:
- Bit-exact 16-second SHA-256 (algorithmic drift)
- Multi-seed audio-characteristic bounds (peak/RMS/clip count)
- Live-mode smoke test (segfault / startup regressions)

### VII. No Partial Features
Start it = finish it. No `TODO` comments, no placeholder `(void)` casts marking "for future use," no mock or stub implementations in committed code. If the work doesn't fit in one PR, the cut is wrong — split the scope, not the implementation.

### VIII. Document Why, Not What
Code identifies what it does via naming. Comments explain non-obvious constraints, subtle invariants, workaround context, or magic-number derivations. No filler comments (`/* increment counter */`), no comments describing the obvious. Doc files (`README.md`, `ARCHITECTURE.md`, `CHANGELOG.md`) are refreshed in dedicated `docs/*` PRs whenever code drift exceeds a few merges.

### IX. Cross-Platform From Day One
Linux PulseAudio and Windows waveOut are first-class. CI runs the Linux build + tests + Windows cross-compile + UPX pack on every PR. Platform-specific code lives in module-named files (`audio_pulse.c`, `audio_winmm.c`) or in clearly-delimited `#ifdef _WIN32` blocks (`ui.c` terminal helpers). The synth's audio engine is platform-independent by design.

### X. Generative > Random
The synth must sound intentional. Pure randomness is a tool, not a goal. New generative features earn merge by adding a perceptible musical structure (long-form arc, harmonic motion, melodic memory, voice interaction) — not just another stochastic process.

## Additional Constraints

### Memory model
Single 128 KB static arena (`arena.c`), 8-byte-aligned bump allocator, no `free`. All audio buffers, voice pool, and reverb/delay state allocate from the arena. Per-module static state (Markov tables, ring buffers, etc.) lives in `.bss`. No `malloc`, no dynamic resizing. OOM in the arena is a programmer error and exits the process.

### Build infrastructure
GNU Make with auto-generated header dependencies (`-MMD -MP`). One pattern rule per object-file class (`%.o`, `%.win.o`, `%.dbg.o`, `$(BUILD_COV)/%.o`). Coverage build isolated in `build_cov/` so `make coverage` and `make test-unit` can be alternated without `make clean`.

### Tooling required
- gcc (Linux + MinGW cross for Windows)
- libpulse-dev (Linux)
- UPX (for size-budget packing)
- gcov + bash (for coverage gates)
- python3 + numpy (for the multi-seed audio-characteristic test)

## Development Workflow

1. **Branch per change.** All work lands on `main` via GitHub PRs. No direct commits to `main`.
2. **PR scope: one feature or one refactor.** No mixed-purpose PRs. Refactors are mechanical (bit-exact regression passes with the existing golden); features regenerate golden in the same PR.
3. **Verify locally before pushing:**
   - `make` — Linux build.
   - `make test` — bit-exact regression.
   - `make test-unit` — all per-module tests.
   - `make test-multiseed` — 4-seed audio bounds.
   - `make test-smoke` — live-mode 2s.
   - `make coverage` — per-file gates.
   - `make win && make winpack` — Windows cross-compile + UPX.
4. **CI re-runs the full suite + size budget.** PRs that fail any gate do not merge.
5. **Doc refresh PRs are scheduled, not skipped.** When 5+ feature/refactor PRs accumulate without docs, the next PR is a `docs/*` refresh covering them all.

## Governance

- This constitution supersedes ad-hoc decisions. PR descriptions reference the relevant principle when a tradeoff requires it (e.g., "lowering coverage gate per Principle VI because new code path requires multi-minute render to exercise").
- Amendments are PRs that modify this file and bump the version below.
- Removing a NON-NEGOTIABLE principle requires explicit user approval in the amendment PR.
- All `/speckit-specify` and `/speckit-plan` outputs must declare compliance with each principle or document the exception.

**Version**: 1.1.0 | **Ratified**: 2026-05-23 | **Last Amended**: 2026-07-08
