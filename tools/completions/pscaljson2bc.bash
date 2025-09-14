# Bash completion for pscaljson2bc
# Usage: source this file in your shell init or manually:
#   . /path/to/tools/completions/pscaljson2bc.bash

_pscaljson2bc()
{
    local cur prev opts
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    # If completing after -o/--output, suggest files
    if [[ "$prev" == "-o" || "$prev" == "--output" ]]; then
        COMPREPLY=( $(compgen -f -- "$cur") )
        return 0
    fi

    # Options list
    opts="--dump-bytecode --dump-bytecode-only -o --output -h --help"

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "$opts" -- "$cur") )
        return 0
    fi

    # Positional: input JSON file (or '-')
    COMPREPLY=( $(compgen -W '-' -- "$cur") )
    # Also complete files ending in .json and generic files
    local files
    files=$(compgen -f -- "$cur")
    if [[ -n "$files" ]]; then
        COMPREPLY+=( $files )
    fi
}

complete -F _pscaljson2bc pscaljson2bc

