# Homebrew formula. The repo doubles as its own tap:
#   brew tap quanticsoul4772/stretto https://github.com/quanticsoul4772/stretto
#   brew install stretto
#
# sha256 pinned against the public archive URL; the tarball is
# verified on each bump to build and report `stretto <version>` with
# the STRETTO_VERSION override below.
# `brew audit --strict quanticsoul4772/stretto/stretto` passes clean
# (last run 2026-07-12 via Linuxbrew in WSL, v1.5.0 bump). Re-run it
# when this file changes.
class Stretto < Formula
  desc "Tiny deterministic generative music synthesizer"
  homepage "https://github.com/quanticsoul4772/stretto"
  url "https://github.com/quanticsoul4772/stretto/archive/refs/tags/v1.5.0.tar.gz"
  sha256 "0f3b85cce651fa33df79952b9e06be3a27d52ac6e117bfa9e9919170f0c02680"
  license "MIT"

  # No macOS audio backend exists: live audio is PulseAudio (Linux) /
  # waveOut (Windows), and __APPLE__ currently routes MIDI to the ALSA
  # backend, which cannot build on macOS. Linux(brew)-only until a
  # CoreAudio backend is written. (Ordering per brew audit: named
  # dependencies sort alphabetically around the :linux symbol.)
  depends_on "alsa-lib"
  depends_on :linux
  depends_on "pulseaudio"

  def install
    # Tarball builds have no .git; STRETTO_VERSION overrides the
    # git-describe machinery so --version reports the release, not
    # "dev". Verified: command-line make variables beat the
    # Makefile's := assignment.
    system "make", "STRETTO_VERSION=#{version}"
    bin.install "synth" => "stretto"
    man1.install "stretto.1"
    # Bare command name: bash-completion's .bash-suffixed lookup only
    # exists in >= 2.12; the bare name works on every 2.x (identical
    # rule at the Makefile install target - do not rename to
    # stretto.bash).
    bash_completion.install "completions/stretto.bash" => "stretto"
    zsh_completion.install "completions/_stretto"
  end

  test do
    assert_match "stretto #{version}", shell_output("#{bin}/stretto --version")
    system bin/"stretto", "--help"
  end
end
