#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build/ios-host"
EXE="${BUILD_DIR}/bin/exsh"
# The exsh corpus and harnesses live in the exsh submodule since the repo split.
HARNESS="${ROOT}/components/exsh/tests/exsh_test_harness.py"
INTERACTIVE_HARNESS="${ROOT}/components/exsh/tests/exsh_interactive_test_harness.py"

# Configure (or reconfigure) the iOS-flavored host build.
cmake -S "${ROOT}" -B "${BUILD_DIR}" \
  -DPSCAL_FORCE_IOS=ON \
  -DVPROC_ENABLE_STUBS_FOR_TESTS=ON \
  -DENABLE_EXT_BUILTIN_3D=ON \
  -DPSCAL_NO_CLI_ENTRYPOINTS=OFF \
  -DPSCAL_BUILD_STATIC_LIBS=ON \
  -DSDL=OFF \
  -DPSCAL_USE_BUNDLED_CURL=OFF

# Build the exsh binary that exercises the vproc path.
cmake --build "${BUILD_DIR}" --target exsh

# Run the targeted jobspec regression against the iOS-flavored binary.
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_subshell
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_cd_nested
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_fd_above_stdio
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_big_substitution
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_compound_pipeline
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_background_group
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_group_redirect
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_nested_shell_redirect
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_assign_pipeline
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_subshell_close_paren
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_assign_status
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_redirect_failure
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_combined_redirect
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only combined_redirect_append
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_option_flags
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_and_or_list
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_backslash_escape
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_quoted_glob
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_set_flags
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_loop_status
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_condition_status
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_bare_redirect
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_empty_stage
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_builtin_failure
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_exec_failure
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_dup_word_redirect
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_heredoc_escapes
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_big_here
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_array_literal
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_tilde
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_param_ops
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_ifs_join
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_double_bracket
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_brace_expansion
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_printf_forms
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_set_options
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_special_vars
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_append_and_names
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_arith_ops
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_ansi_c_quoting
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_heredoc_lines
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_eval_source_state
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_traps_named
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_alias_text
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_process_subst
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_declare_unset
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_condition_list
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_loop_skip
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash5_forms
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_alias_type_lookup
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_negated_errexit
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_alias_option_lookup
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only bash_parity_posixly_correct_aliases
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only fstat_regular_file_ios
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only who_file_ios
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only jobspec
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only jobs_wait_all
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only jobspec_kill
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only watch_top_vproc
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only watch_top_foreground_vproc
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only lps_shell_label
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only top_tree_default
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only lps_tree_option
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only kernel_tree_root
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only pipeline_help_grep_ios
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only pipeline_empty_ios
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only pipeline_clike_head_ios
# A script's `cmd &` starts its children with SIGINT ignored, and a frontend then
# hung for good waiting for the SIGINT catcher to stop.
( trap '' INT; RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only pipeline_clike_head_ios )
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only frontend_status_ios
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only pipeline_rea_head_ios
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only pipeline_frontend_stage_ios
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only pipeline_bg_redirect_ios
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only pipeline_bg_pipe_redirect_ios
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only pipeline_bg_tool_redirect_ios
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only pipeline_bg_tool_dual_stream_ios
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only pipeline_bg_tool_dual_stream_shell_vproc_ios
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only pipeline_bg_tool_abs_log_redirect_ios
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only pipeline_bg_tool_abs_log_redirect_shell_vproc_ios
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only pipeline_bg_tool_simple_web_redirect_ios
RUN_VPROC_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only pipeline_bg_tool_simple_web_redirect_shell_vproc_ios
RUN_VPROC_TESTS=1 RUN_IOS_TIME_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only time_frontend_example
RUN_VPROC_TESTS=1 RUN_IOS_FRONTEND_STDIN_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only frontend_stdin_findprimes_direct
RUN_VPROC_TESTS=1 RUN_IOS_FRONTEND_STDIN_TESTS=1 python3 "${HARNESS}" --executable "${EXE}" --only frontend_stdin_findprimes_time

# Optional interactive PTY signal regressions. Enable explicitly while
# debugging local Ctrl-C/Ctrl-Z behavior:
#   RUN_INTERACTIVE_SIGNAL_TESTS=1 Tests/run_exsh_ios_host_tests.sh
if [ "${RUN_INTERACTIVE_SIGNAL_TESTS:-0}" = "1" ]; then
  RUN_VPROC_TESTS=1 python3 "${INTERACTIVE_HARNESS}" --executable "${EXE}"
fi
