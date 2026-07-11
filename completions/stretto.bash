# bash completion for stretto (068).
#
# Deliberately self-contained: no bash-completion helper functions
# (_init_completion, _filedir, ...) so the file works when sourced
# directly, without the bash-completion package loaded. Installed by
# `make install` / install.sh as .../bash-completion/completions/stretto
# (the BARE command name: the .bash-suffixed lookup only exists in
# bash-completion >= 2.12, the bare name works on every 2.x).
#
# Drift-gated bidirectionally against `stretto --help` by
# tests/test_cli.sh: every help flag must appear here, and every flag
# here must exist in help.

_stretto_complete() {
    local cur prev
    cur=${COMP_WORDS[COMP_CWORD]}
    prev=${COMP_WORDS[COMP_CWORD-1]}

    case "$prev" in
        --scale)
            COMPREPLY=($(compgen -W "dorian lydian phrygian locrian harmminor mixolydian" -- "$cur"))
            return
            ;;
        --filter-mode)
            COMPREPLY=($(compgen -W "lp hp bp notch" -- "$cur"))
            return
            ;;
        --seed|--midi-channel|--bar-ms|--gate|--mod-depth|--cutoff|--resonance|--lfo-depth|--reverb|--delay|--feedback|--comp-threshold|--render)
            # numeric argument (for --render: the seconds; the output
            # path that follows falls through to -o default filenames)
            return
            ;;
    esac

    if [[ $cur == -* ]]; then
        COMPREPLY=($(compgen -W "
            --render --seed --no-ui
            --midi --midi-default --no-midi --midi-channel --midi-list-devices
            --scale --bar-ms --gate --mod-depth --cutoff --resonance
            --lfo-depth --filter-mode --reverb --delay --feedback
            --comp-threshold
            -h --help --version
        " -- "$cur"))
    fi
}

# -o default: fall back to filename completion (the --render output
# path, and anything else we don't claim).
complete -o default -F _stretto_complete stretto
