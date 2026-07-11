#!/bin/sh
# stretto installer - downloads the latest release binary + man page
# from GitHub releases, verifies BOTH against the release's
# sha256sums.txt, and installs to ~/.local (or /usr/local as root).
#
#   curl --proto '=https' --tlsv1.2 -fsSL \
#     https://raw.githubusercontent.com/quanticsoul4772/stretto/main/install.sh | sh
#
# Env overrides:
#   STRETTO_VERSION=vX.Y.Z   install a specific release instead of latest
#   STRETTO_INSTALL_DIR=DIR  install binary AND man page flat into DIR
#                            (test/rehearsal hook; skips /usr/local ~/.local)
#   STRETTO_BASE_URL=URL     fetch assets from URL instead of GitHub
#                            releases (test/rehearsal hook; file:// URLs
#                            require curl)
#
# Compatibility promise: this script is served from the main branch and
# must keep working against ALL published releases' asset layouts. The
# asset names it constructs are defined by release.yml's "Assemble dist"
# step; the release workflow's drift-gate step runs this script against
# every assembled dist, so a rename there fails the release, not the
# user. Linux x86_64 only (Windows: use the release .exe; other
# platforms: build from source).
#
# Invariant: this script NEVER executes the downloaded binary - the
# offline checksum tests in tests/test_cli.sh install a fake dist and
# rely on that. (The release workflow's drift gate does the --version
# execution check separately, on real binaries.)
#
# POSIX sh (dash-clean, no `local`); exercised as `sh install.sh` by
# tests/test_cli.sh so bashisms cannot creep in.
set -eu

REPO_URL="https://github.com/quanticsoul4772/stretto"

say() { printf '%s\n' "$*"; }
die() { printf 'install.sh: %s\n' "$*" >&2; exit 1; }

# fetch URL to FILE. Never pipe fetches into parsers: POSIX sh has no
# pipefail, so a 404 mid-pipeline would be silently ignored. curl -f
# keeps GitHub's "Not Found" HTML out of the output file.
fetch() {
    if [ "$DL" = curl ]; then
        curl -fsSL -o "$2" "$1"
    else
        wget -q -O "$2" "$1"
    fi
}

# verify NAME (in DIR) against DIR/sha256sums.txt. Field-exact match:
# stretto-...-linux-x86_64 is a SUBSTRING of the -upx asset name, so a
# bare grep would return two lines.
verify() {
    vdir=$1
    vname=$2
    want=$(awk -v n="$vname" '$2 == n { print $1; exit }' "$vdir/sha256sums.txt")
    [ -n "$want" ] || die "sha256sums.txt has no entry for $vname - refusing to install"
    if command -v sha256sum >/dev/null 2>&1; then
        got=$(sha256sum "$vdir/$vname" | awk '{ print $1 }')
    elif command -v shasum >/dev/null 2>&1; then
        got=$(shasum -a 256 "$vdir/$vname" | awk '{ print $1 }')
    else
        die "need sha256sum or shasum to verify downloads"
    fi
    # Note: a re-published release being re-uploaded at this exact
    # moment can make this trip (assets replaced one by one) - rerun.
    [ "$got" = "$want" ] || die "checksum MISMATCH for $vname
  expected: $want
  got:      $got
aborting - nothing was installed"
    say "  verified: $vname"
}

main() {
    os=$(uname -s)
    arch=$(uname -m)
    [ "$os" = Linux ] || die "prebuilt binaries are Linux-only (this is $os).
  Windows: download the .exe from $REPO_URL/releases
  anything else: build from source - see $REPO_URL#build"
    [ "$arch" = x86_64 ] || die "prebuilt binaries are x86_64-only (this is $arch).
  build from source instead - see $REPO_URL#build"

    if command -v curl >/dev/null 2>&1; then
        DL=curl
    elif command -v wget >/dev/null 2>&1; then
        DL=wget
    else
        die "need curl or wget"
    fi

    base=${STRETTO_BASE_URL:-}
    case "$base" in
        file://*)
            [ "$DL" = curl ] || die "STRETTO_BASE_URL with file:// requires curl (wget cannot fetch file:// URLs)"
            ;;
    esac

    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    trap 'rm -rf "$tmp"; exit 130' INT
    trap 'rm -rf "$tmp"; exit 143' TERM

    # --- resolve the release ---------------------------------------
    if [ -n "$base" ]; then
        sums_url="$base/sha256sums.txt"
    elif [ -n "${STRETTO_VERSION:-}" ]; then
        case "$STRETTO_VERSION" in
            v[0-9]*) ;;
            *) die "STRETTO_VERSION must look like vX.Y.Z (got \"$STRETTO_VERSION\")" ;;
        esac
        base="$REPO_URL/releases/download/$STRETTO_VERSION"
        sums_url="$base/sha256sums.txt"
    else
        # Stable per-asset redirect; no API calls, no rate limits.
        sums_url="$REPO_URL/releases/latest/download/sha256sums.txt"
    fi
    say "stretto installer"
    fetch "$sums_url" "$tmp/sha256sums.txt" || \
        die "cannot fetch $sums_url
  (to install a specific release: STRETTO_VERSION=vX.Y.Z sh install.sh)"

    # Derive the binary asset name from the sums file itself,
    # shape-agnostically: works for v1.3.0 releases AND the release
    # workflow's rehearsal names (rehearsal-v1.3.0-12-gxxxxxxx). The
    # $-anchor excludes the -upx variant.
    bin_name=$(awk '$2 ~ /^stretto-.*-linux-x86_64$/ { print $2; exit }' "$tmp/sha256sums.txt")
    [ -n "$bin_name" ] || die "no linux-x86_64 asset listed in sha256sums.txt"
    ver=${bin_name#stretto-}
    ver=${ver%-linux-x86_64}

    # Pin ALL remaining downloads to the derived tag: re-resolving
    # "latest" per asset would race a release published mid-install.
    [ -n "$base" ] || base="$REPO_URL/releases/download/$ver"

    say "  release:  $ver"
    fetch "$base/$bin_name" "$tmp/$bin_name" || die "download failed: $base/$bin_name"
    fetch "$base/stretto.1" "$tmp/stretto.1" || die "download failed: $base/stretto.1"

    verify "$tmp" "$bin_name"
    verify "$tmp" "stretto.1"

    # --- install ----------------------------------------------------
    if [ -n "${STRETTO_INSTALL_DIR:-}" ]; then
        bin_dir=$STRETTO_INSTALL_DIR
        man_dir=$STRETTO_INSTALL_DIR
    elif [ "$(id -u)" = 0 ]; then
        bin_dir=/usr/local/bin
        man_dir=/usr/local/share/man/man1
    else
        bin_dir=$HOME/.local/bin
        man_dir=$HOME/.local/share/man/man1
    fi
    mkdir -p "$bin_dir" "$man_dir"
    # Release assets carry no exec bit; install sets modes explicitly.
    install -m 755 "$tmp/$bin_name" "$bin_dir/stretto"
    install -m 644 "$tmp/stretto.1" "$man_dir/stretto.1"

    say ""
    say "installed: $bin_dir/stretto ($ver)"
    say "           $man_dir/stretto.1"

    existing=$(command -v stretto 2>/dev/null || true)
    if [ -n "$existing" ] && [ "$existing" != "$bin_dir/stretto" ]; then
        say "note: a different stretto is already on your PATH at $existing"
    fi

    run=stretto
    case ":$PATH:" in
        *:"$bin_dir":*) ;;
        *)
            run=$bin_dir/stretto
            say "note: $bin_dir is not on your PATH. Debian/Ubuntu's ~/.profile"
            say "      adds ~/.local/bin only if it exists at login - restart your"
            say "      shell for 'stretto' and 'man stretto' to resolve."
            ;;
    esac

    say ""
    say "hear it now:"
    say "  $run                                   # live synth (q quits, ? = key map)"
    say "  $run --render 10 demo.wav --seed 42    # render 10 s; no audio server needed"
    say "  man stretto"
}

main "$@"
