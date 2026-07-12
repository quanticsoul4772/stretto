# Homebrew formula. The repo doubles as its own tap:
#   brew tap quanticsoul4772/stretto https://github.com/quanticsoul4772/stretto
#   brew install stretto
#
# sha256 pinned against the public archive URL; the tarball is
# verified on each bump to build and report `stretto <version>` with
# the STRETTO_VERSION override below.
# `brew audit --strict quanticsoul4772/stretto/stretto` passes clean
# (last run 2026-07-11 via Linuxbrew in WSL). Re-run it when this file
# changes.
# At the NEXT version bump, also apply the prepared completion-install
# block from scripts/release-runbook.md Appendix B (deferred there on
# purpose: the v1.4.0 tarball predates completions/, so adding the
# lines against the current URL would break installs today).
class Stretto < Formula
  desc "Tiny deterministic generative music synthesizer"
  homepage "https://github.com/quanticsoul4772/stretto"
  url "https://github.com/quanticsoul4772/stretto/archive/refs/tags/v1.4.0.tar.gz"
  sha256 "dd6925bbf8dc76bb8618be050512af43dcab4b3351ca01d59a7d4368210a419b"
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
