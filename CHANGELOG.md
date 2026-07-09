# Changelog

## Recent: --render N - streams WAV to stdout (046)

`-` was treated as a filename; renders could only target files, so the composability the deterministic engine earns for free (`./synth --render 60 - --seed 7 | ffmpeg -i - out.flac`) was unreachable. The RIFF header has always been written up front from the known duration with no seek-back, so streaming needed no format work — only the sox/ffmpeg `-` filename convention in `render_wav`.

- Byte-identical to the file output (asserted by a new `cmp` check in `tests/test_cli.sh`); goldens and the bit-exact regression are untouched.
- Windows: `_setmode(_fileno(stdout), _O_BINARY)` on the `-` path — text-mode stdout would CRLF-translate the RIFF stream. README notes PowerShell older than 7.4 re-encodes binary pipes regardless (byte-stream passthrough landed in 7.4); use cmd.exe redirection there.
- SIGPIPE keeps its default disposition: a downstream that closes early kills the render (exit 141), standard pipeline behavior. For parents that inherit SIGPIPE ignored, a new fwrite short-write check turns EPIPE into a clean error exit instead of rendering the full duration into a broken pipe.
- Write-error discipline while in the file: the loop `fwrite`, the final `fflush(stdout)`, and the file-path `fclose` are all checked — previously a disk-full render could exit 0 with a silently truncated WAV (the tail of the render lives in the stdio buffer until close).
- Coverage recipe gains a 1-second stdout render so `wav.c`'s new branch stays over its ≥90% gate. `tests/test_cli.sh` adds the equivalence check plus a broken-pipe smoke (accepts 141 or 1 — pinning the signal exit exactly would be a latent flake under SIGPIPE-ignoring parents).
- Considered and rejected: an `isatty()` guard against dumping WAV bytes to a terminal — `-` is explicit opt-in, `cat` doesn't guard either, and the bytes are budgeted.
- Spec surface: 001 FR-031 amended (`<out.wav|->`); 003 contract grammar `positional_render := "--render" UINT ( PATH | "-" )`; RESEARCH_CLI.md F2 flipped to Shipped.

## Recent: --help / -h / --version (044)

`--help` and `--version` were unknown arguments: usage on stderr, exit 1, empty stdout — scripts and packagers probing `--version` (Homebrew audits, AUR helpers) read that as a broken binary, and no version identity existed anywhere in the tree.

- **GNU Coding Standards §4.8 semantics**: both flags print to **stdout**, **exit 0**, and take precedence over every other option and argument (`--seed abc --help` prints help; `--help --render 60 x.wav` renders nothing). `--version`'s first line is machine-parseable (`stretto <ver>`, version after the last space, constant program name per §4.8.1 — deliberately "stretto", not `argv[0]`, even though the Linux binary is `./synth`), followed by the copyright / MIT / NO-WARRANTY block. `--help` ends with the Report-bugs line, help2man-compatible for a future man page. The usage synopsis is a single `USAGE[]` constant shared by the stderr error paths and `--help`, so the two cannot drift. This CLI-surface addition is recorded in `specs/003-midi-input/contracts/cli.md`'s exit-code table (new exit-0 row) as a standards-mandated exception to the "unknown flag = usage error" rule.
- **Version identity**: `version.h` is generated from `git describe --tags --always --dirty` (leading `v` stripped; `dev` fallback for no-git tarball builds) by a compare-and-swap Makefile rule — the recipe runs every `make`, but the file only changes when the version does, so incremental builds stay no-ops and a version change rebuilds exactly `main.o` (explicit deps on the four `main.o` variants; deliberately not in `$(HEADERS)`).
- **The #129/#130 "chmod trap" is retired at the root**: `make test` and `tools/verify-bridge.sh` ran `chmod +x` on two test scripts committed mode `100644`, flipping tracked mode bits on Linux — which would have made `git describe --dirty` mark every post-`make test` build dirty (poisoning the CI Windows artifact and churning `main.o`). The executable bits are now committed (`100755`, matching the other test scripts) and the chmod lines are gone.
- **ci.yml checks out with `fetch-depth: 0`** so `git describe` on CI yields a resolvable `1.2.0-final-N-g<hash>` instead of the bare hash of the ephemeral PR merge ref.
- **`tests/test_cli.sh`** (new, wired into `make test`): help/version stdout + exit-0 + clean-stderr assertions, the precedence-suppresses-side-effects check (no WAV created by `--help --render`), and the unchanged usage-error contract (stderr, exit 1). Needs no TTY / PA / audio device — runs unconditionally everywhere.
- Determinism untouched (audio path unmodified; goldens unchanged). Size: ~1–2 KB of `.rodata` help/version text against 7,320 B stripped headroom.

## Recent: Linux signal-safe terminal restore (042)

Ctrl-C, SIGTERM, or SIGQUIT during live mode on Linux left the terminal corrupted — echo off, canonical mode off, cursor hidden — requiring a manual `reset`. Root cause: no signal handlers existed; the only cleanup was `atexit(restore_terminal)`, and atexit handlers do not run on signal death (raw mode leaves `ISIG` set, so Ctrl-C delivered SIGINT with default terminate disposition). Confirmed empirically in WSL2 PTY experiments against both a minimal replica and the real binary before fixing.

- **In-handler restore + re-raise** (design chosen over flag-and-poll after plan review): `ui_install_signal_handlers()` installs SIGINT/SIGTERM/SIGHUP/SIGQUIT handlers that restore termios + fcntl + cursor from signal context (`tcsetattr`, `fcntl`, `write`, `raise` are all POSIX async-signal-safe) and then re-raise, preserving the killed-by-signal wait status (130/143) the smoke test already expects. `SA_RESETHAND` makes the re-raise work and keeps a second Ctrl-C as a hard-kill escape hatch. The rejected flag-and-poll variant fails exactly when needed most — main thread wedged in `pa_threaded_mainloop_wait` with a dead PA server.
- **`O_NONBLOCK` leak fixed (affected even clean quits)**: raw-mode setup put `O_NONBLOCK` on stdin but `ui_term_restore_mode` never removed it; the flag lives on the open file description shared with the parent shell, so every live session handed the shell back a nonblocking stdin (measured on the real binary). Flags are now saved and restored inside the idempotence guard.
- **atexit registered before raw mode** in `audio_play()`, closing the window where a failed `fcntl` inside raw-mode setup would `exit(1)` with the terminal already raw and no restore registered. The now-redundant explicit restore on the `q` path is gone (atexit covers it). Handlers install from `audio_play()` only, so `--render` keeps default signal behavior. Windows unchanged (Ctrl-C is keystroke `0x03` → `q` path); its only edit is the shared fix for the cursor-show `write` emitting 8 bytes of a 7-char string (stray NUL).
- **Two PTY regression sub-checks** in `tests/test_smoke_live.sh`, placed before the root-gated MIDI `modprobe` section so they actually run in CI: (A) SIGTERM during live mode → ECHO/ICANON restored, exit 143; (B) clean `q` → `O_NONBLOCK` clear on the *inherited* stdin description (a `</dev/tty` redirect would make that assertion vacuous — it probes a fresh description). Sub-check A uses SIGTERM because non-interactive shells start background jobs with SIGINT ignored. Both verified to FAIL against the pre-fix binary and PASS post-fix.
- Determinism untouched: live path only; bit-exact + multiseed goldens pass unchanged.

## Recent: post-arc review fixes (041) — Constitution v1.2.1, gate single-sourcing, MIDI CLI contract reconciliation

A review of the #107–#133 arc surfaced factual and design defects; this change fixes them. The WHY per item:

- **Constitution v1.2.1 (rationale correction, budgets unchanged).** The v1.1.0/v1.2.0 amendment paragraphs attributed the full gap between the old aspirational targets and the post-#117 measurements to the 003 MIDI chain ("~19 KB stripped growth"). The pre-MIDI binary already measured ~39 KB stripped / ~16 KB packed (pre-#109 README/ARCHITECTURE tables), so MIDI's actual cost was ~5 KB stripped / ~9.5 KB packed; the remainder was the never-met PLAN.md-era targets being realigned to reality. The governance record now says so. Principle V's module map also gains the `audio_midi*` modules it was missing.
- **Binary size budget gate single-sourcing.** The ci.yml gate hardcoded the three budgets inline — a third copy that `tools/spec-budget-amend.sh` did not manage, so the first "correct" v1.3.0 amendment would have left the gate enforcing stale values while the bridge passed. `make size` now echoes `STRIP_TARGET` / `PACK_TARGET` / `WIN_PACK_BUDGET` into `binary-sizes.txt` as `budget_*` rows and the gate reads measurements AND budgets from that one artifact. Constitution + Makefile are now the only two copies, which is exactly the pair the bridge + amend helper already manage.
- **MIDI CLI contract reconciled + FR-002 enforced.** `contracts/cli.md` declared exact strings/exit codes the code never implemented, and drifted further when `--midi` became a wildcard. The contract is amended to the as-built surface (space-separated `<index> <name>` list, wildcard semantics, precedence rules instead of mutual-exclusion errors, channel range 1..16) — and the code is fixed where the spec was explicit: a failed startup open now **exits 1 per FR-002** (continue-without-MIDI is FR-034's mid-session-disconnect semantics, not a startup fallback), `--midi-channel` without an open flag is a usage error (it previously enabled the queue with no producer, against FR-005's no-MIDI-CPU intent), a zero-device list prints an stderr notice, and usage strings finally list the `--midi*` flags.
- **`--render` never opens MIDI (Constitution III).** `render_chunk` drains the MIDI queue, so `--render ... --midi` with a connected controller could have injected live notes into a *seeded* render. Render mode now skips the MIDI open entirely (stderr notice, byte-identical to a no-MIDI render). Contract gains G6/G7 + CT-12 for this.
- **Docs de-drifted.** README size table (~39 KB → 43 944 B stripped, ~14 KB → 25 460 B packed, ~215 KB → ~238 KB Windows), ARCHITECTURE's false "Windows strip does not run" claim (the `stretto.exe` rule runs `$(WIN_STRIP)` and `-s` is in `WIN_LDFLAGS`), stale "~37 KB pre-#109" Windows-packed hedges (38 KB post-#117), quickstart's `--midi-channel 0` example (now a usage error), the stale "backend lands in US3" comment in `main.c`, and the CI step tables in README/ARCHITECTURE (both omitted the `Upload binary sizes artifact` row — the gate is UI step #15, not #14 as the #131 docs and the CHANGELOG entry below state).
- **Working-tree hygiene.** `.gitignore` now covers the `.specify/PR-*-body.md` / `commit_msg*.txt` scratch files and one-off measurement binaries; the 34 accumulated scratch files were deleted (canonical copies live on the GitHub PRs/commits).

## Recent: spec↔build cascade closure (PRs #121–#131) + tagged v1.2.0-final

The 11-PR arc that closed the post-#117 spec↔build drift cascade and added the Constitution↔Makefile bridge, the amend helper, the scoped dirty-tree guard, and the documentation arc. Tagged as `v1.2.0-final` to mark the closure.

**The 4 cascade artifacts** (Constitution Principle I size budgets must stay in lockstep between `.specify/memory/constitution.md` and the `Makefile`):

| Artifact | Role |
|---|---|
| `tools/spec-budget-check.sh` | Read-only bridge: asserts `Makefile byte value = Constitution KB value × 1024` for all 3 budgets (Windows UPX, Linux UPX, Linux stripped). Runs as ci.yml step `Bridge regression test` + `make test` target. |
| `tools/spec-budget-amend.sh` | Write helper: bumps 1-3 budgets in BOTH files in lockstep via targeted `sed -E` regex, prints a `git diff` for review, **refuses to commit** (developer commits manually with a v1.X.0 rationale). Supports `--win` / `--lin-upx` / `--lin-str` flags + `--dry-run`. Rejects shrink attempts (budgets can only grow per post-#117 policy). |
| `tests/test_spec_budget_check.sh` | 5-scenario regression: happy-path, tamper Constitution, tamper Makefile, malformed constant, recovery via `git checkout`. 9 sub-checks. |
| `tests/test_spec_budget_amend.sh` | 6-scenario regression: happy-path amend 1, atomic amend all 3, input validation (4 sub-cases), dry-run, refuse-to-commit, recovery via `git checkout`. 21 sub-checks. Has a **scoped dirty-tree guard** (only checks Constitution + Makefile, not whole tree) to avoid the `chmod +x` mode-change trap from `make test`. |

**The 11 PRs in the arc:**

| PR | Title | Key change |
|---|---|---|
| #121 | Constitution v1.1.0 catchup | Propagated 24 KB → 50 KB Linux stripped amendment to Makefile `STRIP_TARGET` |
| #122 | `tools/spec-budget-check.sh` bridge | Added the read-only Constitution↔Makefile bridge + pre-flight ci.yml step |
| #123 | 5-case regression test | `tests/test_spec_budget_check.sh` — 5 scenarios / 9 sub-checks |
| #124 | Bridge test wiring | Wired bridge test into `make test` + dedicated ci.yml step |
| #125 | Cascade close | Removed the now-redundant `Assert Spec↔Build size budgets` ci.yml step (the bridge step + gate are the only 2 spec↔build enforcement points now) |
| #126 | Step-numbering doc | Added header comment to `ci.yml` noting the 1-indexed-from-`Set up job` numbering convention |
| #127 | `tools/spec-budget-amend.sh` helper | Added the write helper for future v1.3.0+ amendments — 1-invocation Constitution↔Makefile updates that can't drift |
| #128 | Amend test wiring | Wired amend test into `make test` + dedicated ci.yml step (`Amend helper regression test`, step #7 in 1-indexed UI) |
| #129 | Whole-tree dirty-tree guard | First version of the dirty-tree guard (whole tree). Caused the post-merge CI failure in run `28977516712` (chmod +x mode change triggered spurious bail). |
| #130 | Scoped dirty-tree guard | Scoped the guard to only Constitution + Makefile — the files the test actually tampers with. Fixes the chmod +x trap. |
| #131 | Documentation arc | Added `Size budget amendment workflow` + `CI step layout` sections to README.md and ARCHITECTURE.md so the next contributor can find the amend helper + step numbering convention without reading commit history. |

**CI step layout (post-#128, 18 explicit steps):** Build → Bit-exact regression → Bridge regression test (step #6, pre-flight for the gate) → Amend helper regression test (step #7) → Unit tests → Multi-seed integration → Live-mode smoke → Cross-compile Windows → UPX-pack Windows → Binary sizes report → **Binary size budget gate (step #14, 3-key gate against the binary-sizes.txt artifact)** → Coverage report → Coverage gates → Upload Windows binary → Upload coverage log. UI step numbers are 1-indexed from the auto-prepended `Set up job` (header comment in `ci.yml` documents this).

**Verification:** the post-#131 main CI run shows all 18 explicit steps passing, with the 2 spec↔build enforcement points (Bridge regression test at step #6 as pre-flight, Binary size budget gate at step #14 as measurement) both green. The amend helper (`tools/spec-budget-amend.sh`) was end-to-end exercised in the 035 amend drill: dry-run preview, actual amend, then test rollback verified end-to-end (the test's `git checkout --` recovery path correctly restored the working tree from HEAD).

**Tag:** `v1.2.0-final` annotated tag points at the post-#131 main commit (`150f331`). Marks the closure of the 11-PR arc; future archeology can find the stable reference point without reading commit history.

## Recent: 003 MIDI input surface (ALSA + WinMM)

Live MIDI keyboard input ships. Connect a controller to the synth's live mode — the synth plays your notes mixed over the generative output rather than replacing it.

- **Two backends, identical interface.** Linux uses libasound sequencer (`snd_seq_open INPUT` + `snd_seq_connect_from` + polling worker pthread). Windows uses WinMM (`midiInOpen` + `midiInProc` callback, 4-buffer sysex `MIDIHDR` pool). Both deliver events to the same atomic SPSC ring (256 entries, drop counter) consumed by the existing `audio_midi_drain` slot in `mixer.c::render_chunk`.
- **Device routing** (`--midi <N>`, `--midi-default`, `--midi` wildcard, `--midi-list-devices`). Wildcard iterates `snd_seq_query_next_client` + `snd_seq_query_next_port` with a dual CAP filter (`CAP_READ + MIDI_GENERIC|MIDI_KEYBOARD`) per the spec's PR-108 enumerate contract; per-port connect failures are tolerated. Explicit mode decodes `(client << 8) | port` (Linux) / raw device id (WinMM) so `--midi-list-devices` output is directly bindable.
- **Channel filter** (`--midi-channel <1..16>`). Drops events with `channel != N` at drain time, BEFORE the CC dispatch switch — so channel mismatch costs nothing.
- **Scale-degree + velocity mapping (FR-010 / FR-011)**. Notes map through the active scale (`SCALES[cur_scale][K%7]`) the same way the L-system marker does, with octave offset clamped to `[-2, +4]`. Velocity 0 = Note Off. 11-voice pool with Q1 voice-stealing (idle → in-release → oldest).
- **CC dispatch (US2 / FR-020 / FR-022)**. Table-driven `CC_MAP[128]` routes CC#1/7/71/74/91/93 to the corresponding synth parameter; all other CCs (including CC#64, CC#123, General Purpose 1–4) are silently dropped per Principle VII. Delta = `(V - 64) * scale`, summing additively across multiple CCs to the same param. Compressor threshold runtime-tunable.
- **Disconnect (FR-034)**. ALSA `PORT_EXIT` / `CLIENT_EXIT` and winmm `MIM_CLOSE` set the atomic `g_run` shutdown flag from the producer side; `audio_midi_close` completes teardown on the audio thread. No auto-reconnect — synth continues from internal generative state, CC parameters retain last value.
- **--no-midi byte-identity (FR-050 / FR-053 / Constitution III v1.0.1)**. `audio_midi_init(-1)` keeps `g_enabled = 0` in BSS, so `audio_midi_enqueue` and `audio_midi_drain` short-circuit without touching the queue or any sine-table / voice-pool state. The 16-s `golden/regression_16s.sha256` is unaffected by `--no-midi`, the `--midi` / `--midi <N>` paths, or the absence of any MIDI flag at all.
- **Tests.** 23 unit tests across `tests/unit/test_midi.c` (`midi_scale_degree_mapping`, `midi_velocity_scaling`, `midi_octave_clamp`, `midi_voice_stealing`, `midi_no_midi_byte_identical`, The CC dispatch family T025a-k, `midi_cc_multi_sums_additive`, `midi_channel_filter_drops_non_matching`, `midi_list_devices_*`, `midi_note_on_off_live_dispatch`, `midi_open_close_round_trip`, `midi_open_wildcard_sentinel`). T019 directly hashes `golden/regression_16s.sha256` as a smoke check on the FR-053 contract.
- **Smoke test.** `tests/test_smoke_live.sh` now runs two sub-checks: audio (PulseAudio) + MIDI (`modprobe snd-seq-dummy` + `./synth --midi --no-ui` for 2s). The MIDI sub-check skips cleanly if snd-seq-dummy isn't available, so CI without the kernel module still passes the audio check.
- **Docs.** `ARCHITECTURE.md` (T041), `README.md` (T040 new `## MIDI` subsection: flag table + mapping contract + CC table + disconnect behavior + discoverability example), `CHANGELOG.md` (this entry, T042), `.specify/feature.json` (T043 new status registry, modeled on the existing `.specify/integrations/*.manifest.json` convention).
- **`audio_midi.c`** is the single source-of-truth for the gate. `g_enabled = 0` flips when `audio_midi_init(-1)` runs (= `--no-midi` opt-out) OR when `audio_midi_open(N)` fails (so `--midi <N>` against an unplugged controller can't leave the synth in a phantom-device state). `audio_midi_close` resets `g_enabled = 0` unconditionally. The atomic SPSC ring uses `__atomic_*_n` per the Constitution III cross-thread memory model.

Five PRs land 003 end-to-end: #102-#106 baseline (MIDI scaffolding + bind stubs + T041 ARCH refresh) → #107 (checklist sync, 36 [X] flips + 12 honest gaps) → #108 (T034/T035 platform enumerate + `MIDI_LIST_DEVICES_CAP`) → #109 (`--midi <N>` bind routing + `g_enabled` reset-on-failure fix).

## Recent: spec-kit bootstrap + Constitution III amendment to v1.0.1

The `001-stretto-baseline` capability surface now has the full spec-kit artifact set under `specs/001-stretto-baseline/`. Five new / refreshed documents:
- `plan.md` — C99 synth wired up, ≤48 KB UPX + 128 KB arena fits, all 10 principles PASS, project structure (single fixed-binary layout per plan-template Option 1), complexity tracking table empty (no principle violations).
- `research.md` — Phase 0 synthesis: twelve architectural decisions with rationale + alternatives considered. Documents why the synth picks Karplus-Strong + FM + wavetable + additive + super-saw + drum, the 6-layer generative state, 48 kHz × 2-ch × int16, the 128 KB arena, etc.
- `tasks.md` — seventy tasks organized by user story across Setup / Foundational / US1 / US2 / US3 / Polish (5 + 10 + 25 + 5 + 10 + 15). Each task uses the spec-kit checklist format (`- [ ] TID [P?] [Story?] Description with file path`).
- `quickstart.md` — CLI surface, key map, build commands, platform notes, out-of-scope list. Mirrors the operational info already present in this README + ARCHITECTURE.md.
- `CLAUDE.md` updated so the speckit agent-context marker now points at `specs/001-stretto-baseline/plan.md` (was a free-floating "read the current plan" stub).

Resolved analyze HIGH finding **D1**: Constitution Principle III vs spec SC-002 platform-scope wording mismatch (Principle III said "across runs and platforms"; SC-002 said "on the same platform"). Evidence-based decision (Path A): preserve the cross-platform invariant, document its actual scope. Amendment:
- `.specify/memory/constitution.md` — Principle III rewritten with the precise supported-build-target scope (Linux glibc + Windows winmm, both little-endian x86) and an explicit caveat that the cross-platform invariant holds by code construction (a Windows-side bit-exact regression runner is not currently in CI).
- Constitution bumped **1.0.0 → 1.0.1**, Last Amended 2026-07-06.
- `specs/001-stretto-baseline/spec.md` — SC-002 dropped `… on the same platform` qualifier; FR-020 amended to match (Principle III is non-negotiable so the requirement must align with the principle, not the other way around).
- `specs/001-stretto-baseline/quickstart.md` — same wording fix.
- `specs/001-stretto-baseline/plan.md` — Constitution Check row for Principle III reflects v1.0.1.

Net: no `… on the same platform` qualifier remains anywhere in the spec-kit artifact set.

Docs-only change (no `.c` / `.h` / `Makefile` touched). The bit-exact 16-s SHA-256 regression, unit suite, 4-seed multi-seed integration, live-mode smoke test, per-file coverage gates, and Windows cross-compile + UPX pack cannot regress from this change. The push-time "verify locally before pushing" gate from Constitution Principle VI is the contributor's responsibility before opening the PR. Per Principle VIII this entry documents the WHY (spec-kit pipeline bootstrap + cross-platform invariant scope honest-on-paper), not the WHAT of each file (the WHAT lives in the files themselves).

## Recent: per-section voice-family masking

- Sections now differ by which voice families play, not just by parameter bias. `section.c` adds a 7-bit voice-family mask (`section_voice_mask`): BODY/TENSION full ensemble, RESOLVE drumless, INTRO a randomized 1–3-voice subset from `INTRO_COMBOS[8]` chosen once per 96-bar cycle (seed-deterministic via a `prng()` draw in `schedule_bar_boundary` + the opening one in `gen_init`).
- `gen.c` schedulers consult the mask before triggering — but only the trigger calls are gated, never the PRNG / L-system / Markov / motif updates, so masking silences a voice without altering the rest of the piece.
- INTRO now opens sparse and oriented instead of full-density. Measured INTRO-vs-BODY RMS ratio 0.41–0.87 across seeds.

## Recent: portamento (glide) on super-saw bass

- Legato SUB bass re-triggers (previous note still above half sustain) slide ~50 ms to the new pitch instead of jumping. New top-level Voice fields `inc_target` / `glide_remain`; `voice_step` walks the oscillator increment linearly, rebuilding the ±detune each step. No extra PRNG draws.

## Recent: chord arpeggio (TENSION)

- TENSION chords arpeggiate — 8 events/bar cycling up the 3 voicing notes — instead of block triads. `section_chord_arpeggio` gates it; other sections stay block. Coverage-render bumped 16 s → 110 s so the TENSION branch is exercised.

## Recent: super-saw subtractive bass

- Bass switches from 1:1 FM to VOICE_SUB: 3 detuned band-limited saws (≈±0.78 %, reusing `WAVETABLE[4]`) summed and run through the SVF. Thicker, wider bass. No new `.rodata`.

## Recent: additive chord voice (BODY)

- VOICE_ADD: 8 sinusoidal partials at integer harmonics weighted by one of 4 drawbar profiles (`ADD_PROFILES[4][8]`). Used by the BODY section for an organ-like pad.

## Recent: wavetable chord voice (INTRO/RESOLVE)

- VOICE_WT: reads `WAVETABLE[8][256]` (built by `gen_wavetable.c`), linearly interpolating between adjacent single-cycle waveforms at a position swept by the per-voice pan LFO — an animated morphing pad. Section state machine selects chord synthesis per section (INTRO/RESOLVE wavetable, BODY additive, TENSION FM).

## Recent: master compressor + brickwall limiter

- New `compressor_process()` in `effects.c`. Feed-forward, stereo-linked envelope (drives both channels from `max(|L|, |R|)`). 4:1 ratio above threshold, ~5 ms attack / ~200 ms release at 48 kHz, +1 dB makeup gain, brickwall ceiling at 32 000.
- Master chain becomes: voice mix → reverb → delay → soft sat → compressor → `sat16`.
- Threshold runtime-tunable in `[8000, 30000]` via `l` / `L` keys; default 20 000.
- Status row gains `Lm:<n>` field.
- 5 new tests in new `tests/unit/test_effects.c`.
- Multi-seed clip count drops from "≤100 per render" to **0** on every seed.

## Recent: spec-kit + baseline spec

- Installed GitHub Spec Kit (`.specify/`, `.claude/skills/speckit-*`, `CLAUDE.md`).
- Wrote `.specify/memory/constitution.md` v1.0.0 encoding the 10 principles the project already enforces (3 NON-NEGOTIABLE: size budget, determinism, test discipline).
- Wrote `specs/001-stretto-baseline/spec.md` documenting what stretto does today across 3 prioritized user stories + functional requirements + measurable success criteria.

## Recent: long-term motifs

- New `motif.c` / `.h`. Ring buffer of the last 8 four-bar main-melody phrases (512 B + ~13 B state). Every ~30 bars with ~25% per-bar probability, replay one (verbatim or ±2 diatonic transpose) instead of L-system output for 4 bars.
- `gen.c` integrates: `gen_init` calls `motif_init`; `schedule_bar_boundary` calls `motif_bar_step`; `schedule_melody` swaps L-system for `motif_replay_at` (snapped to active mask) when `motif_in_replay()`.
- Counter-melody continues its 2nd-order Markov walk during replay — familiar main over a fresh counter is the contrast that makes the replay feel like a return.
- Status row gains `Mo:c` / `Mo:r` field (capture / replay).
- 8 new tests; `motif.c` at 97%.

## Recent: inter-voice listening

- Counter-melody now reads `cur_degree` (main melody's most recent degree) and biases the 2nd-order Markov walk away from unison and toward 3rd/6th consonances. Per-degree weight multiplied by an interval-consonance factor before sampling: unison × 0, 2nd × 64, 3rd × 192, 4th × 128.
- Bass plays a one-step diatonic approach note at the very first bass event of every new chord. Direction follows the previous chord root. Listener hears approach → root resolution instead of bass jumping between chord roots.
- `chord_progression.h` exposes `chord_progression_get_prev_root()`.
- Voices stop sounding like 5 parallel streams.

## Recent: 2nd-order Markov for counter-melody

- 1st-order `markov[7][7]` replaced with 2nd-order `markov2[7][7][7]` (343 B vs 49 B). Indexed by `[prev_prev][prev][next]`.
- Seeded at `gen_init` by replicating the existing 1st-order tuning across the prev_prev axis so day 1 character matches the old chain.
- `mutate()` drifts cells of `markov2` so distinct `(prev_prev, prev)` contexts evolve different transition tendencies over the piece.
- Pre-first-mutation behavior is byte-identical to the old code (multi-seed 4-second hashes unchanged); 16-second hash regenerated.

## Recent: Tier-3 cleanup wave (4 PRs)

Finished the post-audit Tier-3 cleanup:

- **config.h + named constants** — `SAMPLE_RATE` / `BUFFER_FRAMES` centralized; `gen_note_table.c` now reads them too (was duplicated). Three magic numbers in `voice.c` named (`KS_AVG_COEF`, `CUTOFF_LFO_SHIFT`, `CUTOFF_FENV_GAIN` / `CUTOFF_FENV_SHIFT`).
- **Test/build/bias cleanup** — `test_init_synth()` in `test.h` replaces five per-file setup helpers (legacy names kept as macro aliases so existing TEST bodies don't change). New `make debug` target builds `synth_debug` with `-O0 -g -DDEBUG` and separate `.dbg.o` filenames so it co-exists with the release build. `effective_mutate_interval()` centralizes the mutation-bias math that was inline at the gen_step call site.
- **`gen_step` decomposition** — was ~210 LOC, now ~25 LOC dispatcher delegating to five static-inline schedulers (`schedule_bar_boundary`, `schedule_drums`, `schedule_bass`, `schedule_chord`, `schedule_melody`) plus `compute_active_mask`. LTO folds the inlines; Windows binary size unchanged.
- **Status row decomposition** — `ui_draw_oscilloscope` was 138 LOC doing two jobs; split into `build_status_row` and `build_oscilloscope_grid` with shared `append_num` / `append_str` helpers and file-scope `COL_*` ANSI macros.

All four refactors are mechanical — bit-exact regression passes with the existing golden across every PR.

## Recent: adaptive density

- New `density.c` / `.h` module. `tension = popcount(active_mask) * 18 + gate_prob >> 2`. Counter-cyclical biases: high tension yields negative gate/reverb deltas (pull back when busy), low tension yields positive (fill in when sparse).
- Composes with `section.c` additively. Both modules' reverb biases sum before being pushed to `effects.c::reverb_set_wet_bias`. Gate bias adds to the section + user value at the melody trigger clamp step.
- Pure function of current bar state — no PRNG, no persistent state beyond the cached tension byte. `--seed N` reproducibility preserved.
- Status row gains `Td:<n>` field (yellow).
- 7 new unit tests, `density.c` at 100%. Total unit tests 88 → 95.

## Recent: unit tests for mixer / wav / keys

- 20 new tests across `tests/unit/test_mixer.c` (3), `tests/unit/test_wav.c` (2), `tests/unit/test_keys.c` (15). Covers `render_chunk` contract, WAV header bytes against RIFF/WAVE/fmt/data spec, and every key dispatcher branch.
- `effects.c` coverage jumped 82% → **100%** (test_keys exercises every effects setter).
- `main.c` coverage rose to 96%.
- CI gates raised: `effects.c` 80% → 95%, `main.c` 60% → 90%.
- `ui.c` gains a small ergonomic fix: `ui_show_help` and `ui_clear_screen` early-return when `no_ui` is set, matching the existing `--no-ui` "no terminal I/O" contract and keeping test output quiet.

## Recent: CI per-file gates + size budget + arena to 100%

- CI workflow gates per file with a threshold map: pure-synth modules (arena, voice, gen, section, mixer) at ≥95%; lsystem / chord_progression / wav at ≥90%; effects at ≥80%; main at ≥60%. Tightens over time as coverage grows.
- Interactive modules (ui.c, keys.c, audio_pulse.c) excluded from measurement; comment in the workflow documents why.
- New CI step: Windows packed binary size budget of 48 KB (current is 32 KB; "<64 KB" stated goal). PRs that exceed the budget fail.
- arena.c driven from 80% to 100% by a new fork-based test that verifies the OOM exit path. Total unit tests: 68.

## Recent: coverage build isolated under build_cov/

- `make coverage` now writes every artifact (instrumented `.o`, `.gcno`, `.gcda`, `synth_cov`, `.cov` test binaries) into `build_cov/`. The repo root sees only the normal build's outputs.
- `make coverage` and `make test-unit` can be alternated without `make clean`. The known issue documented in earlier commit messages is fixed.
- Silent bug fix: unit-test coverage was previously linking the non-instrumented `.o` files (so coverage was never aggregated correctly). Now correctly uses the instrumented subset.
- `.gitignore` extended to cover `*.d` (gcc -MMD output), `build_cov/`, `synth_cov`, `*.gcda/gcno/gcov`, and unit-test binaries.

## Recent: main.c split into ui / audio / mixer / wav / keys

- `main.c` 783 → **78 LOC** (90% reduction). Six new modules carve out tightly-scoped concerns:
  - `mixer.c` (19 LOC) — `render_chunk()`, single source of the master-bus chain.
  - `wav.c` (52 LOC) — `render_wav()` + WAV header.
  - `ui.c` (308 LOC) — terminal raw mode, oscilloscope, status row, help overlay.
  - `keys.c` (47 LOC) — key dispatcher. Removed ~50 LOC of duplication between the Linux and Windows audio backends.
  - `audio_pulse.c` (147 LOC) — Linux PulseAudio playback.
  - `audio_winmm.c` (102 LOC) — Windows waveOut playback.
- `audio.h` provides a one-function interface (`audio_play()`); platform backend selected at link time.
- Bit-exact regression PASSES with the existing golden — the refactor is purely mechanical.

## Recent: Makefile pattern rules + auto-deps

- Replaced 16 hand-written per-file recipes with 2 pattern rules (`%.o: %.c`, `%.win.o: %.c`).
- Added `-MMD -MP` so gcc auto-generates `.d` files capturing real `#include` dependencies; the trailing `-include` picks them up.
- Adding a new module is now 2 lines (OBJS and WIN_OBJS). Header-dependency bugs become impossible.
- `OBJS_NO_MAIN = $(filter-out main.o,$(OBJS))` so the unit-test link line auto-extends with new modules.

## Recent: effects extraction

- New `effects.c` / `effects.h` module pulls master-bus delay, Schroeder reverb, soft saturation, and the shared `sat16` helper out of `main.c`.
- Removed the three-file weak-stub workaround that the section-bias work had introduced. `reverb_set_wet_bias()` is now a normal exported function.
- `voice.c`'s two inline int16-saturation expressions replaced with `sat16()` calls (no behavior change; less duplication).

## Recent: song-section state machine

- New `section.c` / `section.h` module — 4-section state machine (INTRO → BODY → TENSION → RESOLVE), 96-bar full cycle (24 bars per section).
- Per-section biases on gate, filter cutoff, reverb wet, mutation interval (continuous, crossfade across 8 bars centered on each boundary); kick pattern + L-system character (discrete, switch at boundary).
- Determinism preserved (section is a pure function of `bar_count`, no PRNG).
- Status row gains `Sec:<name>` field. 10-minute renders now have audible long-form shape.

## Recent: probabilistic chord progressions

- New `chord_progression.c` / `.h` module. Chord function root advances every 2 bars via a Markov chain. All chord triggers within those 2 bars share the same root, so harmonic motion happens at a slow ambient pace rather than per-bar churn.
- Two 7x7 weight tables (~98 B `.rodata`):
  - `CHORD_MARKOV_MAJOR` for Lydian / Mixolydian - biased toward V to I, IV to I, vii to I, ii to V.
  - `CHORD_MARKOV_MINOR` for Dorian / Phrygian / Locrian / Harmonic Minor - modal-friendly VII-i, iv-i, no strict dominant pull.
- Chord trigger rebases each voicing degree: `(pattern[i].degree + current_root) % 7`. Voice-leading octave-shift unchanged.
- Bass tracks the current chord root - its root/fifth alternation now follows the chord function rather than always playing scale degree 0/4.
- One-way coupling: gen.c passes `prng()` output and `cur_scale` to `chord_progression_step()`. The module never reads gen.c state.
- Status row gains `Cr:<n>` field (current chord root 0..6).
- 5 new unit tests in `tests/unit/test_chord_progression.c`. Coverage on the new module: 91.3%.

## Recent: L-system melodic phrase generator

- New `lsystem.c` / `.h` module replacing the Markov walker on the main melody. Counter-melody keeps Markov so the two lines contrast (phrased vs walked).
- 6-symbol alphabet (u, U, d, D, r, .) over scale-degree relative moves + rests.
- 3 hand-tuned characters (stepwise, leaping, sparse) selected by mutation. Each character has a 6-rule production table.
- 3-generation rewrite into a 256-byte output buffer per `lsystem_reset()`. Walker reads sequentially, snaps each pointer position to the nearest in-mask degree, returns `LSYSTEM_REST` for the rest symbol so the caller skips the trigger (breathing).
- `mutate()` calls `lsystem_mutate()` with ~33% probability per event. Mutation re-rolls one rule's RHS (50%), cycles character (25%), or swaps an axiom symbol (25%) - drift the melodic style across the piece.
- Memory cost: ~410 B static state.
- 6 new unit tests in `tests/unit/test_lsystem.c`. Coverage on the new module: 93.1%.

## Recent: testing + CI

- Hand-rolled unit-test framework at `tests/unit/test.h` (~130 LOC, `TEST(name) {...}` registration via constructor attributes plus assertion macros).
- 49 unit tests across `tests/unit/test_arena.c`, `test_voice.c`, `test_gen.c`. Coverage: `arena.c` ~80%, `voice.c` ~98%, `gen.c` ~97%.
- `tests/test_multi_seed.sh` renders 4 seeds, asserts determinism, distinct sha256s, audio characteristics within bounds, plus golden-hash regression in `golden/regression_multiseed.sha256.txt`.
- `tests/test_smoke_live.sh` spawns `./synth --no-ui` under a 2 s timeout, accepts clean exit / SIGTERM, fails on segfault, auto-skips without PulseAudio.
- New Makefile targets: `test-unit`, `test-multiseed`, `test-smoke`, `coverage`, `golden-multiseed`.
- GitHub Actions CI (`.github/workflows/ci.yml`) runs the full suite on push and pull-request to `main`, plus Windows cross-compile. Uploads `stretto.packed.exe` and coverage log as build artifacts. Per-file coverage gate at 80%.

Two bug fixes uncovered by the new tests:

- `gen_seed(0)` and `gen_seed(1)` produced identical state. The old ternary `hash32(seed ? seed : 1u)` collapsed both to `hash32(1)`. Fix: XOR the seed with `0xDEADBEEFu` before hashing so all seeds map to distinct chains.
- `tcgetattr` exited synth with code 1 when stdin had no TTY (script invocation, smoke test). `--no-ui` now skips `term_raw_mode` entirely; live mode without TTY runs cleanly until SIGTERM.

## Recent: cleanup pass

- Removed dead `drum_attack` / `drum_release` placeholder arrays in `voice_pool_trigger_drum` that were marked `(void)` for-future. The actual per-drum-type release table is in `env_step`; per-drum attack is just `role_attack[ROLE_DRUM]`.
- Pruned Makefile `clean` target of legacy experiment artifacts (synth.upx, synth.test, synth.orig, synth.unpacked, synth.xz, synth.lto.o, synth_xz.h, start.c, stub.c) that haven't existed for many PRs.
- Reorganized `.gitignore` into labeled groups; added Windows artifacts (stretto.exe, stretto.packed.exe, *.win.o) that were previously showing as untracked.

## Recent: filter controls

- Runtime cutoff and resonance, live-tunable via `c`/`C` and `n`/`N`.
- Per-role cutoff/Q offset tables (bass darker, melody open, drums heavily damped).
- Per-voice cutoff LFO modulation, reusing the existing per-voice pan LFO at zero new state. Depth adjustable live via `m`/`M`.
- Multi-mode filter: LP / HP / BP / notch, cycled via `t`. SVF natively computes all four outputs; `voice_step` selects.
- Chord-voice filter envelope: separate ADSR feeds cutoff modulation only for chord voices. Each chord trigger sweeps the filter open then closed.
- `mutate()` drifts cutoff and resonance ~50% of the time it fires, so the timbre evolves over long timescales.
- Status row gains `F:<cutoff> N:<resonance> L:<lfo_depth> T:<mode>` fields.
- User base ranges deliberately tighter than effective-value clamps so LFO + filter-envelope modulation always lands without losing swing at the top of the dial:
  - `svf_f_base`: user [30, 180], effective clamp [20, 230].
  - `svf_q_base`: user [0, 180], effective clamp [0, 220].

## Recent main-branch work (since stereo)

### Windows port
- Native Windows binary `stretto.exe` via MinGW cross-compile (`make win`). No WSL involved at runtime.
- Win32 `waveOut` audio backend on Windows (4 cycling buffers of 1024 frames, CALLBACK_EVENT, links `winmm.lib`). Bypasses WSLg's broken RDP audio pipe entirely.
- Platform-abstracted terminal layer (`term_get_size` / `term_read_key` / `term_raw_mode` / `term_restore_mode`) so the oscilloscope + status row + key handler work the same on Linux and Windows. Windows uses `SetConsoleMode` with `ENABLE_VIRTUAL_TERMINAL_PROCESSING` for ANSI escapes and `_kbhit`/`_getch` for non-blocking key input.
- Size-optimized Windows build: 529 KB → 30 KB packed via `-Os -flto + section gc-sections + strip + UPX`. `make winpack` packs in one step.

### Audio quality
- 48 kHz native sample rate (was 44.1 kHz). WSL/PulseAudio's 44.1 → 48 resampler was broken on the user's machine; switching native eliminates that path. All sample-count constants (envelope timings, reverb delays, samples per substep, LFO increments) rescaled to preserve identical musical timing at the new rate.
- Stereo output with per-voice panning. Voices have role-based base pan positions plus a slow per-voice LFO for continuous motion in the stereo field. Linear pan law, applied at mix time.
- Per-voice peak normalization. 50 ms measurement window after each trigger; gain scales the voice output toward a common peak target (16000) with a 4× cap. Equalizes perceived loudness between bass (FM 1:1, soft) and chord (FM 2:1, bright).
- LFO pitch detune on FM voices (~5 cents peak chorus). Reuses the existing pan LFO at zero state cost.
- Master-bus stereo delay (250 ms, two independent buffers per channel) with feedback (default ~0.55).
- Master-bus Schroeder reverb. 4 parallel comb filters → 2 series all-pass per channel, with prime delays slightly different L/R so the tail keeps stereo separation. RT60 ~1.5 s.
- Soft cubic saturation `y = x - x^3 / 2^31` on the master bus. Transparent at typical levels; smoothly compresses peaks.

### Generative
- Time-seeded PRNG by default — every launch sounds different. `--seed N` overrides for reproducible runs (used by the regression test).
- Dynamic mutation rate. Triangle LFO sweeps the interval between 1 bar (busy) and 16 bars (calm) over a ~4.3-min cycle so the piece alternates between dense and sparse sections.
- True 3-against-4 polyrhythm via 48-substep bar (was 16-step). Bass 3 events vs chord 4 events fit cleanly. LCM(3, 4, 16) = 48.
- Bouncing bass — 4 events per bar at substeps 0, 18, 24, 42 (long-short alternating). Beats 1 and 3 anchor tempo; offbeats anticipate. Alternates root/fifth with bar parity swap.
- Voice leading on chord triggers. Each new chord pitch octave-shifts toward the running chord centroid for stepwise motion instead of leaps.
- Counter-melody: second melodic line on its own Euclidean rhythm + Markov walk, transposed +12 semitones to occupy a different register.
- Chord voicing variety: triad / seventh / sus4 / sus2 / inv1 / inv2 rotating per bar.
- Probability gates on Euclidean hits. `gate_prob` (default 200/256) drops some hits for melodic breath; drifts via mutation; live-tunable with `g`/`G`.

### Scales
- Six modes: D Dorian, D Lydian, D Phrygian, D Locrian, D Harmonic Minor, D Mixolydian. The `s` key cycles between them in live mode. Scale never changes automatically. Status row shows current scale.

### Drums
- 11-voice pool (was 8): added kick (slot 8), snare (9), hihat (10) as new `VOICE_DRUM` type with `ROLE_DRUM`.
- Synthesis: kick is a sine pitch-sweep + 5 ms noise attack click; snare is noise-dominant (90/10) with a 200 Hz body; hihat is pure white noise. Per-drum-type linear envelope releases (kick 150 ms / snare 100 ms / hihat 30 ms).
- Rotating pattern banks: kick 4 patterns, snare 3, hihat 5. Coprime sizes → LCM(4, 3, 5) = 60 bars before exact repeat.
- Per-drum-type post-normalization gain (kick 3×, snare 2.5×, hihat 1.5×) so drums sit on top of the harmonic mix.

### UI
- Color heat-map oscilloscope (silence dim gray → blue → cyan → green → yellow → magenta → red peak), single `write()` syscall per frame.
- Expanded status row: M (mod_depth), S (scale), V (11 activity dots colored by role), G (gate), R (reverb wet), D (delay wet/feedback), deg (Markov walk position), act (active scale-degree mask), chord (current voicing name).
- Help screen toggled by `?` listing all live keys.

### Polish
- ALSA → libpulse-simple → pa_stream + threaded mainloop on Linux. Final libpulse architecture matches paplay's exactly.
- ALSA latency bumped to 300 ms while the WSLg crackling investigation was ongoing; retained as the PulseAudio buffer target.

## Pre-stereo work (recent main-branch)

### Code quality / cleanup
- Removed dead `voice_pool_trigger` (non-role variant); only the role-aware API is used.
- Inline rationale comments at four points that previously required reading commit history: SVF int32 widening, `MUTATE_BARS` deviation from PLAN.md, Markov weights, Rule 110 + Rule 30 pairing.
- ALSA underrun recovery via `snd_pcm_recover` (later replaced when moving to libpulse, then to waveOut on Windows).
- `--render <seconds>` input validation via `strtol`.
- Makefile housekeeping: `STRIP_TARGET` / `PACK_TARGET`, `UPX_BIN` / `UPX_FLAGS`, `make clean` removes abandoned experiment artifacts.

### Earlier synth features
- True Bjorklund Euclidean rhythm masks replace the floor-distributed approximation.
- Per-voice roles: bass / chord / melody with role-scoped envelope and synth parameters.

### Earlier audio fixes
- SVF state widened `int16_t` → `int32_t` with int16 saturation at output. Eliminated resonance-wrap clicks (395 → 0 per 16 s render).
- Default FM `mod_depth` reduced 6000 → 1500 to cut aliasing (HF energy >8 kHz dropped 45% → 12%).

## Phase 5 (PR #4)

- Terminal UI: ASCII oscilloscope, raw stdin via termios, atexit restore.
- Live keyboard controls: SPACE (force mutate), `+`/`-` (tempo ±10%), `q` (quit).

## Phase 4 (PR #3 series)

- UPX `--ultra-brute` packing; binary fits demoscene-tier sizes.
- Direct ioctl path on `/dev/snd/pcmC0D0p` explored, then reverted to libasound for WSL compatibility (PR #6).

## Phase 3 (PR #2)

- Generative MVP per PLAN.md section I:
  - Rule 110 CA evolves active-degree mask per bar.
  - First-order Markov chain over 7 D-Dorian degrees.
  - Two parallel Euclidean rhythm masks combined for melody triggers.
  - Mutation every 4 bars (deviation from PLAN.md's 16; documented).
- Build adds `gen_euclid_table.c` as a fourth build-time generator.

## Phase 2 (PR #1)

- Static `pool[65536]` arena with 8-byte-aligned bump allocator.
- Voice struct unioning Karplus-Strong and 2-op FM variants. ADSR per voice.
- 4-voice polyphony, hard-coded C-major arpeggio for testing.
- Build-time generators for envelope curve and per-MIDI-note tables.

## Phase 1 (PR #1)

- Hard-coded 440 Hz sine via 1024-entry int16 LUT.
- ALSA `snd_pcm_writei` live playback at 44.1 kHz S16_LE mono.
- `--render <seconds> <out.wav>` mode.
- Bit-exact regression test (`tests/test_bitexact.sh`, `golden/regression_16s.sha256`).

## Initial commit

- `PLAN.md` design document and project skeleton.
