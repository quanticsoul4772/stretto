# Research: 003-midi-input (MIDI Input)

**Date**: 2026-07-06 · **Branch**: `003-midi-input` · **Spec**: [spec.md](./spec.md)

This document records the 7 architectural decisions made during Phase 0 of the `/speckit-plan` workflow for the MIDI input feature. Each decision cites the relevant Constitution principle and existing code surface; alternatives are documented with rejection rationale for future spec authors.

---

## D1. ALSA input pattern

**Decision**: `snd_seq_create_thread()` + callback handler. The ALSA-managed thread calls our callback for every incoming sequencer event; the callback enqueues `midi_event_t` to the shared ring buffer.

**Rationale**: This matches the spec's Q2 answer (callback-only on both platforms, see `spec.md:14`). ALSA's `snd_seq_create_thread()` spawns a thread managed by ALSA itself, so we don't need a raw `pthread_create` (Constitution V: no extra threads outside the existing PulseAudio threaded mainloop). The callback runs in a well-defined context; the audio thread never enters ALSA APIs. ALSA libasound is dynamically linked via `-lasound` in the `synth` and `synth_cov` Makefile targets (mirrors the existing `-lpulse` dynamic-link pattern at `Makefile:104`, `Makefile:207`).

**Alternatives considered**:
- *B: Dedicated pthread looping on `snd_seq_event_input()`*. Rejected: introduces a second user-managed thread in the audio path (Constitution V prefers library-managed concurrency where possible); the `pthread_*` symbols are already pulled in via libpulse but adding a hand-rolled thread + cancel-safety contract expands the per-event allocation path. ALSA's managed thread is functionally equivalent for this use case.
- *C: Audio-thread polling via `snd_seq_event_input_pending()` inside `render_chunk`*. Rejected: violates FR-033 ("the audio thread is never blocked on platform-specific I/O") — the audio thread would call ALSA APIs, and even non-blocking poll adds per-buffer overhead. Also creates a determinism hole (the audio thread reading the ALSA event queue would interleave with the engine's render path in a way that depends on event-arrival timing, complicating Constitution III).

**Confidence**: HIGH. This is the standard ALSA pattern for `snd_seq`-based MIDI input, and the synth's existing `audio_pulse.c:46-58` pattern (library-managed thread + callbacks + signals) is the template.

---

## D2. Win32 input pattern

**Decision**: `midiInOpen()` + `midiInStart()` + `midiInProc` callback. `midiInGetDevCaps()` for device enumeration. The `midiInProc` callback enqueues to the same shared ring buffer used on Linux.

**Rationale**: Win32 MIDI input is callback-only — there is no blocking-read or polling API equivalent to ALSA's sequencer. The `midiInProc` callback runs in a system-managed thread created by `midiInStart()`. `winmm` is **already** linked in the Windows build at `Makefile:62` (`-lwinmm`) for `waveOut*`; no new runtime dep, no binary size cost. The CALLBACK_FUNCTION flag is required for the `midiInProc` callback path.

**Alternatives considered**:
- *B: midiStreamOpen + MMRESULT-based polling*. Rejected: midiStream is for output, not input. For input, the callback path is the only choice.

**Confidence**: HIGH. Single API path; no choice to make.

---

## D3. SPSC ring buffer in C99

**Decision**: GCC `__atomic_*` built-ins (`__atomic_load_n` / `__atomic_store_n` with `__ATOMIC_ACQUIRE` / `__ATOMIC_RELEASE`) for the ring buffer's head pointer + a `_mm_mfence` style full fence on the producer side. The consumer (audio thread) reads head once at the start of `audio_midi_drain()` and drains all events up to that head before advancing its local tail. Power-of-two size (256 events) so the index wrap is a `& 0xFF` mask.

**Rationale**: The engine is C99 (Constitution II). The Makefile compiles with gcc's default dialect (gnu11+ on ubuntu-latest). GCC's `__atomic_*` built-ins are documented as C99-compatible gcc extensions — they work in `-std=c99` mode as well as `gnu11`. On x86 (both Linux and Windows MinGW targets per Constitution III v1.0.1), these built-ins compile to plain loads/stores for naturally-aligned `uint32_t` on the producer side and to a single `mov` on the consumer side. No `<stdatomic.h>` (C11) is needed. The single-producer/single-consumer invariant (callback = producer, audio thread = consumer, per FR-033) means we don't need `__atomic_compare_exchange` (CAS) — just an acquire-load on the consumer and a release-store on the producer.

**Alternatives considered**:
- *B: C11 `<stdatomic.h>` with `atomic_uint`*. Rejected: technically violates "C99 Only" (Constitution II) even though gcc supports it in any dialect. Mixing C11 atomics in a C99 codebase would be an unnecessary dialect exception.
- *C: `volatile uint32_t` + `__sync_synchronize()`*. Rejected: `volatile` is not a synchronization primitive in C — it only prevents the compiler from optimizing away the access. `__sync_*` built-ins are deprecated in gcc 11+ in favor of `__atomic_*` and emit a `-Wdeprecated-declarations` warning. Using the deprecated form is poor practice even if functional.
- *D: Plain non-atomic uint32_t + aligned access (relying on x86 natural memory ordering)*. Rejected: the compiler is allowed to reorder the producer's event-store and head-store if it cannot prove they're observed together. In practice gcc doesn't reorder at `-Os`, but a future compiler change or `-O2` build would silently break the invariant. The acquire/release fence is cheap (a single `mfence` on x86) and removes the reliance on compiler behavior.

**Confidence**: HIGH. Standard pattern, well-documented, portable to any gcc target the project supports.

---

## D4. Voice pool integration path

**Decision**: Add two new entry points: `voice_pool_trigger_midi(uint8_t note, uint8_t vel)` and `voice_pool_release_midi(uint8_t key, uint8_t channel)`. Extend `Voice` with two new fields: `trigger_key` (the MIDI key that started this voice, for Note-Off matching) and `trigger_channel` (the MIDI channel, 1..16, for channel filter + matching). `voice_pool_trigger_midi` walks all 11 voices (`N_VOICES` in `voice.h:8`) as a non-role-scoped pool, applying the Q1 voice-stealing rules (oldest in-release; if none, oldest regardless of state).

**Rationale**: The existing `voice_pool_trigger_role(note, type, role)` at `voice.c:649` is role-scoped via `role_slot_start[role]` / `role_slot_end[role]` at `voice.c:632-633` — the 4 bass slots, 3 chord slots, 3 melody slots, 3 drum slots are disjoint. Calling it with `role=ROLE_MELODY` would only use 3 voices, not 11. Per Q1 (`spec.md:13`), the MIDI pool is a single non-role-scoped 11-voice pool (`N_VOICES` in `voice.h:8`). Adding a dedicated entry point is the smallest change that honors Q1 without disturbing the existing role-scoped engine code path that `gen.c` uses for the generative scheduler.

The `trigger_key` / `trigger_channel` fields are needed for Note-Off matching (FR-012 / FR-013). Without them, a Note Off cannot find its corresponding voice. The fields are written by `voice_pool_trigger_midi` and read by `voice_pool_release_midi`; they don't affect the existing role-scoped voice allocation.

**Alternatives considered**:
- *B: Generalize `voice_pool_trigger_role` to accept a `role_mask`*. Rejected: changes the function signature that `gen.c` (the generative scheduler) uses; would require updating every call site in `gen.c` and risks subtle behavior changes in the engine. The dedicated `*_midi` entry point is a smaller, isolated addition.
- *C: Track MIDI note → voice mapping in a side table in `audio_midi.c`*. Rejected: requires synchronizing the side table with the voice pool's slot-allocation logic, doubling the bookkeeping. Adding 2 bytes to `Voice` is simpler and Constitution V's "no extern declarations across module boundaries" is satisfied since the new fields are inside `voice.h`.

**Confidence**: HIGH. Smallest reasonable refactor; honors Q1; preserves existing engine behavior.

---

## D5. libasound binary size cost

**Decision**: Dynamic link via `-lasound` in the `synth` and `synth_cov` targets. Mirror the existing `-lpulse` pattern at `Makefile:104` and `Makefile:207`. Document the new runtime dep in `README.md` + `quickstart.md` (Phase 1 deliverable). The Windows build needs no change (winmm is already linked at `Makefile:62`).

**Rationale**: Static-linking libasound would add ~1 MB to the binary — blowing the 24 KB / 48 KB budget. Dynamic linking keeps the binary at the existing size (libasound's symbol references become entries in the dynamic loader's table, ~few hundred bytes; the actual libasound.so is loaded at runtime). GitHub Actions `ubuntu-latest` has `libasound2-dev` preinstalled; existing CI runners don't need new apt steps. The smoke test's auto-skip pattern (mirroring `tests/test_smoke_live.sh` which auto-skips when PulseAudio is missing) handles CI runners without libasound without breaking `make test-smoke`.

**Alternatives considered**:
- *B: Pure rawmidi (snd_rawmidi_*) instead of sequencer*. Rejected: rawmidi is a byte stream, doesn't include MIDI 1.0 channel/status parsing or system-routing. The sequencer API is the right abstraction for USB-MIDI controllers + virtual loopback.
- *C: PipeWire MIDI*. Rejected: adds a third runtime dep; not first-class on all Linux distros; ALSA sequencer already works on top of PipeWire via the kernel's ALSA sequencer compat layer.

**Confidence**: HIGH. Standard practice; mirrors existing pattern.

---

## D6. Test plan

**Decision**:

1. **`tests/unit/test_midi.c` (new)** — 6+ test cases:
   - `midi_scale_degree_mapping` — verifies K%7 → SCALES[cur_scale][K%7] (FR-010)
   - `midi_velocity_scaling` — verifies V/127 amplitude clamp to [64, 32767] (FR-010)
   - `midi_cc_mapping` — verifies CC#1 → cutoff, CC#7 → compressor threshold, etc. (FR-020)
   - `midi_ring_buffer_enq_deq` — verifies single-producer/single-consumer roundtrip; overflow behavior (oldest event dropped, head advances)
   - `midi_channel_filter` — verifies `--midi-channel N` filter routes only channel N (FR-004)
   - `midi_no_midi_byte_identical` — verifies that with `midi_init(0)` not called (i.e., the `--no-midi` path), `render_chunk` produces byte-identical output to a baseline run (FR-050 / FR-053)

2. **`tests/test_smoke_live.sh` extension** — new step before the existing PulseAudio step:
   - `load_module snd-seq-dummy` + `modprobe snd-seq-dummy` (or skip if kernel module unavailable)
   - Connect `snd-dummy` MIDI port to a sequencer subscribe
   - Launch `synth --midi --no-ui` in the background
   - `amidi -d -p <client>:<port> -s <NoteOn hex>` to send a Note On
   - Capture 1 s of audio, verify non-zero RMS
   - Auto-skip the entire step if `amidi` or `snd-seq-dummy` is unavailable (echo `[skip] MIDI loopback not available`, exit 0)

3. **Existing tests, no change**:
   - `tests/test_bitexact.sh` (FR-053) — already validates byte-identity for `--no-midi` (no MIDI thread spawned, no callback registered → identical to baseline)
   - `tests/test_multi_seed.sh` — catches audio-characteristic drift across seeds
   - `tests/unit/test_voice.c` — exercises voice pool internals (independent of the new `_midi` entry points)

**Rationale**: The spec already enumerated these test categories in FR-050..FR-054 + SC-002..SC-007. The new unit test file lives in the same `tests/unit/` directory as the existing per-module tests and follows the same `test.h` framework (see `tests/unit/test_voice.c:1` for the pattern). Coverage gate ≥90% on `audio_midi.c` per FR-054. The smoke test step is conditional (auto-skip) so CI without libasound still passes.

**Alternatives considered**:
- *B: Pure property-based test (e.g., QuickCheck) for the ring buffer*. Rejected: Constitution II ("C99 Only") and Constitution VI ("no external test frameworks beyond gcov + bash + python3"). The deterministic, in-process unit test covers the same cases.
- *C: Run smoke test on a real hardware MIDI controller in CI*. Rejected: no CI runner has USB-MIDI hardware. Virtual loopback is the only CI-realistic option.

**Confidence**: HIGH. Test cases are concrete and mirror existing test patterns.

---

## D7. Binary size impact estimate

**Decision**: Expected stripped Linux binary add: **~3-5 KB (M2 fix — original ~1.5-2.5 KB estimate undercounted the dispatch module's weight; `audio_midi.c`'s SPSC-ring + dispatch + CC_MAP lookup + channel-filter + list-devices + `--no-midi` short-circuit logic is heavier than originally assumed)**. Expected Windows packed add: **~4-6 KB** (M2 fix). Both still well within the budget headroom: ~21 KB Linux headroom against the 24 KB soft target; ~24 KB Windows headroom against the 48 KB hard limit.

**Rationale**:
- `audio_midi.h` (~40 lines, no executable code)
- `audio_midi.c` (~250-350 lines: ring buffer, dispatch, CC map table, channel filter, --midi-list-devices, --no-midi path)
- `audio_midi_linux.c` (~150-200 lines: ALSA sequencer enumerate + open + callback thread)
- `audio_midi_winmm.c` (~80-120 lines: midiInGetDevCaps enumerate + midiInOpen + callback)
- `voice.c` additions (~30-50 lines: voice_pool_trigger_midi + voice_pool_release_midi; +2 fields on Voice)
- `main.c` additions (~30-50 lines: --midi / --midi-list-devices / --midi-channel / --no-midi CLI parsing)
- `mixer.c` additions (~3 lines: call audio_midi_drain() at the start of render_chunk)
- `tests/unit/test_midi.c` (~300-500 lines, but tests don't count toward binary size)
- `Makefile` additions (~10-15 lines)
- `tests/test_smoke_live.sh` additions (~30 lines)

Total new executable code: ~550-820 lines. With `-Os -flto` and the existing `gc-sections` + `ffunction-sections`, this is roughly 1.5-2.5 KB stripped. The CC map table (128 entries × 2 bytes per entry) is ~256 bytes in `.rodata`. The 256-event ring buffer is 256 × 8 bytes = 2 KB in the arena. The Windows packed binary adds a bit more for the `midiInProc` callback thunk + the `midiInGetDevCaps` enumeration loop, but no new library dep.

**Alternatives considered**: None — this is a measurement estimate, not a design choice.

**Confidence**: MEDIUM-HIGH. The estimate is order-of-magnitude correct. The PR will measure exact `make size` and `make win && make winpack` to confirm.

---

## Summary

7 decisions made: callback-only on both platforms (D1, D2); GCC atomic built-ins for SPSC ring buffer (D3); dedicated `voice_pool_*_midi` entry points (D4); dynamic libasound link (D5); in-process unit tests + virtual-loopback smoke test (D6); expected +1.5-2.5 KB Linux / +2-3 KB Windows (D7). All 10 Constitution principles PASS. Ready for Phase 1 (data-model, contracts, quickstart).
