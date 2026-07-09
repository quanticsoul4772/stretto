# Homebrew formula. The repo doubles as its own tap: once the repo is
# PUBLIC, users run
#   brew tap quanticsoul4772/stretto https://github.com/quanticsoul4772/stretto
#   brew install stretto
#
# PUBLICATION CHECKLIST (repo is private today; none of this resolves
# until it is public):
#   1. Make the repo public.
#   2. Pin the real sha256:
#        curl -sL https://github.com/quanticsoul4772/stretto/archive/refs/tags/v1.3.0.tar.gz | sha256sum
#      (The hash CANNOT be precomputed while private: the API tarball
#      an authenticated fetch returns has a different directory prefix
#      than the public archive URL serves, so its bytes differ.)
#   3. brew audit --strict --formula ./Formula/stretto.rb
class Stretto < Formula
  desc "Tiny deterministic generative music synthesizer"
  homepage "https://github.com/quanticsoul4772/stretto"
  url "https://github.com/quanticsoul4772/stretto/archive/refs/tags/v1.3.0.tar.gz"
  sha256 "REPLACE_WITH_REAL_SHA256_ONCE_REPO_IS_PUBLIC"
  license "MIT"

  # No macOS audio backend exists: live audio is PulseAudio (Linux) /
  # waveOut (Windows), and __APPLE__ currently routes MIDI to the ALSA
  # backend, which cannot build on macOS. Linux(brew)-only until a
  # CoreAudio backend is written.
  depends_on :linux

  depends_on "alsa-lib"
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
