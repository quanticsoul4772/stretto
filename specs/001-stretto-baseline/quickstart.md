# Quickstart — Stretto Baseline

**Branch**: `001-stretto-baseline` | **Date**: 2026-07-06 | **Spec**: [spec.md](spec.md) | **Plan**: [plan.md](plan.md)

Minimal usage reference for the shipped synth (the `001-stretto-baseline` capability surface). Full feature rationale lives in [spec.md](spec.md) and architectural decisions in [research.md](research.md).

## 1. Build

```bash
# Linux (PulseAudio).
make

# Windows cross-compile + UPX pack (CI-equivalent).
make win && make winpack
```

Build artifact names: `synth` (Linux), `synth.exe` (Windows). UPX-packed Windows binary targets ≤48 KB (last measured ~37 KB).

## 2. Listen (P1)

```bash
./synth                 # Linux PulseAudio default output
stretto.exe             # Windows waveOut default output
```

Within 1 second, ambient generative music begins. The terminal status row updates at every audio buffer (~21 ms). Press `?` for the key overlay; press `q` to exit cleanly.

## 3. Render reproducible audio (P2)

```bash
./synth --render 16 /tmp/out.wav --seed 0
```

Bit-exact reproducer: the same `--seed N` produces a byte-identical WAV across the supported build targets (Linux glibc + Windows winmm, both little-endian x86). Two different seeds always produce distinct SHA-256 hashes. Range is `[1, 3600]` seconds; out-of-range exits with a usage error.

For headless capture or scripted CI runs:

```bash
./synth --render 16 /tmp/out.wav --seed 0 --no-ui > /dev/null 2>&1
```

## 4. Live-mode key map (P3)

| Key            | Action                                                  |
|----------------|---------------------------------------------------------|
| `?`            | Toggle help overlay                                     |
| `q`            | Quit (drains audio + restores terminal)                 |
| `+` / `-`      | Tempo up / down                                         |
| `[` / `]`      | Gate density down / up (lower → sparser)                |
| `s`            | Cycle scale (Dorian → Lydian → Phrygian → Locrian → HM → Mixolydian) |
| `g` / `G`      | Reverb wet down / up                                    |
| `d` / `D`      | Delay wet down / up                                     |
| `f` / `F`      | Delay feedback down / up                                |
| `r` / `R`      | Mutation rate down / up                                 |
| `c` / `C`      | Filter cutoff down / up                                 |
| `n` / `N`      | Filter resonance down / up                              |
| `m` / `M`      | LFO depth down / up                                     |
| `t`            | Capture / cycle motif replay                            |

All bounds clamp — keys below `0` or above max are no-ops.

## 5. Run tests

```bash
make test          # 16-s SHA-256 regression (golden)
make test-unit     # per-module unit
make test-multiseed# 4-seed audio bounds (python3 + numpy required)
make test-smoke    # 2-s timeout live-mode launch
make coverage      # per-file gates (95% arena/effects/voice/section/density/motif/mixer;
                   #                90% gen/lsystem/chord_progression/wav/main)
```

All five must pass on `main` for a PR to merge (Constitution Principle VI).

## 6. Platform notes

- Linux: requires `libpulse-dev` for live mode; `--no-ui` skips PulseAudio for scripted runs.
- Windows 10+: native terminal works (ANSI via `ENABLE_VIRTUAL_TERMINAL_PROCESSING` enabled in `ui.c`).
- WSL2 audio is best-effort; the Windows native binary is the supported path.
- Modern `gcc` + GNU Make + UPX are the only build tools.

## 7. Out of scope (this baseline)

The following are explicitly NOT part of `001-stretto-baseline`:

- MIDI input / output
- FFT / spectral analysis
- VST / AU / LV2 plugin form factor
- Microphone input
- Neural-network priors
- Microtonal / non-12-EDO tuning
- Preset save / load (beyond `--seed N`)
- Graphical (non-terminal) UI
- Multi-track / stem export

Each of these is a candidate future spec.
