# Tasks: MIDI Input (003-midi-input)

**Input**: Design documents from `/specs/003-midi-input/`
- `spec.md` — user stories US1 (P1, MVP), US2 (P2), US3 (P3); FR-001..FR-054; SC-001..SC-007
- `plan.md` — C99 + libasound + winmm; estimated add ~3-5 KB stripped Linux / ~4-6 KB packed Windows (post M2-fix); Constitution Check PASS all 10
- `research.md` — 7 architectural decisions D1..D7; ALSA + winmm callback-only; SPSC ring with `__atomic_*`; dedicated `voice_pool_*_midi` entry points
- `data-model.md` — 5 entities: `midi_event_t`, `midi_input_device_t`, `midi_queue_t`, `cc_map_entry_t` + `CC_MAP[128]`, `voice_pool_*_midi` + Voice struct extension
- `contracts/cli.md` — CL-001..CL-XXX exact argv grammar + exit codes + error strings
- `quickstart.md` — listener usage + CC map + build commands + platform notes

**Path Convention**: Single project (one binary, one Makefile) — `src/` at repo root; tests under `tests/`. `audio_midi.{c,h}` + `audio_midi_linux.c` + `audio_midi_winmm.c` are the four new source files; `main.c`, `voice.{c,h}`, `mixer.c` are the three modified engine files.

**Tests**: REQUIRED (Constitution VI NON-NEGOTIABLE + spec FR-051, SC-005). Tests-first within each user story phase; tests must FAIL before their story's implementation completes. Coverage gate ≥90% per-file on `audio_midi.c` (FR-054).

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Project-init scaffolding that ALL stories depend on. Header types, CLI argv plumbing, Makefile dynamic-link, CLAUDE.md pointer.

- [ ] T001 Create `audio_midi.h` at repo root with the cross-platform interface: `midi_event_t` (4 B; type/channel/key/value as `uint8_t`), `MIDI_EVENT_*` enum, `midi_input_device_t { int32_t index; char name[64]; }`, `midi_state_t channel_filter` (1..16; 0 = all), function decls `audio_midi_init(int channel_filter)`, `audio_midi_open(int device_index)`, `audio_midi_close(void)`, `audio_midi_enqueue(const midi_event_t *ev)` (called by platform callbacks), `audio_midi_drain(void)` (called by audio thread in render_chunk), `audio_midi_list_devices(midi_input_device_t *out, int32_t *count)`, `audio_midi_drop_count(void)`. Mirror arena-aligned 8-byte convention.
- [ ] T002 [P] Add `--midi [N]`, `--midi-default`, `--midi-list-devices`, `--midi-channel N`, `--no-midi` recognition to the argv pre-scan loop in `main.c` (mirrors existing `--seed` pre-scan). On exit from pre-scan, route to `audio_midi_init` / `audio_midi_open` / `audio_midi_list_devices` based on flags. Preserve existing positional dispatch `--render <seconds> <output.wav>` and the live branch.
- [ ] T003 [P] Update `Makefile`: append `audio_midi.o` to OBJS, `audio_midi_linux.o` to the Linux-only subgroup of OBJS (mirrors existing `audio_pulse.o` pattern). Append `audio_midi_winmm.win.o` to WIN_OBJS. Append `-lasound` to the `synth` and `synth_cov` link lines (mirrors the existing `-lpulse` at `Makefile:104`, `:207`). Add `audio_midi.c` to COV_SRCS; add `audio_midi_linux.c` to COV_SRCS_INTERACTIVE.
- [ ] T004 [P] Verify `CLAUDE.md` SPECKIT block points at `specs/003-midi-input/plan.md` (already set during `/speckit-plan`; this task is the audit gate per Principle VIII).

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Ring buffer + Voice struct extension + voice pool entry points + audio-thread drain hook. ALL user stories block on this.

**⚠️ CRITICAL**: No user story implementation can begin until this phase is complete.

- [ ] T005 [P] Extend `Voice` struct in `voice.h`: append `uint8_t trigger_key` and `uint8_t trigger_channel` after the existing `glide_remain` / `_glide_pad` fields, before the `u` union. Update struct-level docstring to describe the sentinel values: `trigger_key = 0xFF` and `trigger_channel = 0` mean "not a MIDI voice" (set by the existing `voice_init()` path). Existing fields and union shape unchanged.
- [ ] T006 [P] Initialize the new fields to their sentinel values in `voice_init()` in `voice.c`: set `v->trigger_key = 0xFF; v->trigger_channel = 0;` after the existing `glide_remain = 0;` line.
- [ ] T007 Implement `voice_pool_trigger_midi(uint8_t note, uint8_t velocity, uint8_t channel)` in `voice.c` per `data-model.md` Entity 5 steps 1-7: walk all `N_VOICES` (= 11) voices; first-pass pick `env_phase == ENV_OFF`; second-pass pick min `env_time` with `env_phase == ENV_R` (Q1 voice-stealing — oldest in release); third-pass pick min `env_time` regardless of phase (Q1 fallback); call `voice_trigger()` with scale-mapped note (`SCALES[cur_scale][note%7] + (note/7 - 5)*12` with the result clamped to `[-2*12, +4*12]` per FR-010 + H2 fix); set `velocity` as the env peak scale (`env_amp = velocity * 256` clamped to `[64, 32767]`); set `trigger_key = note; trigger_channel = channel;`.
- [ ] T008 Implement `voice_pool_release_midi(uint8_t key, uint8_t channel)` in `voice.c` per `data-model.md` Entity 5: walk all `N_VOICES` voices; find first match `trigger_key == key && trigger_channel == channel && env_phase != ENV_OFF`; on match, set `env_phase = ENV_R; env_time = 0;`; no-op if already `ENV_R` (FR-013) or no match.
- [ ] T009 [P] Implement `midi_queue_t` ring buffer (static state in `audio_midi.c`): declare `static midi_queue_t q;` sized at `MIDI_QUEUE_CAPACITY = 256` (per FR-040); allocate from the 128 KB arena in `audio_midi_init()`. Implement drop counter `static uint32_t drops = 0;` exposed via `audio_midi_drop_count()`. Handle-1: `q.head` is producer-acquire/release atomic; `q.tail` is audio-thread-only plain write.
- [ ] T010 [P] Implement `audio_midi_enqueue(const midi_event_t *ev)` in `audio_midi.c`: producer-side (called from ALSA / midiInProc context). Write `q.events[q.head & MASK] = *ev;` then `__atomic_store_n(&q.head, q.head + 1, __ATOMIC_RELEASE);`. Overflow behavior: if `q.head - q.tail >= MIDI_QUEUE_CAPACITY`, increment `drops` and skip the event (no malloc, no fprintf from a callback per FR-040 + Q2 callback-only).
- [ ] T011 Implement `audio_midi_drain(void)` in `audio_midi.c` (the consumer-side dispatch): begin with `local_head = __atomic_load_n(&q.head, __ATOMIC_ACQUIRE);` then loop `while (q.tail != local_head)`; for each event, apply the **CHANNEL FILTER** (per FR-004 + M1 fix) — drop if `q.channel_filter != 0 && ev->channel != q.channel_filter` — then dispatch by `ev->type` to Note On / Note Off / CC handlers (the dispatch handlers are stubbed in this task; full implementation lives in US1/US2 phases). After the loop, plain-write `q.tail = local_head;`. Honor FR-033: this function MUST be the only consumer; no callbacks, no platform APIs.
- [ ] T012 Wire `audio_midi_drain()` call into `mixer.c render_chunk` at the very top of the function (before any voice iteration), per FR-033 + `plan.md` Project Structure row.
- [ ] T013 [P] Implement `audio_midi_init(int channel_filter)` opt-out path: when channel_filter = -1 OR a global `--no-midi` flag is set, set a static `enabled = 0` flag and skip the arena allocation, queue init, and `audio_midi_open()` work entirely. `audio_midi_drain()` becomes a no-op (early return). When `enabled = 0`, `render_chunk` MUST be byte-identical to baseline (FR-050 / FR-053).

---

## Phase 3: User Story 1 — Trigger notes from USB MIDI keyboard (Priority: P1) 🎯 MVP

**Goal**: Listener launches `synth --midi`, presses a USB MIDI key, hears a synth note within ~21 ms in the active section's voice type.

**Independent Test**: `synth --midi --no-ui` on Linux with `snd-seq-dummy` loopback; send `90 3C 7F` (Note On middle-C velocity 127) via `amidi`; verify non-silent audio buffer within 1 s (validated by `make test-smoke` per SC-006).

### Tests for User Story 1

> Tests MUST be written FIRST and FAIL before implementation completes (Constitution VI)

- [ ] T014 [P] [US1] Write `tests/unit/test_midi.c` scaffolding: include `tests/unit/test.h`; declare test cases listed below; per-file coverage gate ≥90% tagged in Makefile (FR-054).
- [ ] T015 [P] [US1] `midi_scale_degree_mapping` in `tests/unit/test_midi.c` (FR-010 K%7 → `SCALES[cur_scale][K%7]`): assert K=0..6 → expected degrees for D Dorian (the default test scale); assert K=60 (middle C) → correct degree + octave.
- [ ] T016 [P] [US1] `midi_velocity_scaling` in `tests/unit/test_midi.c` (FR-010 V/127 amplitude clamp [64, 32767]): assert V=0 maps to 64, V=127 maps to 32767, V=64 maps to ~16384.
- [ ] T017 [P] [US1] `midi_octave_clamp` in `tests/unit/test_midi.c` (FR-010 + H2 fix clamp `[-2, +4]`): assert K=0 (clamp → -2), K=60 (no clamp → 3), K=127 (clamp → +4).
- [ ] T018 [P] [US1] `midi_voice_stealing` in `tests/unit/test_midi.c` (Q1 voice-stealing rules): assert first Note On allocates ENV_OFF voice; assert 12th concurrent Note On steals the quietest ENV_R voice (Q1); assert no ENV_R available → steal oldest regardless (Q1 fallback).
- [ ] T019 [P] [US1] `midi_no_midi_byte_identical` in `tests/unit/test_midi.c` (FR-050): assert hash of `render_chunk` output with `audio_midi_init(-1)` (no-midi path) equals the baseline golden hash from `golden/regression_16s.sha256` (FR-053).

### Implementation for User Story 1

- [ ] T020 [US1] Implement Note On dispatch handler in `audio_midi.c` `audio_midi_drain()` (per data-model Entity 4; depends on T007, T008 already in foundational): for `MIDI_EVENT_NOTE_ON` events, compute `scaled_note = SCALES[cur_scale][note%7] + clamp(note/7 - 5, -2, +4) * 12`; call `voice_pool_trigger_midi(scaled_note, velocity, channel)`. Wire from T011's dispatch switch.
- [ ] T021 [US1] Implement Note Off dispatch handler in `audio_midi.c` `audio_midi_drain()`: for `MIDI_EVENT_NOTE_OFF` events OR `MIDI_EVENT_NOTE_ON` with velocity == 0 (FR-011), call `voice_pool_release_midi(key, channel)`. Wire from T011's dispatch switch.
- [ ] T022 [P] [US1] Implement `audio_midi_linux.c` ALSA sequencer backend: `snd_seq_open()` + subscribe to all clients/ports that send MIDI Keyboard events (via `snd_seq_connect_from(seq, port, src_client, src_port)` iterating the enumerate output from T034) + `pthread_create(&worker_thread, NULL, alsa_worker, seq)`. The worker loops on blocking `snd_seq_event_input(seq, &ev)`; per-event switch on `ev->type` — `SND_SEQ_EVENT_NOTEON` → Note On (channel = `ev->data.note.channel + 1`); `SND_SEQ_EVENT_NOTEOFF` → Note Off; `SND_SEQ_EVENT_CONTROLLER` → CC; `SND_SEQ_EVENT_PORT_EXIT` or `snd_seq_event_input() < 0` → break (per FR-034 disconnect handling). Populate `midi_event_t` from structured fields (`ev->data.note.note`, `ev->data.note.velocity`) — libasound parses MIDI 1.0 wire format automatically, no manual status-byte nibble extraction. Tear down via signal-flag + `pthread_join()` (or `pthread_cancel` + `pthread_join`). **Preflight correction 2026-07-06**: replaces the originally-planned `snd_seq_create_thread()` (NOT a stock libasound API) with this standard pthread + blocking-input pattern.
- [ ] T023 [P] [US1] Implement `audio_midi_winmm.c` Win32 backend: `midiInOpen(&handle, dev = device_index, (DWORD_PTR)&MidiInProc, instance, CALLBACK_FUNCTION)` + `midiInStart(handle)`. `MidiInProc` parses MIM_DATA messages (status byte determines Note On / Off / CC and channel; first data byte is key, second is value) into a `midi_event_t` and calls `audio_midi_enqueue()`. Tear down via `midiInStop` + `midiInClose`.
- [ ] T024 [US1] Wire disconnect handling per FR-034 + Q3 (no auto-reconnect): Linux — when `snd_seq_event_input` reports `SND_SEQ_EVENT_PORT_EXIT` for the subscribed source, call `audio_midi_linux_close()`; audio thread's `audio_midi_drain()` continues to no-op on empty queue; synth continues from internal generative state. Win32 — `MM_MIM_CLOSE` triggers `audio_midi_winmm_close()` analogously. CC-modulated parameters retain last value (no reset on disconnect per FR-034).

**Checkpoint**: US1 fully functional; `make test-unit` passes T014-T019; an end-to-end smoke test (T037 in polish) verifies live note-on produces audio.

---

## Phase 4: User Story 2 — Modulate live parameters via MIDI CC (Priority: P2)

**Goal**: Listener routes a hardware controller's CCs to cutoff / resonance / reverb wet / delay wet / compressor threshold via the static `CC_MAP[128]` table.

**Independent Test**: With `synth --midi --no-ui` running, send CC#1 (mod wheel) value 127 via `amidi`; verify `voice_get_cutoff()` reflects a Δ(127) = (127-64)*1 = +63 unit change (per `data-model.md` Entity 4 dispatch formula + H1 fix).

### Tests for User Story 2

- [ ] T025 [P] [US2] `midi_cc_mapping` in `tests/unit/test_midi.c` (FR-020 + H1 fix): for each assigned CC (#1, #7, #71, #74, #91, #93), assert the `audio_midi_enqueue_canned_cc(C, V)` test helper routes to the expected target with `delta = (V-64)*scale` formula; for CC_TARGET_NONE entries (CC#16, #17, #19, #123), assert silent drop (no adjust_* invocation recorded via a global side-effect counter in the test).
- [ ] T026 [P] [US2] `midi_cc_default_zero` in `tests/unit/test_midi.c` (Q4 / FR-020 first sub-bullet): assert all CC values start at 0 at synth launch and that the first CC message updates state (test by capturing audio output before/after a series of CC sends and asserting the post-state change is audible via cutoff parameter read out).
- [ ] T027 [P] [US2] `midi_cc_unassigned_noop` in `tests/unit/test_midi.c`: assert CC#16/#17/#19/#123 produce zero side effects (audio output unchanged).
- [ ] T028 [P] [US2] `midi_cc_multi_sums_additive` in `tests/unit/test_midi.c` (FR-022): assert two CCs targeting the same parameter (e.g., CC#1 AND CC#74 both → cutoff) sum additively — total Δ = Δ_C1 + Δ_C74.

### Implementation for User Story 2

- [ ] T029 [US2] Build static `CC_MAP[128]` table in `audio_midi.c` per `data-model.md` Entity 4 spec: `CC#1 → CC_TARGET_CUTOFF, scale=+1`; `CC#7 → CC_TARGET_COMPRESSOR_THRESH, scale=+60`; `CC#71 → CC_TARGET_RESONANCE, scale=+1`; `CC#74 → CC_TARGET_CUTOFF, scale=+1`; `CC#91 → CC_TARGET_REVERB_WET, scale=+1`; `CC#93 → CC_TARGET_DELAY_WET, scale=+1`; all other indices (including #0, #10, #16, #17, #19, #64, #123) → `CC_TARGET_NONE`. Total table footprint: 128 × 4 B = 512 B in `.rodata`.
- [ ] T030 [US2] Implement CC dispatch handler in `audio_midi.c` `audio_midi_drain()`: for `MIDI_EVENT_CC` events, read `CC_MAP[ev->key]`; if `target != CC_TARGET_NONE`, compute `delta = ((int)(ev->value) - 64) * entry.scale;` and call the corresponding adjust function: `voice_adjust_cutoff(delta)` / `voice_adjust_resonance(delta)` / `reverb_adjust_wet(delta)` / `delay_adjust_wet(delta)` / `compressor_adjust_threshold(delta)` (T031 confirms `compressor_adjust_threshold` exists in `effects.c`). Wire from T011's dispatch switch.

**Checkpoint**: US1 + US2 both functional; CC modulation audible in live mode; CC+Note On coexist on the same channel.

---

## Phase 5: User Story 3 — List and select MIDI input devices (Priority: P3)

**Goal**: Listener runs `synth --midi-list-devices` to see available devices; runs `synth --midi [N]` to bind a specific device; runs `synth --midi-channel N` to filter to one channel.

**Independent Test**: Run `--midi-list-devices` with a USB MIDI controller connected → output lists the device name with its index; run `--midi` (no number) → device 0 selected.

### Tests for User Story 3

- [ ] T031 [P] [US3] `midi_channel_filter` in `tests/unit/test_midi.c` (FR-004 + M1 fix): assert that with `channel_filter = 5`, events on channels 1-4, 6-16 are silently dropped via the drain; events on channel 5 dispatch as expected.
- [ ] T032 [P] [US3] `midi_list_devices_returns_count` in `tests/unit/test_midi.c`: stub platform enumeration (`enumerate_via_*`) with 2 mock devices; assert `audio_midi_list_devices(out, &count)` returns the populated count; assert no memory corruption when out is bounded to N devices and actual count > N (test passes if function caps writes at N).

### Implementation for User Story 3

- [ ] T033 [US3] Implement `audio_midi_list_devices(midi_input_device_t *out, int32_t *count)` in `audio_midi.c` (per `data-model.md` Entity 2): calls platform-specific `enumerate_via_*` backend; populates the output array with `{ index, name }` pairs and sets `*count` to the populated length. Both backends append devices until they hit the caller's array bound.
- [ ] T034 [P] [US3] Implement `audio_midi_linux.c enumerate_via_snd_seq_client_info(seq, out, count_max)`: walk `snd_seq_query_next_client` + `snd_seq_query_get_port_info` for each client; populate `midi_input_device_t { .index, .name = truncated_name }` for ports with `MIDI_GENERIC` or `MIDI_KEYBOARD` capability; exclude the synth's own announce port.
- [ ] T035 [P] [US3] Implement `audio_midi_winmm.c enumerate_via_midiInGetDevCaps(out, count_max)`: call `midiInGetNumDevs()`; for each device call `midiInGetDevCaps(i, &caps, sizeof(MIDIINCAPS))` and populate index + truncated `szPname`.
- [ ] T036 [US3] Wire `--midi-channel N` in `main.c` argv parser: parse N as int in [1..16]; pass N to `audio_midi_init(N)` so the drain's channel filter matches (per FR-004 + M1 fix). Default (`--midi-channel` omitted or `N=0`) means accept all channels.
- [ ] T037 [US3] Wire `--midi-list-devices` in `main.c` argv parser: stack-allocate `midi_input_device_t devs[32];` call `audio_midi_list_devices(devs, &count);` then printf `"%d %s\n", devs[i].index, devs[i].name` for each; exit 0. Per `contracts/cli.md` exit-code/dispatch table.
- [ ] T038 [US3] Wire `--midi [N]` in `main.c` argv parser (depends on T036): parse N as int (0-based index); call `audio_midi_open(N)`; if `audio_midi_open` returns non-zero (no device ready), print error message per `contracts/cli.md` row `--midi [N] with no device present` and exit non-zero. `--midi-default` is an alias for `--midi 0` (FR-002); recognized in the pre-scan loop in main.c.

**Checkpoint**: All three user stories functional; `--midi-list-devices` works; `--midi [N]` opens the device; `--midi-channel N` filters; CC, Note On, Note Off all work end-to-end via the existing live mode.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Smoke test, byte-exact verification, coverage gate, README/CHANGELOG, size budget verification. Each task is independent and can run in parallel.

- [ ] T039 [P] Update `tests/test_smoke_live.sh` to add a virtual-MIDI-loopback step before the existing PulseAudio step: `modprobe snd-seq-dummy` (or auto-skip with `[skip] snd-seq-dummy kernel module not available`); subscribe the synth's port to a sequencer subscribe; `amidi -p hw:0,0 -s 90 3C 7F` to send a Note On; capture 1 s of audio; verify non-zero RMS in the captured buffer. Auto-skip the entire step if `amidi` or `snd-seq-dummy` is unavailable (echo `[skip] MIDI loopback not available`, exit 0). Mirrors the existing PulseAudio skip pattern.
- [ ] T040 [P] Update `README.md` under the existing `## Specification` section: add a "## MIDI Control Surface" subsection explaining `synth --midi` prerequisite (USB MIDI controller + libasound2-dev on Linux / winmm already linked on Windows), `--midi-list-devices` diagnostic, `--midi-channel N`, and short list of CC bindings from the quickstart CC map.
- [X] T041 [P] Update `ARCHITECTURE.md` to describe MIDI input as part of the input surface (alongside the existing `keys.c` keyboard dispatcher); add a header-level pointer to `specs/003-midi-input/spec.md` for the capability spec.
- [ ] T042 [P] Update `CHANGELOG.md` with a top entry: `feat(midi): USB-MIDI keyboard input + CC modulation + device listing (003-midi-input)` with a link to the commit hash once the implementation PR lands.
- [ ] T043 [P] Update `.specify/feature.json` to mark status as `tasks_generated` (after this tasks.md is written) so downstream `/speckit-implement` can find the task list.
- [ ] T044 Verify `make size` on Linux: post-build, stripped binary stays under the 24 KB soft target (Constitution I, post M2-fix estimate ~3-5 KB add).
- [ ] T045 Verify `make win && make winpack`: post-build Windows .exe stays under the 48 KB hard limit (Constitution I, post M2-fix estimate ~4-6 KB add).
- [ ] T046 Verify `make coverage`: post-build, the per-file coverage tool reports ≥90% line coverage on `audio_midi.c` (FR-054). `audio_midi_linux.c` + `audio_midi_winmm.c` join `audio_pulse.c` in the INTERACTIVE set (require connected device or loopback) and are excluded from the coverage measurement.
- [ ] T047 Run `make test` (bitexact + multi-seed + units): bitexact golden MUST still pass per FR-050/FR-053 (no regression in the `--no-midi` path); multi-seed + unit tests pass.
- [ ] T048 Run `make test-smoke` locally with libasound available: verify the loopback step (T039) passes within 1 s. Skip if libasound not installed (auto-skip behavior).
- [ ] T049 Run `/speckit-analyze` (or manual cross-check via grep) on the post-implementation spec-kit artifacts to confirm they still match the as-built surface per Principle VIII: no stale spec.md references to something no longer true; data-model.md still authoritative for dispatch formulas; research.md D1-D7 still accurate.

**Checkpoint**: All phases complete; PR ready to open.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — can start immediately.
- **Foundational (Phase 2)**: Depends on Setup completion (T001..T004) — **BLOCKS** all user stories.
- **User Stories (Phase 3-5)**: All depend on Foundational phase completion.
  - US1 (P1) → US2 (P2) → US3 (P3) — sequential priority. Each is independently testable.
  - In a parallel-team context: once Phase 2 completes, US1/US2/US3 can run in parallel (different file subsets, minimal cross-dependencies; US2's T029 reads the static `CC_MAP` table built in US2 itself, US3's `audio_midi_list_devices` depends on T033 which depends on the platform backends built in US1's T022/T023 — so US3 has a dependency on US1's platform backends being available).
- **Polish (Phase 6)**: Depends on all desired user stories being complete.

### Within Each User Story

- Tests (T015-T019, T025-T028, T031-T032) MUST be written and FAIL before the implementation tasks in the same phase complete.
- Models / data structures before dispatch logic.
- Dispatch logic before platform callbacks.
- Platform callbacks before integration (CLI wiring in main.c).

### Parallel Opportunities

Anywhere a `[P]` tag appears the task can run in parallel with other `[P]` tasks in the same phase (different files, no dependency edges).

- **Phase 1**: T001 (header), T002 (main.c argv), T003 (Makefile), T004 (CLAUDE.md audit) — all `[P]`, can launch in parallel.
- **Phase 2**: T005 (voice.h struct), T006 (voice.c init), T009 (queue), T010 (enqueue), T013 (init opt-out) — all `[P]`. T007 (trigger_midi), T008 (release_midi), T011 (drain stub), T012 (mixer wire) have dependencies on T005/T006.
- **Phase 3**: T014-T019 (tests) all `[P]` and run before T020-T024 (implementation). T022 (Linux backend) and T023 (Win32 backend) `[P]`.
- **Phase 4**: T025-T028 (tests) all `[P]`. T029 (CC_MAP static) `[P]` with T030 (CC dispatch handler).
- **Phase 5**: T031-T032 (tests) all `[P]`. T034 (Linux enumerate) and T035 (Win32 enumerate) `[P]`.
- **Phase 6**: All polish tasks are `[P]` except T044-T048 which are verification gates that run serially after prior steps complete.

---

## Implementation Strategy

### MVP First (User Story 1 only) — minimal demo path

1. Complete Phase 1: Setup (T001..T004)
2. Complete Phase 2: Foundational (T005..T013) — **CRITICAL**; blocks all stories
3. Complete Phase 3: User Story 1 (T014..T024)
4. **STOP and VALIDATE**: run `make test-unit` to confirm T015-T019 pass; run `make test-bitexact` to confirm FR-053 still passes; run `amidi` smoke (T039) to confirm live note-on works on a real Linux machine with libasound + snd-seq-dummy
5. Demo / merge if ready

### Incremental Delivery

1. Setup + Foundational → audio_midi.c core scaffolding in place (no audible change yet)
2. + US1 (P1) → live note-on works on USB MIDI keyboard; **first demo**
3. + US2 (P2) → CC knobs modulate cutoff / resonance / reverb / delay / compressor; demo grows
4. + US3 (P3) → device listing + channel filter + explicit device selection; demo more discoverable
5. + Polish → README/CHANGELOG/quickstart updated; size gate passes

### Parallel Team Strategy (with multiple developers)

1. Team completes Setup + Foundational together
2. Once Phase 2 completes:
   - Developer A: User Story 1 (Note On/Off — core audio path)
   - Developer B: User Story 2 (CC table — extends dispatch)
   - Developer C: User Story 3 (CLI surface — main.c argv)
3. US2's `CC_MAP` table is independent of US1's note dispatch; US3's CLI surface is independent of both. The cross-dependency (US3's `audio_midi_list_devices` calls platform enumeration that needs the backends from US1) means US3's T034/T035 must wait for US1's T022/T023 backend skeletons, but the main.c argv plumbing (T036-T038) does not.
4. Each story has independent tests, so each can merge separately if needed; final integration in Phase 6.

---

## Notes

- `[P]` tasks = different files, no dependencies
- `[Story]` label maps task to specific user story (US1/US2/US3) for traceability and parallel development
- Each user story is independently completable and testable
- Tests (T014..T019 etc.) MUST FAIL before their story's implementation completes; they verify the contract per Constitution VI
- Commit checkpoints recommended: after Setup, after Foundational, after each User Story's full phase, after Polish. Belt-and-suspenders per Constitution VIII (each commit documents the why)
- One PR for the entire feature per Constitution VII ("No Partial Features") — single PR ships Note On/Off routing, CC modulation, device listing, unit + smoke tests
- Constitution I budget verification (T044-T045) is a hard gate; if the actual stripped/Windows size exceeds the budget, the PR is rejected regardless of feature completeness
- Constitution III v1.0.1 determinism preserved by T013 + T019 + T047 (the `--no-midi` path is byte-identical and exercised by both unit tests and the existing bitexact regression)
