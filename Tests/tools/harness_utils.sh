#!/usr/bin/env bash

# Shared helpers for regression harness output formatting.
#
# These functions normalise test result reporting to match the
# "[PASS] id – description" style used by the exsh scope harness.
# Scripts should call harness_init once at startup, use harness_report
# for each test case, and finally invoke harness_summary followed by
# harness_exit_code (or inspect the global counters).

if [ -n "${HARNESS_UTILS_SOURCED:-}" ]; then
    return 0
fi

HARNESS_UTILS_SOURCED=1

harness_init() {
    HARNESS_TOTAL=0
    HARNESS_FAILURES=0
    HARNESS_SKIPS=0
}

_harness_indent_block() {
    # Indent every line of stdin by four spaces.
    sed 's/^/    /'
}

harness_report() {
    if [ "$#" -lt 3 ]; then
        printf 'harness_report requires at least 3 arguments (status, id, description)\n' >&2
        return 1
    fi

    local status="$1"
    local test_id="$2"
    local description="$3"
    shift 3

    HARNESS_TOTAL=$((HARNESS_TOTAL + 1))
    case "$status" in
        PASS)
            ;;
        FAIL)
            HARNESS_FAILURES=$((HARNESS_FAILURES + 1))
            ;;
        SKIP)
            HARNESS_SKIPS=$((HARNESS_SKIPS + 1))
            ;;
        *)
            printf 'Unknown harness status: %s\n' "$status" >&2
            ;;
    esac

    printf '[%s] %s – %s\n' "$status" "$test_id" "$description"

    if [ "$#" -gt 0 ]; then
        local detail
        for detail in "$@"; do
            if [ -n "$detail" ]; then
                printf '%s\n' "$detail" | _harness_indent_block
            fi
        done
    fi
}

harness_summary() {
    local label="${1:-}"

    if [ -n "$label" ]; then
        printf '\nRan %d %s test(s); %d failure(s)' "$HARNESS_TOTAL" "$label" "$HARNESS_FAILURES"
    else
        printf '\nRan %d test(s); %d failure(s)' "$HARNESS_TOTAL" "$HARNESS_FAILURES"
    fi
    if [ "$HARNESS_SKIPS" -gt 0 ]; then
        printf '; %d skipped' "$HARNESS_SKIPS"
    fi
    printf '\n'
}

harness_exit_code() {
    if [ "${HARNESS_FAILURES}" -eq 0 ]; then
        return 0
    fi
    return 1
}
