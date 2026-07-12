# Release runbook (written for v1.5.0; the procedure is version-generic)

Tagging is the ONLY human decision; everything else is automated or
scripted here. There is **no version file to bump**: `version.h` is
untracked and regenerated from `git describe`, and the release
workflow asserts the built binary reports exactly the tag's version —
the tag IS the source of truth.

## 1. Preconditions

- The readiness PR (083) is merged; both CI jobs green on main tip.
- Clean tree (`git status`), local main up to date.
- Tag the CURRENT MAIN TIP. The tag run re-runs every gate on the
  tagged SHA on the pinned ubuntu-24.04 image (the goldens are bound
  to that image's libm — see release.yml's header), so any drift
  since the last rehearsal is caught, not trusted.
- `grep -rn "1\.4\.0" README.md` — no stale outgoing-version text
  (the status-panel example was pre-bumped to 1.5.0 in 083).

## 2. Rehearse (no-publish dry run)

```
gh workflow run release.yml        # workflow_dispatch = rehearsal
gh run watch $(gh run list --workflow release.yml --limit 1 --json databaseId -q '.[0].databaseId')
```

Every gate runs (make test / test-unit / test-multiseed, size budget
gate, post-build assertions, installer drift gate); only the publish
step is tag-gated. Then verify the artifact:

```
gh run download <run-id> -n release-rehearsal -D /tmp/rehearsal
cd /tmp/rehearsal
wc -l sha256sums.txt        # exactly 8 entries
sha256sum -c sha256sums.txt # all OK
```

The 8: linux binary, linux-upx, windows exe, windows-upx exe,
stretto.1, stretto.bash, _stretto, stretto-demo.wav. Rehearsal
binaries carry `rehearsal-<git describe>` names — expected.

## 3. Tag

Annotated tag with the drafted message (Appendix A):

```
git tag -a v1.5.0        # editor opens: paste Appendix A
git push origin v1.5.0
```

A typo'd tag name is SAFE: the workflow's version assertion
(`./synth --version` must equal `stretto <tag>`) fails the run before
anything publishes.

## 4. Watch the tag run

Same gates as the rehearsal, plus: the version assertion, the
exe-embedded-version grep, and the publish step
(`gh release create --draft` → `upload --clobber` → `edit --draft=false`).

Failure handling:
- **Any gate failure** (flake, size budget): re-run the workflow from
  the same tag — publish is idempotent.
- **Mid-publish failure** leaves an INVISIBLE DRAFT release: re-run
  the failed run from the tag; never retag.
- **Tag on the wrong commit** (the only case a re-run can't fix):
  `git push origin :refs/tags/v1.5.0`, delete the draft release
  (`gh release delete v1.5.0`), fix, retag.

## 5. Post-publish smoke

```
d=$(mktemp -d) && cd "$d"
curl -fsSL https://raw.githubusercontent.com/quanticsoul4772/stretto/main/install.sh | sh -s -- --prefix "$d"
"$d"/bin/stretto --version         # stretto 1.5.0
curl -fsSLo demo.wav https://github.com/quanticsoul4772/stretto/releases/latest/download/stretto-demo.wav
```

(install.sh derives everything from the published sha256sums.txt; it
needs no version argument and no edits per release.)

## 6. Formula bump (WSL Linuxbrew) — the 068 completions IOU lands here

The tarball exists ONLY after the tag is pushed; this PR merges
strictly after step 3.

```
curl -fsSL https://github.com/quanticsoul4772/stretto/archive/refs/tags/v1.5.0.tar.gz | sha256sum
```

Apply Appendix B (URL, sha256, and the two completion-install lines),
then in the tap clone:

```
brew audit --strict --online quanticsoul4772/stretto/stretto
brew install --build-from-source quanticsoul4772/stretto/stretto   # pre-merge test
brew test stretto
ls "$(brew --prefix)"/etc/bash_completion.d/stretto \
   "$(brew --prefix)"/share/zsh/site-functions/_stretto
```

PR the formula; after merge: `brew update && brew upgrade stretto`.

## 7. Optional post-release chores

- **AUR** (`packaging/aur/PKGBUILD`): still pins v1.3.0 and has never
  been published (no AUR account). If ever published, the bump is
  version + sha only — its `make install` already ships both
  completions. Not release-blocking.
- README demo-wav link (README.md:42) goes from forward-promise to
  true automatically once v1.5.0 publishes; no edit.

---

## Appendix A — v1.5.0 tag message (draft, edit freely at tag time)

```
v1.5.0: expressive MIDI, swing, rich TUI, completions

Since v1.4.0:
- MIDI expressiveness: CC#64 sustain pedal with true gate semantics,
  CC#123 All Notes Off (strict MIDI 1.0 damper rules), pitch bend;
  channel-range guard closes a latent UB in the event drain
- --swing <0-100>: MPC-style shuffle for the generative grid
- Rich TUI: five-line full-word status panel on 20+-row terminals
  (width-clamped at word boundaries) and a live-value help overlay
  on ?; the compact fallback row gains the Sw field
- Ctrl-Z suspends cleanly: terminal restored before the stop, fg
  re-enters raw mode
- Arrow keys no longer mutate parameters in live mode (CSI sequences
  swallowed; Windows scancode twin filtered); 1-2-row terminals no
  longer scroll-flood; stale scope rows erased every frame
- Distribution: bash/zsh completions installed by install.sh, make
  install, and brew; releases attach stretto-demo.wav and the
  completion files
- Hardening: sanitizers required in CI, 50k-event MIDI fuzz, coverage
  gates across 15 modules, Windows live-path smoke in CI with
  cross-platform bit-exactness, 1,888 B size reclaim
```

## Appendix B — formula block for the v1.5.0 bump (Formula/stretto.rb)

```ruby
  url "https://github.com/quanticsoul4772/stretto/archive/refs/tags/v1.5.0.tar.gz"
  sha256 "<output of: curl -fsSL <that url> | sha256sum>"
```

and in `def install`, after the `man1.install` line:

```ruby
    # Bare command name: bash-completion's .bash-suffixed lookup only
    # exists in >= 2.12; the bare name works on every 2.x (identical
    # rule at the Makefile install target - do not rename to
    # stretto.bash).
    bash_completion.install "completions/stretto.bash" => "stretto"
    zsh_completion.install "completions/_stretto"
```

These lines are DEFERRED to the bump on purpose: the v1.4.0 tarball
predates completions/, so adding them against the current URL would
break `brew install` today.
