# Tasks: Stretto Baseline

**Input**: Design documents from `/specs/001-stretto-baseline/`
- [spec.md](spec.md) (required — user stories with priorities)
- [plan.md](plan.md) (required — tech stack, library + structure choices)
- [research.md](research.md) (Phase 0 — decisions / rationale / alternatives)
- [quickstart.md](quickstart.md) (Phase 1 — minimal usage examples)

**Tests**: Test tasks are included — Stretto has explicit per-module unit tests and three integration layers gated in CI per Constitution Principle VI.

**Organization**: Tasks are grouped by user story to enable independent implementation, testing, and bite-sized MVP delivery. Everything below is already shipped (validated by `golden/regression_16s.sha256` + multi-seed integration); restructuring this list is what future feature/refactor PRs do.

## Format

`- [ ] [TaskID] [P?] [Story?] Description with file path`

- **[P]** = parallelizable (different files, no dependency on incomplete peer)
- **[Story]** = `[US1]` / `[US2]` / `[US3]` — only on user-story phase tasks
- File paths in descriptions are absolute to the repo root.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Repository skeleton, build infrastructure, platform codepath wiring. Everything below must complete before any user story starts.

- [ ] T001 Create repository skeleton: `Makefile`, `config.h`, `.gitignore`, `.gitattributes` (LF normalization), `.github/workflows/ci.yml`.
- [ ] T002 [P] Create empty C99 source stubs: `main.c`, `arena.{c,h}`, `gen.{c,h}`, `voice.{c,h}`, `effects.{c,h}`, `mixer.{c,h}`, `ui.{c,h}`, `keys.{c,h}`, `wav.{c,h}`, `audio.{h}`.
- [ ] T003 [P] Create audio platform stubs: `audio_pulse.c` (Linux, libpulse callback shell) + `audio_winmm.c` (Windows, winmm callback shell) sharing `audio.h` API.
- [ ] T004 [P] Create build-time table generators: `gen_sin_table.c`, `gen_env_table.c`, `gen_euclid_table.c`, `gen_note_table.c`, `gen_wavetable.c` — each emits a C array into `.rodata`.
- [ ] T005 [P] Initialize test scaffolding: `tests/test.h`, `tests/test_bitexact.sh`, `tests/test_multi_seed.sh`, `tests/test_smoke_live.sh`, `tests/golden/regression_16s.sha256` placeholder.

**Checkpoint**: `make` builds an empty binary; `make test` runs against an empty golden and warns.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Module-level infrastructure that blocks all user stories. Completing this phase is **CRITICAL** before any story can be safely implemented.

- [ ] T006 Implement 128 KB static arena in `arena.c` with `arena_init`, `arena_alloc(size)`, `arena_used()`, `arena_remaining()`. OOM exits with code 1 (FR-050).
- [ ] T007 [P] Implement single-source LCG PRNG in `gen.c` with `lcg_next(state)`, `lcg_seed(state, n)`. Single mutable `g_rng` global, declared in `gen.h` (Principle III).
- [ ] T008 [P] Implement scale table + scale selection in `gen.c`: `SCALES[6][7]` (Dorian/Lydian/Phrygian/Locrian/HM/Mixolydian) and `cur_scale` runtime selector (FR-010). No auto-rotate.
- [ ] T009 [P] Implement voice-pool skeleton in `voice.c`: enum `VOICE_KS | VOICE_FM | VOICE_WT | VOICE_ADD | VOICE_SUB | VOICE_DRUM`, `voice_pool_t` structure, 11-slot allocation (2 bass / 3 chord / 3 melody / 3 drum). Per FR-001.
- [ ] T010 [P] Implement WAV writer skeleton in `wav.c`: RIFF header builder, 16-bit stereo 48 kHz interleaved writer, `wav_close` flush on exit (FR-002, FR-031).
- [ ] T011 [P] Implement effects chain scaffold in `effects.c::effects_process()`: per-voice ADSR + SVF placeholder, voice mix → (placeholder reverb) → (placeholder delay) → `sat16()` final clamp (FR-003, FR-004).
- [ ] T012 Implement mixer scaffold in `mixer.c::mixer_run()`: invoke `voice_pool_mix()`, then hand the buffer to `effects_process()`, then `wav_write_buf()` (renders) or `audio_write_buf()` (live).
- [ ] T013 [P] Wire `main.c` CLI parser: `--seed N`, `--render <secs> <out.wav>`, `--no-ui`. Default = live mode. Exits with usage on unknown arg combinations (FR-030..FR-034).
- [ ] T014 [P] Implement `audio.h` callback surface shared by Linux + Windows platforms; `audio_open(dev, cb)`, `audio_close()`, `audio_write_buf(buf)`.
- [ ] T015 Implement genesis path in `gen.c::gen_init(seed)`: derive song state from PRNG, reset tempo, init section, init mask, init motif, init chord Markov (research.md §5).

**Checkpoint**: A baseline run with `synth --no-ui --render 1 /tmp/out.wav --seed 0` produces 48 kHz × 2-ch silence for 1 second (warm-up — audio material follows).

---

## Phase 3: User Story 1 — Listen to evolving generative music (Priority: P1) 🎯 MVP

**Goal**: `./synth` plays continuous, deterministic-but-evolving ambient music that demonstrates per-bar rhythm, per-2-bar chord motion, per-minute section changes, and at least one recurring melodic motif within 5 min (US1 acceptance scenarios).

**Independent Test**: `./synth --no-ui --render 300 <out>.wav --seed 0` produces a 5-min WAV whose peak ∈ [500, 32767), clip count ≤ 100, non-silent on ≥50 % of samples, spectral centroid ∈ [100, 5000] Hz, ZCR ∈ [0.01, 0.30`. SC-001..SC-003 must hold. SC-006 (listener identifies section + motif in 10 min) is a manual check.

### Implementation for User Story 1

#### Voice synthesis methods (FR-001, FR-005..FR-009a)

- [ ] T016 [P] [US1] Implement Karplus-Strong synthesis in `voice.c::voice_step_KS()`: noise-seeded delay line + averaging-filter feedback. Used by melody bank (FR-005).
- [ ] T017 [P] [US1] Implement 2-op FM in `voice.c::voice_step_FM()`: carrier modulated at per-role carrier:mod ratio, both ops detuned together by pan LFO. Used by melody bank alternation (FR-006).
- [ ] T018 [P] [US1] Implement wavetable in `voice.c::voice_step_WT()`: linear interp over 8 morphed single-cycle waves (256 samples each) from `gen_wavetable.c`; position swept by per-voice pan LFO (FR-007). INTRO and RESOLVE sections only.
- [ ] T019 [P] [US1] Implement additive in `voice.c::voice_step_ADD()`: 8 sinusoid partials at integer multiples of fundamental with drawbar-style amplitude profiles (FR-008). BODY section only.
- [ ] T020 [P] [US1] Implement super-saw subtractive in `voice.c::voice_step_SUB()`: 3 detuned (±0.78 %) band-limited saw oscillators summed and SVF-filtered (FR-009). Bass role only.
- [ ] T021 [P] [US1] Implement drum synthesis in `voice.c::voice_step_DRUM()`: kick/snare/hihat envelopes + oscillator/noise source per patch. 3 drum slots follow a section-pinned pattern (FR-001).
- [ ] T022 [US1] Implement portamento (glide) in `voice.c::voice_step_SUB()`: when a SUB re-trigger lands while the previous amplitude envelope is above half-sustain, slide `inc_target` linearly to the new note over ~50 ms instead of jumping; preserve phase + envelope (FR-009a). Determinism preserved (no extra PRNG draws).

#### Generative state (FR-010..FR-016)

- [ ] T023 [P] [US1] Implement 6-symbol L-system grammar in `lsystem.c` over `u U d D r .` with 3 hand-tuned characters selectable by mutation (FR-011). Drives main melody.
- [ ] T024 [P] [US1] Implement 2nd-order Markov counter-melody in `gen.c`: table indexed by previous two degrees, biased away from unison with main-melody recent degree, toward 3rd/6th consonances (FR-012).
- [ ] T025 [P] [US1] Implement chord-progression Markov chain in `chord_progression.c`: 7 scale-degree functions, advances every 2 bars; bass plays root + fifth; one-step diatonic approach at every chord change (FR-013). Cycle every N bars, no end-state.
- [ ] T026 [P] [US1] Implement cellular-automaton 7-bit degree mask in `gen.c::gen_advance_ca()` for `ca_row` and `ca_harm` per substep; forced non-empty (re-seed `0x01` if it collapses) and forced non-all-zero (reseed `0x12345678`/`0x87654321`).
- [ ] T027 [P] [US1] Implement motif ring buffer in `motif.c`: 8 slots × 4-bar main-melody phrases, capturing every 4 bars; `motif_replay` invoked at 30+ bar intervals with ~25 % probability, verbatim or ±2 diatonic transposition (FR-015).

#### Section state machine (FR-014, FR-017)

- [ ] T028 [US1] Implement 4-section state machine in `section.c` (INTRO / BODY / TENSION / RESOLVE) on a fixed 96-bar period; track current bar in bar counter; advance deterministically.
- [ ] T029 [US1] Implement 8-bar crossfade in `section.c::section_apply_bias()`: gate / cutoff / reverb / mutation-interval biases lerped over 8 bars at each boundary.
- [ ] T030 [US1] Implement per-section pinning in `section.c::section_apply_pins()`: kick pattern, L-system character, chord voice type (INTRO=WT, BODY=ADD, TENSION=FM, RESOLVE=WT), chord playback mode (TENSION=arpeggio, others=block).
- [ ] T031 [US1] Implement voice-family mask in `section.c::section_apply_mask()`: per-section 7-bit active-family mask. INTRO = one of 8 curated 1–3-voice combos, randomized once per 96-bar cycle, seed-deterministic (FR-017). Mask MUST NOT advance PRNG/L-system/Markov/motif state when silenced.

#### Effects chain (FR-003)

- [ ] T032 [P] [US1] Implement Schroeder reverb in `effects.c::effects_reverb_step()` with section-biased wet level.
- [ ] T033 [P] [US1] Implement stereo delay in `effects.c::effects_delay_step()` with `g`/`G` (wet) + `f`/`F` (feedback) live-mode controls (FR-041).
- [ ] T034 [P] [US1] Implement soft cubic saturation in `effects.c::effects_saturate()` ahead of the compressor.
- [ ] T035 [P] [US1] Implement feed-forward compressor in `effects.c::effects_compressor()` with section-dependent threshold.
- [ ] T036 [P] [US1] Implement brickwall limiter in `effects.c::effects_limiter()` catching transients missed by compressor.
- [ ] T037 [US1] Final-clamp pipeline in `effects.c::effects_process()`: chain order = voice mix → reverb → delay → saturation → compressor → limiter → `sat16()`. Per FR-003.

#### Audio streaming (FR-002, FR-022)

- [ ] T038 [P] [US1] Implement Linux audio streaming in `audio_pulse.c::audio_thread()` with libpulse; callback-driven, 48 kHz × 2-ch × int16.
- [ ] T039 [P] [US1] Implement Windows audio streaming in `audio_winmm.c::audio_callback()` with winmm waveOut; same surface as Linux.
- [ ] T040 [US1] Ensure no `time()` reads in the synth path (`grep -n 'time(NULL' audio_*.c gen.c voice.c effects.c mixer.c` returns zero hits) — Constitution III.

**Checkpoint**: User Story 1 fully functional. `./synth` plays evolving music on Linux + Windows. `--no-ui --render` produces a deterministic, non-trivial 5-min WAV.

---

## Phase 4: User Story 2 — Render reproducible audio (Priority: P2)

**Goal**: User renders a fixed-length WAV from a seed; identical seeds → identical bytes; different seeds → distinct files. Bounds `(secs ∈ [1, 3600])` enforced.

**Independent Test**: `./synth --render 16 /tmp/a.wav --seed 0 && ./synth --render 16 /tmp/b.wav --seed 0 && cmp /tmp/a.wav /tmp/b.wav` returns 0; changing seed flips the SHA-256.

### Implementation for User Story 2

- [ ] T041 [US2] Implement `--render` exit path in `main.c`: validate `secs ∈ [1, 3600]` (FR-031), open `wav_open(out)`, loop `secs * 48000` samples calling `mixer_run()`, `wav_write_buf()`, `wav_close()`.
- [ ] T042 [US2] Validate output-path write permission in `main.c`: on `fopen(out, "wb")` failure, exit with error referencing the path (FR-031 scenario 4).
- [ ] T043 [US2] Strict `--seed` parser in `main.c::parse_seed()`: any non-digit character → `argv_idx = 0` → exits with usage (FR-030).
- [ ] T044 [US2] Hook `--seed N` into `gen.c::gen_init(N)` so render path populates PRNG + Markov + CA + section + motif deterministically (FR-020).
- [ ] T045 [US2] Without `--seed`, fall back to `time(NULL)` per launch so each live-mode listen is different (FR-021); only valid in live path, not `--render`.

**Checkpoint**: User Story 2 fully functional. Bit-exact regression holds against current golden. Multi-seed integration test passes for 4 known seeds.

---

## Phase 5: User Story 3 — Shape the texture live (Priority: P3)

**Goal**: While listening, the user adjusts synthesis parameters via single-keystroke commands; each change is reflected in audio + status row within ~21 ms (one buffer).

**Independent Test**: `tests/unit/test_keys.c` covers all key bindings. Live acceptance: `?` shows help; `+` speeds up tempo (status `M:` advances faster); `q` exits clean within 1 s with cooked terminal.

### Implementation for User Story 3

- [ ] T046 [US3] Implement live-mode key poll in `keys.c::keys_poll()` with single-keystroke dispatch via `tcgetattr`-raw `/ O_NONBLOCK`-style reads.
- [ ] T047 [US3] Implement tempo control in `gen.c` + `keys.c`: `+` decrements `samples_per_substep` (faster), `-` increments (slower); clamped at documented bounds (FR-041).
- [ ] T048 [US3] Implement `c`/`C` (cutoff), `n`/`N` (resonance), `m`/`M` (LFO depth), all applying to per-voice SVF + pan LFO with documented step sizes (FR-041).
- [ ] T049 [US3] Implement `s` (cycle scale), `g`/`G` (reverb wet), `d`/`D` (delay wet), `f`/`F` (delay feedback), `r`/`R` (mutation rate). Each setter clamps at documented bounds (FR-041).
- [ ] T050 [US3] Implement `t` (motif capture / cycle replay) integrating with `motif.c::motif_capture_or_replay()`.

#### Live-mode UI (FR-040, FR-042)

- [ ] T051 [P] [US3] Implement ANSI oscilloscope in `ui.c::ui_render_oscilloscope()`: redrawn at every audio buffer boundary, scoped to current mixer output.
- [ ] T052 [P] [US3] Implement status row in `ui.c::ui_render_status()`: live state of `cur_scale`, chord root, section name, density tension, filter params, motif capture/replay state.
- [ ] T053 [P] [US3] Implement help overlay toggle on `?` in `ui.c::ui_render_help()` — pure ANSI overlay, dismisses on any key.
- [ ] T054 [US3] Implement terminal restore in `ui.c::ui_shutdown()`: cooked mode restored, cursor visible on exit (FR-042). Linux path uses `tcsetattr`; Windows path uses `ENABLE_VIRTUAL_TERMINAL_PROCESSING` toggle (already-enabled by `ui.c` on Win 10+).
- [ ] T055 [US3] `--no-ui` path in `main.c`: skip all `tcgetattr` calls — `synth` runs headlessly under script / CI without TTY.

**Checkpoint**: User Story 3 fully functional. Live mode is end-to-end testable on Linux + Windows; key bindings unit-covered; terminal state survives `q`.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Quality gates, CI infrastructure, doc conventions, and cross-section concerns. Independent of any single user story.

- [ ] T056 [P] Add per-module unit tests in `tests/unit/test_<module>.c` for each pure-synth module; cover goal/CI-gate thresholds: 95 % for `arena / effects / voice / section / density / motif / mixer`; 90 % for `gen / lsystem / chord_progression / wav / main` (FR-063).
- [ ] T057 [P] Wire coverage gating in `Makefile::coverage`: gcov + lcov → per-file minimum enforcement. Exclude interactive modules (`ui.c`, `keys.c`, `audio_pulse.c`) with rationale comment in CI.
- [ ] T058 [P] Wire CI workflow in `.github/workflows/ci.yml`: Linux build → bit-exact regression → multi-seed integration → unit + coverage → Windows cross-compile → UPX pack.
- [ ] T059 Implement Windows cross-compile target in `Makefile::win` via MinGW; emulates Linux audio path by no-op + flags `synth.exe` build.
- [ ] T060 Implement UPX pack step in `Makefile::winpack` with `--ultra-brute`; verify final size ∈ ≤48 KB (FR-051).
- [ ] T061 Implement Linux size-budget soft warning in `Makefile::size` (24 KB stripped target) (FR-052).
- [ ] T062 [P] Generate `golden/regression_16s.sha256` from a known seed; regenerate only when an intentional output change lands (FR-060).
- [ ] T063 [P] Generate `golden/regression_multiseed.sha256.txt` for 4 known seeds with distinct output (FR-061).
- [ ] T064 [P] Author `README.md` (listener quick-start), `ARCHITECTURE.md` (one-way dependency map), `CHANGELOG.md` (per-PR summary). Refresh in dedicated `docs/*` PRs when ≥5 feature/refactor PRs accumulate.
- [ ] T065 Enforce no-`TODO` invariant in CI: `grep -RnE 'TODO|FIXME|XXX' '*.c' '*.h'` returns zero (Constitution VII).
- [ ] T066 [P] Remove placeholder `(void)` casts + stub helpers in committed code; `git grep -nE '\(void\)' '*.c'` returns zero (Constitution VII).
- [ ] T067 Implement `--seed` strictness in CI: fuzzed non-integer inputs must produce usage error (FR-030).
- [ ] T068 Implement `make test-smoke` 2-second timeout in `tests/test_smoke_live.sh`; exit codes 0/124/143 are pass, all other codes are fail (FR-062).
- [ ] T069 Verify cross-platform build: `make win && make winpack` produces a packed binary that runs on Windows (when a build host is available) without platform-specific runtime issues.
- [ ] T070 Verify all integration tests on a clean clone: `make && make test && make test-unit && make test-multiseed && make test-smoke && make coverage && make win && make winpack` exit 0 (Constitution VI).

**Checkpoint**: All Constitution gates pass on `main`. Future PRs only need to maintain these gates to merge.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — start immediately.
- **Foundational (Phase 2)**: Depends on Setup completion — **BLOCKS** all user stories.
- **User Stories (Phase 3–5)**: All depend on Foundational phase completion.
  - Stories can proceed in parallel when capacity allows.
  - Sequentially by priority: US1 (P1) → US2 (P2) → US3 (P3).
- **Polish (Phase 6)**: Depends on all desired user stories being complete.

### User Story Dependencies

- **US1 (P1)**: Independent after Foundational. No dependencies on US2/US3.
- **US2 (P2)**: Independent after Foundational + US1 (P1) seed-init path. May overlap with US1 but should be independently testable.
- **US3 (P3)**: Independent after Foundational. UI layer touches `gen.c` + `effects.c` setters but does not block US1/US2.

### Within Each User Story

- Unit tests first (must FAIL before implementation per Principle VI).
- Module skeletons before full behavior.
- Voice synthesis methods before effects chain.
- Section state machine before voice-family mask.
- Effects chain before audio stream wiring (so the first byte of audio is meaningful).

### Parallel Opportunities

- All Phase-1 Setup tasks marked `[P]` can run in parallel.
- All Phase-2 Foundational tasks marked `[P]` can run in parallel.
- All voice synthesis methods (T016–T021) can be implemented in parallel (different functions in `voice.c`, no shared mutable state between methods).
- All generative state modules (T023–T027) can be implemented in parallel.
- All effects chain blocks (T032–T037) can be implemented in parallel.
- Audio platform implementations (T038, T039) can be developed in parallel (one Linux, one Windows person).
- Live-mode keys + UI (T046–T054) can be split between two developers.

---

## Parallel Example: User Story 1

```bash
# Voice synthesis methods (parallel — different functions in voice.c):
Task: "Implement Karplus-Strong in voice.c::voice_step_KS()"
Task: "Implement 2-op FM in voice.c::voice_step_FM()"
Task: "Implement wavetable in voice.c::voice_step_WT()"
Task: "Implement additive in voice.c::voice_step_ADD()"
Task: "Implement super-saw subtractive in voice.c::voice_step_SUB()"

# Generative state modules (parallel — different files):
Task: "Implement L-system in lsystem.c"
Task: "Implement 2nd-order Markov in gen.c"
Task: "Implement chord-progression Markov in chord_progression.c"
Task: "Implement cellular-automaton mask in gen.c"
Task: "Implement motif ring buffer in motif.c"

# Effects chain (parallel — different functions in effects.c):
Task: "Schroeder reverb in effects.c"
Task: "Stereo delay in effects.c"
Task: "Soft cubic saturation in effects.c"
Task: "Feed-forward compressor in effects.c"
Task: "Brickwall limiter in effects.c"

# Audio platforms (parallel — different files, different OS):
Task: "Linux libpulse in audio_pulse.c"
Task: "Windows winmm in audio_winmm.c"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1 (Setup). ⏱ ~1 PR.
2. Complete Phase 2 (Foundational). ⏱ ~2 PRs.
3. Complete Phase 3 (User Story 1). ⏱ ~5 PRs.
4. **STOP and VALIDATE**: 5-min render against audio-characteristic bounds. ✅ Bit-exact regression golden recorded.

### Incremental Delivery

1. Setup + Foundational → Foundation ready (T001..T015). 🎯 Commit a1.
2. US1 → MVP demo. Commit a2.
3. US2 → Render reproducibility guaranteed. Commit a3.
4. US3 → Live shaping complete. Commit a4.
5. Each story adds value, no commit breaks byte-for-byte reproducibility for the previous audio production (bit-exact regression still passes between commits unless the change is intended to alter output → golden regenerated in the same PR per Constitution III).

### Parallel Team Strategy

With multiple developers:

1. Team completes Setup + Foundational together (T001..T015).
2. Foundational done:
   - Developer A: US1 voice synthesis methods (T016..T022).
   - Developer B: US1 generative state (T023..T027).
   - Developer C: US1 section state machine + mask (T028..T031).
3. US1 entirely complete + US2 + US3 then proceed in mirrored parallel.

---

## Notes

- `[P]` tasks = different files, no cross-dependencies.
- `[Story]` label maps task to a spec user story for traceability.
- Each user story is independently completable + testable.
- Test tasks are present because Principle VI is non-negotiable; tests are written before implementation.
- Tasks are committed after each task (or logical group).
- Stop at any checkpoint to validate a story independently.
- Avoid: vague tasks, same-file conflicts, cross-story dependencies that break independence.
- The full set of 90+ shipped PRs maps onto this task breakdown — every completed PR corresponds to one or more of these tasks with its own regen-golden (if intentional) commit.

## Amendment history

- **2026-07-08** — Constitution v1.0.1 → v1.1.0: Principle I stripped-Linux binary budget amended ≤24 KB → ≤50 KB per PR #116. The T061 task's "(24 KB stripped target)" parenthetical references the spec-at-time-of-authoring claim; the current Constitution target is ≤50 KB per `.specify/memory/constitution.md` Principle I amendment paragraph (the 003 MIDI-input chain's principled ~19 KB cost is the documented reason for the raise). The `Makefile::size` warning threshold in T061 (now STRIP_TARGET = 51200 per Makefile) references the new post-amendment value; the T061 task itself is complete and the warning logic is now wiring to the new budget.
