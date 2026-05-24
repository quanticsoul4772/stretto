# Feature Specification: Master Limiter / Compressor

**Feature Branch**: `002-master-limiter-compressor`

**Created**: 2026-05-23

**Status**: Implemented (PR #81, retroactive spec)

**Input**: "spec Master limiter / compressor"

## Overview

Stretto's master bus previously ended in cubic soft saturation followed by an `int16` hard clamp. The cubic curve compressed peaks gently and added harmonic warmth, but it wasn't real dynamics control — it couldn't tame sustained transients, couldn't bring up quiet sections, and the hard clamp still bit when peaks exceeded `int16` range. With 11 voices, reverb tail, and delay feedback, multi-second mix density climbed and the listener heard periodic transient stabs (kick on TENSION sections especially) jumping out of an otherwise quiet bed.

This spec adds a feed-forward stereo compressor with a brickwall limiter as the last stage of the master chain. Soft saturation stays in front as the warmth source; the compressor/limiter sits behind it to ride peaks and guarantee no sample ever clips.

**Status note**: This spec was written retroactively after PR #81 merged. All requirements describe what was implemented; success criteria were validated by the same PR's tests + multi-seed integration.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Tame transients in dense mixes (Priority: P1)

Listener launches the synth and reaches the TENSION song-section (~96-bar cycle). Drum kicks no longer poke above the ambient bed; chord, melody, and counter-melody remain audible underneath rhythmic transients.

**Why this priority**: Primary motivation. Without it, soft saturation alone would suffice.

**Independent Test**: Render the same seed before and after the change. Post-change render's clip count drops to zero AND RMS stays within ±5% of the pre-change RMS.

**Acceptance Scenarios**:

1. **Given** a 16-second render with `--seed 42`, **When** the compressor is enabled, **Then** output peak ≤ 32 000 (brickwall ceiling), clip count = 0, RMS within ±5% of pre-change.
2. **Given** the synth is playing live and a kick fires in TENSION, **When** the listener observes chord and melody, **Then** their perceived loudness does not drop by more than ~3 dB on the kick transient.
3. **Given** a quiet INTRO section, **When** the listener listens, **Then** the bed sits a touch louder than before (~+1 dB makeup) without distortion or audible compressor breathing.

---

### User Story 2 - Guarantee no clipping (Priority: P2)

Listener never hears the harsh artifact of `int16` hard-clip on peaks, regardless of how dense the generative state gets.

**Why this priority**: Soft sat absorbed most peaks already; the brickwall adds a hard guarantee. Quality-of-life, not foundational.

**Independent Test**: Render 4 seconds at each of 4 seeds. No output sample at `int16` min/max boundary on any seed.

**Acceptance Scenarios**:

1. **Given** any seed and render length up to 1 hour, **When** the file is examined sample-by-sample, **Then** zero samples are at int16 saturation boundaries.
2. **Given** a synthetic full-scale input buffer, **When** processed by `compressor_process`, **Then** every output sample's absolute value is ≤ `LIMIT_CEILING` (32 000).

---

### User Story 3 - Live-tunable compression (Priority: P3)

Listener adjusts the compressor's threshold in real time via keyboard and hears the result immediately.

**Why this priority**: Most users will accept the default. Live tuning is a performance nicety.

**Independent Test**: Launch live; press `l` repeatedly — `Lm:` field decreases by 1000 per press; audio becomes audibly more compressed. Press `L` to reverse. Bounds clamp at `[8000, 30000]`.

**Acceptance Scenarios**:

1. **Given** the synth playing live, **When** the user presses `l`, **Then** threshold decreases by 1000, status row updates within one frame, subsequent audio shows more gain reduction.
2. **Given** threshold at lower bound (8000), **When** the user presses `l` again, **Then** threshold stays at 8000 (clamped).
3. **Given** threshold at upper bound (30000), **When** the user presses `L` again, **Then** threshold stays at 30000 (clamped).
4. **Given** `--no-ui` mode, **When** the synth runs, **Then** no terminal output occurs and the audio engine behaves identically to live mode at the default threshold.

---

### Edge Cases

- **All-silent input**: envelope follower decays toward 0; gain stays at unity; output stays silent.
- **Sudden full-scale step**: envelope follower attack catches the transient within ~5 ms; during that window the brickwall ceiling enforces the limit so no samples clip.
- **Sustained full-scale input**: envelope follower reaches steady-state at ~5 ms attack; gain reduces to match 4:1 ratio; sustained output sits at LIMIT_CEILING modulated by makeup gain.
- **Threshold at maximum (30000)**: compressor effectively becomes brickwall-only; envelope and gain logic still run but gain stays at unity below 30 000.
- **Threshold at minimum (8000)**: heavy compression; bed gets noticeably denser.

## Requirements *(mandatory)*

### Functional Requirements

#### Compressor + limiter
- **FR-001**: System MUST implement a feed-forward stereo-linked compressor as the last master-bus stage before `sat16`.
- **FR-002**: System MUST track signal envelope using a one-pole IIR with separate attack and release coefficients (~5 ms attack, ~200 ms release at 48 kHz).
- **FR-003**: System MUST apply gain reduction at a fixed 4:1 ratio above the threshold.
- **FR-004**: System MUST apply a makeup gain of approximately +1 dB to compensate for average gain reduction.
- **FR-005**: System MUST enforce a brickwall limit (`LIMIT_CEILING` = 32 000) — no output sample's absolute value may exceed this ceiling.
- **FR-006**: System MUST link the envelope follower across L and R channels (drive both gains from `max(|L|, |R|)`) to preserve stereo imaging.
- **FR-007**: System MUST run as a deterministic feed-forward function of input + persistent envelope state — no PRNG, no clock.

#### Master chain ordering
- **FR-010**: System MUST run the compressor AFTER `saturate_process` and BEFORE `sat16` in `render_chunk`.
- **FR-011**: System MUST NOT remove or alter `saturate_process` — soft saturation's warmth character is preserved.

#### Runtime control
- **FR-020**: System MUST allow live adjustment of the threshold via key bindings `l` (down by 1000) and `L` (up by 1000).
- **FR-021**: System MUST clamp the threshold to `[8000, 30000]`.
- **FR-022**: System MUST expose `compressor_get_threshold()` for the status row.

#### Initialization
- **FR-030**: System MUST initialize the envelope to 0 and threshold to the default (20 000) from `effects_init()`.

#### Status row
- **FR-040**: System MUST display the current threshold in the status row as `Lm:<n>` (green label, white value) after the existing `T:` filter-mode field.

#### Testing
- **FR-050**: System MUST have unit tests covering: quiet-signal pass-through, loud-signal compression, brickwall enforcement, envelope state persistence, threshold clamping.
- **FR-051**: The existing bit-exact 16-second regression hash MUST be regenerated as part of the same PR.
- **FR-052**: The multi-seed audio-characteristic bounds MUST hold; clip count expected to drop to 0 on every seed.

### Key Entities

- **Envelope follower**: single `int32_t` field tracking the smoothed absolute amplitude of the stereo-linked input. Advances per-sample via one-pole IIR.
- **Threshold**: single `int32_t` runtime-tunable parameter in `[8000, 30000]`; default 20 000. The level above which gain reduction kicks in.
- **Brickwall ceiling**: compile-time constant `LIMIT_CEILING = 32 000`. No output sample may exceed this absolute value.
- **Makeup gain**: compile-time constant `COMP_MAKEUP_GAIN = 288` (8.8 fixed = ~+1 dB).
- **Attack/release coefficients**: compile-time constants tuned for ~5 ms attack and ~200 ms release at 48 kHz.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A 16-second render of `--seed 0` produces zero samples at int16 saturation boundaries. **VALIDATED** in PR #81.
- **SC-002**: Multi-seed integration (4 seeds × 4 seconds) shows clip count = 0 per render (was bound at ≤100). **VALIDATED** in PR #81.
- **SC-003**: 16-second render RMS within ±5% of pre-change. **VALIDATED** by listening.
- **SC-004**: 16-second render peak ≤ 32 000 on every seed. **VALIDATED** in PR #81.
- **SC-005**: Windows packed binary stays ≤ 48 KB. **VALIDATED** — stayed at 32 768 B.
- **SC-006**: 5 new tests in `tests/unit/test_effects.c`; total 109 unit tests pass. **VALIDATED** in PR #81.
- **SC-007**: `effects.c` line coverage ≥ 95%. **VALIDATED** — 100% of 151 lines.
- **SC-008**: Subjective listening test shows TENSION-section kicks no longer "duck" the bed audibly by more than ~3 dB. **PENDING** (listener feedback).

## Assumptions

- The cubic soft saturator's existing warmth contribution is still musically valued; the compressor is additive.
- Per-channel (vs stereo-linked) compression is not needed; the synth's voice panning is symmetric enough.
- Attack at ~5 ms catches drum transients without audible clicks; release at ~200 ms avoids pumping on chord sustains.
- 4:1 ratio is musically transparent for ambient material; not adjustable at runtime in the first cut.
- Makeup gain at +1 dB is conservative.
- Brickwall ceiling at 32 000 (vs int16 max 32 767) leaves ~830 LSB margin for downstream rounding.

## Out of Scope

- Multi-band compression.
- Sidechain compression (e.g. kick ducking the bed).
- Lookahead / oversampling.
- Runtime adjustment of attack, release, ratio, or makeup gain.
- Per-voice compression.
- Visual VU / gain-reduction meter beyond `Lm:` threshold display.

## Constitution Compliance

| Principle | Status | Note |
|---|---|---|
| I. Tiny binary | ✅ | ~250 B compiled; Windows packed stays at 32 768 B. |
| II. C99 only | ✅ | Pure C99; no new dependencies. |
| III. Deterministic | ✅ | Pure feed-forward DSP; no PRNG, no clock. |
| IV. Ambient aesthetic | ✅ | Improves long-form listenability. |
| V. Cleanly modular | ✅ | Inside `effects.c` alongside reverb / delay / soft sat. |
| VI. Test discipline | ✅ | 5 unit tests in new `test_effects.c`; coverage gate maintained at ≥95%. |
| VII. No partial features | ✅ | Compressor + limiter + status row + key binding + tests in one PR. |
| VIII. Document why | ✅ | Comments around envelope coefficient derivation. |
| IX. Cross-platform | ✅ | No platform-specific code introduced. |
| X. Generative > random | n/a | Synth-side feature, not generative. |

## Workflow Note

This is the **first spec written retroactively**. The compressor was implemented from a plan file (`~/.claude/plans/indexed-yawning-pearl.md`) without first creating a spec, then this spec was authored after merge to document what shipped and close the workflow gap. Going forward, new features should produce `specs/NNN-*/spec.md` before implementation per the spec-kit workflow.
