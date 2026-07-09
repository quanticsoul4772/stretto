# Homebrew formula. The repo doubles as its own tap:
#   brew tap quanticsoul4772/stretto https://github.com/quanticsoul4772/stretto
#   brew install stretto
#
# sha256 pinned against the public archive URL (repo made public
# 2026-07-09; the tarball was verified to build and report
# `stretto 1.3.0` with the STRETTO_VERSION override below).
# `brew audit --strict quanticsoul4772/stretto/stretto` passes clean
# (run 2026-07-09 via Linuxbrew in WSL). Re-run it when this file
# changes.
class Stretto < Formula
  desc "Tiny deterministic generative music synthesizer"
  homepage "https://github.com/quanticsoul4772/stretto"
  url "https://github.com/quanticsoul4772/stretto/archive/refs/tags/v1.3.0.tar.gz"
  sha256 "26a50398d9d15a56733aee36f585dd2a72d2df0c75fafd8d151c0e6d2dd85c0f"
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
  end

  test do
    assert_match "stretto #{version}", shell_output("#{bin}/stretto --version")
    system bin/"stretto", "--help"
  end
end
