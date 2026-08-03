# stretto Constitution

Stretto is a tiny native generative ambient music synthesizer in C99 and Zig. These principles encode the architectural and process commitments the project has already enforced through ~80 PRs; subsequent specs, plans, and tasks must comply.

## Core Principles

### I. Tiny Native Binary (NON-NEGOTIABLE)
Hard size budget: ≤48 KB UPX-packed Windows `.exe` (current 43 520 B / ~42.5 KB measured 2026-07-19 per CI run 29211125164 on `c7db9fc`; was recorded as 38 KB post-#117), ≤34 KB UPX-packed Linux binary (raised 30 KB → 34 KB by the v1.4.0 amendment, 2026-08-03, so the 005 Zig port can finish; current 30 664 B measured 2026-08-03 per CI run 30775952889 with five modules in Zig; cap originally added 2026-07-08 per v1.2.0; Makefile `PACK_TARGET = 34816` enforces this cap), ≤56 KB stripped Linux binary (raised 50 KB → 56 KB by the v1.4.0 amendment, 2026-08-03, alongside the packed cap; current 49 976 B measured 2026-08-03 per CI run 30775952889; bumped to 50 KB on 2026-07-08 from a prior ≤24 KB target per v1.1.0; Makefile `STRIP_TARGET = 57344` enforces this cap). CI gates the Windows budget on every PR. Choose minimal-dependency designs; prefer one-file modules over libraries. Features that would push past the budget must justify themselves explicitly or be deferred.

**Measurement refresh — nine modules (v1.4.2, 2026-08-03).** `arena` was ported at PR #204, after v1.4.1 had already declared the port complete at eight. Its exclusion rationale had expired: arena was held back to preserve the ASan redzone around `pool`, on the grounds that the writes it catches come from `effects` and `voice` — both of which became Zig, and therefore uninstrumented, at PRs #197 and #199.

The 005 port is now: `density`, `chord_progression`, `motif`, `section`, `lsystem`, `effects`, `voice`, `gen` and `arena` in Zig; `mixer`, `wav`, `ui`, `keys`, `main`, the audio backends and the five table generators in C.

Measured on `main`, CI run 30784977189:

| | measured | cap | headroom |
|---|---|---|---|
| Linux stripped | 52 600 B | 57 344 B | 4 744 B / **8.3 %** |
| Linux packed | 31 728 B | 34 816 B | 3 088 B / **9.7 %** |
| Windows packed | 45 056 B | 49 152 B | 4 096 B / **9.1 %** |

**Page-cliff headroom is 324 B against a 256 B advisory**, down from 484 before arena. The caps still hold ~9 % each, but the cliff is now the operative constraint and is within 68 B of the advisory threshold. Any feature that crosses the next 4 KB boundary costs 4 096 B stripped whatever the caps report.

**Measurement refresh — the port is complete (v1.4.1, 2026-08-03).** The 005 Zig port finished at PR #200 at eight modules: `density`, `chord_progression`, `motif`, `section`, `lsystem`, `effects`, `voice` and `gen`. Superseded by v1.4.2 above, which adds `arena`.

Measured at eight modules, CI run 30781463812 — **superseded by v1.4.2 above**; kept because the caps-stay reasoning below rests on these figures:

| | measured | cap | headroom |
|---|---|---|---|
| Linux stripped | 52 504 B | 57 344 B | 4 840 B / **9.2 %** |
| Linux packed | 31 624 B | 34 816 B | 3 192 B / **10.1 %** |
| Windows packed | 45 056 B | 49 152 B | 4 096 B / **9.1 %** |

**The caps are UNCHANGED, and this amendment corrects measurements only.** v1.4.0 promised to re-tighten them once the port landed, on the grounds that a ceiling picked in advance is not a constraint. Running that arithmetic settles it the other way:

- The project's customary headroom is **~14 %** (STRIP_TARGET, v1.1.0) and **~21 %** (PACK_TARGET, v1.2.0). Applied to the measured tree those give 58 KB and 37 KB — **looser than the caps already in place.**
- The v1.4.0 values, chosen deliberately loose without knowing what `effects`, `voice` and `gen` would cost, landed at 9.2 % and 10.1 %. That is tighter than this project has ever set a budget, and it is a real constraint rather than a provisional ceiling.

Lowering them further would tighten past the project's own precedent. The v1.4.0 promise is discharged by the numbers, not by an edit. **This conclusion is unchanged by v1.4.2**: arena moved the measurements +96 B stripped / +104 B packed, which shifts the headroom to 8.3 % / 9.7 % and leaves both still tighter than the ~14 % / ~21 % precedent.

**What actually constrains growth is the page cliff, not either cap.** At eight modules `linux_synth_page_cliff_headroom` was **484 B** against a 256 B advisory, down from 3 428 B across the port; v1.4.2 records the current 324 B. The code segment pays file size in whole 4 KB pages, so the next feature that crosses a boundary costs 4 096 B stripped regardless of how much headroom the caps show. Reason from that number first.

**Budget realignment for the Zig port (v1.4.0, 2026-08-03).** Both Linux caps are raised: packed 30 → 34 KB, stripped 50 → 56 KB. Windows is untouched at 48 KB and remains the loosest of the three.

**This is the first raise of a cap that was enforced and currently met.** v1.1.0 and v1.2.0 each realigned *aspirational* targets the shipped synth had never hit, gated as warnings only. That difference is real and the reasoning is recorded rather than assumed:

- **The caps constrain the C synth's footprint. They are not a scope decision about porting that synth to another language.** They were derived by measuring what the C build shipped, for a project whose goal was to stay small. `specs/005-zig-port` is a port; its purpose is that the code is in Zig. Reading the budgets as an answer to "how much of the port happens" is a category error, and it produced two wrong calls in that arc — first a GO/NO-GO gate that closed the port outright on a measurement of the wrong strategy, then "the measured packed size decides how far it goes," which recommended abandoning three modules mid-port. Both are withdrawn; see `specs/005-zig-port/plan.md` Non-Goals.

- **The values are PROVISIONAL and deliberately loose.** `effects`, `voice` and `gen` are unported. The `-fno-lto` probe floors them at +1 176 B stripped, and measured probe-to-actual ratios (`section` 2×, `lsystem` sign-flipped from −40 to +376) put the real figure materially higher. Picking a tight cap now would mean amending twice.

- **Phase 7 step 3 re-tightens both caps** to the finished tree's measurement plus the project's customary headroom, once the last module lands. Until then these are a ceiling to work under, not a claim about the shipped binary. The size gate stays enforced throughout — no port PR merges without it.

The port itself carries no size, build-simplification or portability benefit, and never claimed one (`specs/005-zig-port/spec.md`, Non-Goals). The reason is that the codebase should be in Zig.

**Measurement refresh (v1.2.2, 2026-07-19).** The three figures above were stale: `main` grew +5 184 B stripped and +4 588 B packed between the PR #117 artifact and `c7db9fc`, and Principle I was never refreshed. The caps are UNCHANGED — this amendment corrects measurements only and does not weaken a NON-NEGOTIABLE principle. Two consequences follow and are recorded rather than left implicit:

- **`PACK_TARGET`'s stated derivation is now historical.** The v1.2.0 amendment justified 30 720 as "the post-#117 measurement plus ~21 % headroom (5 260 B)". Against the current 30 048 B the same cap retains **672 B, or ~2.2 %**. The cap value stands; the arithmetic that originally justified it no longer holds, and any future reasoning must start from the measured 30 048 B rather than the retired 21 % figure. The same applies to `STRIP_TARGET`: its "~14 % headroom" is now 2 072 B, or ~4.0 %.
- **Stripped-byte savings do not transfer to packed at parity.** Measured 2026-07-19 while sizing a candidate feature: removing twelve prose diagnostic strings saved 288 B stripped but only 96 B packed — UPX compresses repetitive English roughly 3:1. Any size argument reasoning from stripped bytes overestimates the packed result by ~3×. This is why identical edits move the two budgets differently, and why the packed budget — now the tighter of the two in percentage terms — is the one that binds.

The 24 KB → 50 KB (Linux stripped) amendment (v1.1.0 / PR #116; growth attribution corrected in v1.2.1) realigns the budget with measured reality. The prior ≤24 KB figure was an aspirational PLAN.md-era target the shipped synth never met: the pre-MIDI binary already measured ~39 KB (pre-#109 README / ARCHITECTURE size tables), and `make size` enforced the target as a WARNING only. The 003 MIDI-input chain (FR-001..FR-054, int32_t SPSC ring + CC dispatch + libasound sequencer worker + opt-out + 23 unit tests in `tests/unit/test_midi.c`) added ~5 KB on top of that (~39 KB → 43 944 B post-#117) — the principled cost of supporting Principle III (Deterministic) + Principle IX (Cross-Platform From Day One) + Principle X (Generative > Random) that the PR #108→PR #109→PR #113 chain chose to eat rather than defer. The 50 KB cap is the post-#117 measurement plus ~14 % headroom, now enforced as a hard CI gate (the pre-arc check was warning-only for Linux and gated Windows only).

The 12 KB → 30 KB (Linux UPX-packed) amendment (v1.2.0 / PR #121; growth attribution corrected in v1.2.1) closes the "implicit Linux UPX cap" loophole: prior versions of Principle I only enumerated the Windows UPX and Linux stripped budgets, leaving the Linux UPX cap implicit in the Makefile `PACK_TARGET = 12288` — a warning-only target the shipped synth likewise never met (the pre-MIDI `synth.packed` measured ~14–16 KB per the pre-#109 README / ARCHITECTURE tables). The 003 chain added ~9.5 KB packed on top of that (~16 KB pre-#109 → 25 460 B post-#117 per the PR #117 `binary-sizes` artifact). Codifying the cap explicitly as 30 KB — ~21 % headroom over the measured 25 460 B (5 260 B) and matching STRIP_TARGET's ~14 % headroom pattern — makes the architecture-level commitment traceable from Constitution v1.2.0 → Makefile `PACK_TARGET = 30720` → `make size` printout → CI binary-sizes artifact per PR. The same Principles III + IX + X cited by the v1.1.0 amendment apply.

### II. C99 and Zig
No C++, no external runtime dependencies beyond libc + libpulse (Linux) / winmm (Windows). C99 and Zig are both first-class implementation languages: a module may be written in either, and exposes the same C ABI through its own `.h` either way. Build-time tools (`gen_*_table.c`) are C99. No code generators outside what already exists. **No build system beyond GNU Make** — gcc compiles the C, `zig build-obj` compiles ported modules to object files, and gcc links every target, so the build system is unchanged by the language split.

The runtime-dependency clause binds Zig modules identically and is checked, not assumed: `nm -u` on a produced object must show nothing outside libc/libgcc. Zig's own runtime is not linked in — at the optimization levels the release build uses, a ported leaf module produces no undefined symbols at all.

**Amendment history (v1.3.0, 2026-08-02).** This principle was "C99 Only" through v1.2.2. The Zig port (`specs/005-zig-port`) is incremental and per-module, and the honest end state is a permanent mix rather than a migration: `main.c`, `ui.c`, `keys.c`, `wav.c`, `mixer.c`, `arena.c` and the `audio_*` backends stay C. *(Superseded for `arena` only: it was ported at PR #204 once its exclusion rationale expired — see the v1.4.2 measurement-refresh block above, which records why. The other seven still stay C. This sentence is left as written because it is what v1.3.0 reasoned from.)* Cost was measured before this amendment, not asserted after it — a tuned Zig module and the same module compiled as C without LTO produce byte-identical stripped binaries (CI run 30756190233), so the language change costs nothing against Principle I and the per-module cost is leaving gcc's LTO unit.

### III. Deterministic (NON-NEGOTIABLE)
Given `--seed N`, audio output is byte-identical across runs and across the supported build targets (Linux glibc + Windows winmm, both little-endian x86). The runtime engine is integer-only (int16 / int32 / int64) — no `double` or `float` in any synth / voice / mixer / effects module. The build-time table generators (`gen_*_table.c`) use `pow()` / `sin()` / `double`, but their outputs are rounded with `(int)(x + 0.5)` per the deterministic IEEE-754 round-half-to-even contract and committed to headers, so the source-of-truth bytes for every constant are identical regardless of which platform produced the committed `.h`. The WAV writer emits native-endian RIFF (`fwrite(&uint16/uint32, ...)`); little-endian on both supported targets. No clock reads inside the synth, no untracked PRNG sources, no thread-induced ordering. A bit-exact 16-second SHA-256 regression test gates every PR on Linux; a multi-seed integration test catches drift across seeds. Intentional output changes require regenerating goldens in the same PR. (A Windows-side WAV byte-identity runner is not currently in CI — the cross-platform invariant holds by code construction, not by automated cross-platform test.)

### IV. Ambient + Algorithmic Aesthetic
Targeted at long-form listening (10+ minutes). Per-bar variation is fine; per-second jarring change is not. Voices should sound composed, not random. Features earn their place by adding perceptible structure on top of stochastic note selection — the song-section state machine, chord progressions, L-system phrasing, adaptive density, inter-voice listening, and long-term motifs all exist to push the output away from "random noise" toward "intentional music."

### V. Cleanly Modular
Each concern lives in one `.c`/`.h` or `.zig`/`.h` pair with a one-way dependency direction documented in `ARCHITECTURE.md`. No `extern` declarations across module boundaries, no weak-symbol workarounds, no circular includes. Current shape:

```
main → {wav, audio, ui, gen, effects, voice, audio_midi, arena}
audio_pulse / audio_winmm → {mixer, ui, keys, arena}
audio_midi → {effects, voice, arena}
audio_midi_linux / audio_midi_winmm → {audio_midi}
wav → {mixer, arena}
mixer → {gen, voice, effects, audio_midi}
keys → {ui, gen, voice, effects}
ui → {voice, gen, effects}
gen → {voice, lsystem, chord_progression, section, density, motif, effects}
voice → {arena, effects}
```

The graph is a module graph, not a file graph, and the Zig port does not alter a single edge.

**The `extern` clause and Zig (v1.3.0).** A Zig module cannot `#include` a C header, so it declares its exported symbols with `export fn` and anything it calls with `extern fn`. That is textually the thing the sentence above forbids, and the exception is stated rather than glossed: it is permitted **only against the module's own `.h`**, which remains the single seam. What is genuinely lost is compile-time signature checking — nothing verifies that the Zig declaration and the C header agree, so a mismatch is a link-clean, run-wrong bug rather than a compile error. The bit-exact golden is the backstop, and it is the reason a port PR that changes the render hash is a failed port and not a new golden.

`@cImport` would restore the checking but produces distinct incompatible types per importing module, forcing a shared `c.zig` and flattening the per-module `.h` seam this principle exists to protect. It is the worse trade for leaf modules; it becomes unavoidable at `voice`, whose four generated lookup-table headers hold ~2500 `const` entries that cannot be hand-transcribed.

### VI. Test Discipline (NON-NEGOTIABLE)
Every pure-synth module has unit tests and a per-file coverage gate in CI (typically ≥90–95%). Interactive modules (`audio_pulse.c`, `audio_midi_linux.c`) are explicitly excluded from measurement with rationale (require a live audio server / ALSA sequencer that CI does not provide). Three integration layers gate audio regressions:
- Bit-exact 16-second SHA-256 (algorithmic drift)
- Multi-seed audio-characteristic bounds (peak/RMS/clip count)
- Live-mode smoke test (segfault / startup regressions)

**Coverage measurement for Zig modules (v1.3.0).** The per-file gate applies to every pure-synth module regardless of language; only the backend differs. gcov cannot instrument Zig at all — `zig build-obj` rejects `-fprofile-arcs`, `-ftest-coverage`, `-fprofile-instr-generate` and `-fcoverage-mapping` alike — so Zig modules are measured with kcov, which reads DWARF and needs no instrumentation. Both backends emit into the same `make coverage` output in the same format, and one CI gate reads both.

The two backends do **not** produce comparable line counts: gcov counts instrumented arcs, kcov counts DWARF source lines. A threshold carried across a port is therefore a new measurement, not a continued one, and the port PR states both numbers.

**What a Zig module does not get (v1.3.0).** This is a real reduction and is recorded rather than absorbed:

- **No gcc sanitizer instrumentation.** `make test-asan` still builds, links and passes with a Zig module present — gcc supplies the ASan/UBSan runtime and tolerates partial instrumentation — but the module's own code is not instrumented. In its place the module is built at `-OReleaseSafe` in that tree only, which keeps Zig's integer-overflow and bounds checks. That is deliberate: at `-OReleaseSmall` Zig disables its own checks too, which would leave the module with no runtime checking from either side.
- **No `-Wall -Wextra -Werror`.** The warning gate lives in `SAN_FLAGS` and nowhere else, and has no `zig build-obj` equivalent covering the project's existing rules.

Neither loss is offset by tooling that does not exist today. A future Zig lint step would close the second one.

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
Single 128 KB static arena (`zig/arena.zig`), 8-byte-aligned bump allocator, no `free`. All audio buffers, voice pool, and reverb/delay state allocate from the arena. Per-module static state (Markov tables, ring buffers, etc.) lives in `.bss` unless it carries a non-zero initializer. No `malloc`, no dynamic resizing. OOM in the arena is a programmer error and exits the process. `arena.h` is unchanged and remains the seam; the pool carries an explicit all-zero initializer and 64-byte alignment, because `undefined` would move 128 KB out of `.bss` into `.data`.

**The arena exclusion, and why it expired (v1.4.4, 2026-08-03).** This section previously read:

> `arena.c` stays C (`specs/005-zig-port/spec.md`, Out of Scope): it is a single 128 KB instrumented global, and porting it removes the ASan redzone guarding the writes that `effects.c` and `voice.c` — which remain C and remain instrumented — make into the pool. It is the one module whose port would degrade sanitizer coverage for modules that were not ported, to save 22 lines.

Three of those claims are now false. `arena` was ported at PR #204; `effects` and `voice` became Zig at PRs #197 and #199, so neither "remains C" nor "remains instrumented"; and the `specs/005-zig-port/spec.md` Out-of-Scope entry it cites is struck through as superseded.

The rationale was sound when written and expired without being noticed. gcc cannot instrument a `zig build-obj` output, so by the time arena was reached **every module allocating from the pool was already uninstrumented** and the redzone protected nothing it was kept for. Leaving arena as the one C module on the synth side would have been arbitrary rather than principled. What compensates is Principle VI's `build_san` rule: ported modules compile at `-OReleaseSafe` so a module gcc cannot instrument still carries Zig's own overflow and bounds checks.

`specs/005-zig-port/plan.md` recorded this section's `arena.c` reference as "resolved by putting arena out of scope." That resolution was undone by PR #204 and nothing pointed back here.

A file-scope variable in a Zig module carries an **explicit initializer matching the C declaration it replaces**. `undefined` is prohibited: it is 0xAA-poisoned outside ReleaseSmall/ReleaseFast and moves the symbol out of `.bss`. The hazard is silent in both directions — a wrong initial value can pass the golden hash, the multi-seed test, the unit tests and the size gate at once when the first write precedes any consequential read.

### Build infrastructure
GNU Make with auto-generated header dependencies (`-MMD -MP`) for C sources. Five object-file classes, each with one pattern rule per language: `%.o`, `%.win.o`, `%.dbg.o`, `$(BUILD_COV)/%.o`, `$(BUILD_SAN)/%.o`. Coverage build isolated in `build_cov/` and the sanitizer build in `build_san/`, so `make coverage`, `make test-asan` and `make test-unit` can be alternated without `make clean`.

Zig emits no `.d` files, so ported modules are outside `-MMD -MP` dependency tracking; their rules carry `$(HEADERS)` as an explicit prerequisite instead. Deleting a `.c` in a port leaves a stale `.d` naming it, which breaks any tree that has built before — a port PR says `make clean`.

### Tooling required
- gcc (Linux + MinGW cross for Windows)
- Zig, pinned (a codegen change in a 0.x compiler can alter the render hash with no source change, so a version bump is a golden-regeneration-class change and gets its own PR)
- libpulse-dev (Linux)
- UPX (for size-budget packing)
- gcov + bash (coverage gates, C modules)
- kcov (coverage gates, Zig modules; built from source, since it is not packaged for the pinned runner image)
- python3 + numpy (for the multi-seed audio-characteristic test)

## Development Workflow

1. **Branch per change.** All work lands on `main` via GitHub PRs. No direct commits to `main`.
2. **PR scope: one feature or one refactor.** No mixed-purpose PRs. Refactors are mechanical (bit-exact regression passes with the existing golden); features regenerate golden in the same PR.
3. **Verify locally before pushing.** Every command below needs the pinned Zig on `PATH` once any module is ported; `make coverage` also needs kcov. Commit first: the bridge regression tests refuse to run against an uncommitted `Makefile` or Constitution, because their recovery path is `git checkout --` on those two files.
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

**Version**: 1.4.4 | **Ratified**: 2026-05-23 | **Last Amended**: 2026-08-03

<!--
v1.4.4 (2026-08-03) — Memory model caught up to PR #204.

PATCH, same basis as v1.4.3: corrected wording, no principle content
changed. Principle I is UNTOUCHED.

The Memory model section named `arena.c` as the arena implementation
and carried a paragraph asserting that arena "stays C" because porting
it would remove the ASan redzone guarding writes from `effects.c` and
`voice.c`, "which remain C and remain instrumented". All three module
claims were false: arena went Zig at PR #204, effects at #197, voice
at #199. The cited spec.md Out-of-Scope entry is struck through as
superseded.

The rationale was correct when written. It expired at #199 — the
moment the last instrumented caller of the pool became Zig — and was
acted on correctly at #204, but this section was never updated to
match. `specs/005-zig-port/plan.md` had recorded the reference as
"resolved by putting arena out of scope"; #204 undid that resolution
and nothing pointed back here.

Also added: a supersession note on the v1.3.0 amendment-history
paragraph, which lists `arena.c` among the modules that stay C. That
sentence is left as written, because it is what v1.3.0 reasoned from.
-->

<!--
v1.4.3 (2026-08-03) — preamble caught up to Principle II.

PATCH, per the v1.2.1/v1.2.2 precedent: corrected wording only, no
principle content changed. The opening sentence of this document still
read "a synthesizer in C99" while Principle II — amended at v1.3.0,
seven versions of this footer ago — reads "C99 and Zig". The governing
document contradicted itself on its own first line.

v1.3.0 retitled the principle and rewrote its body, and every
downstream propagation (README, stretto.1, PKGBUILD, --help) was
treated as the work. The preamble was not a propagation target because
nobody enumerated the containing file. Principle I is UNTOUCHED and
this edit cannot reach it: the three budget regexes in
tools/spec-budget-check.sh key on '≤N KB UPX-packed ...' literals that
appear nowhere in the preamble sentence.

SC-005 of specs/005-zig-port ("the Constitution accurately describes
the tree at every merge point") is what this closes for the preamble.
-->

<!--
v1.3.0 (2026-08-02) — Zig as a second implementation language.

MINOR, not PATCH: this changes principle content, where v1.2.1/v1.2.2
corrected wording and measurements only.

Amended: II (retitled "C99 Only" -> "C99 and Zig"), V (the .c/.h pair
wording and the cross-module `extern` clause), VI (coverage backend for
Zig modules, and what such a module does not get), Memory model, Build
infrastructure, Tooling required, Development Workflow 3.

Principle I is UNTOUCHED, deliberately: three suites parse its paragraph
by literal string including the U+2264 character
(tools/spec-budget-check.sh, tests/test_spec_budget_check.sh,
tests/test_spec_budget_amend.sh), so reflowing it reddens a required
check. The budgets are unchanged and the port has not spent against
them.

Principle III needs no amendment. Its "committed to headers" claim
became true with PR #181, and six independent LLVM builds across two
toolchain arrangements render byte-identical to the golden, making its
integer-only guarantee an empirical cross-compiler result rather than
an argument from the text.

VI is NON-NEGOTIABLE and this amendment reduces it: ported modules lose
gcc sanitizer instrumentation and the -Werror warning gate. Coverage
itself is NOT lost -- it moves to kcov and the per-file gate still
applies. The reduction is named in the principle rather than left to be
discovered.
-->

