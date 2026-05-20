# PSCAL V.1R29 Release Notes

Date: 2026-05-20

Base tag: `V.1R28`

Status: Release candidate validated for tagging as `V.1R29`.

## Summary
- This delta is relatively small in commit count but meaningful in user-facing surface: Pascal and Rea method-call handling changed, the iOS/iPadOS project moved forward again, and the examples/docs tree picked up several upgrades.
- Release-prep validation also uncovered and fixed release-candidate issues in host OpenSSL discovery, Pascal structured file I/O, Rea receiver dispatch, iOS SFTP regression-test isolation, and one `ios_vproc` portability test that was exercising an untracked raw socket instead of the vproc socket shim.
- Full regression, including the dedicated iOS/iPadOS portability gate, is now green.

## Highlights Since `V.1R28`
- Pascal front end and runtime:
  - Fixed builtin shadowing behavior and added a `flush` alias.
  - Improved selector-call lowering and related compiler behavior.
  - Added `trim` as a builtin.
  - Added/updated compiler coverage for receiver-method expressions and subrange type aliases.
- Rea/object model:
  - Fixed receiver duplication on qualified method calls so `myself`/method-style dispatch works again in examples like `myself` and `method_demo`.
- Examples and docs:
  - Added `Examples/pascal/base/3DCube`.
  - Expanded `Examples/pascal/base/RPNCalc.pas`.
  - Updated README links including Discord/TestFlight references.
  - Refreshed VM documentation.
- iOS/iPadOS app/project:
  - Updated `ios/PSCAL.xcodeproj/project.pbxproj`.
  - Advanced the `src/smallclue` submodule pointer.

## Release-Prep Fixes On Top Of `devel`
- Build/release environment:
  - Made Homebrew OpenSSL discovery resilient to stale Cellar-version cache paths by redirecting host builds to `/opt/homebrew/opt/openssl@3` when needed.
- Pascal file I/O:
  - Added structured `record`/`array` support to file `Read`/`Write` in the runtime builtins.
  - Added a dedicated Pascal regression test for record round-trip file I/O.
- Rea correctness:
  - Corrected compiler argument-offset logic so qualified method calls no longer pass the receiver twice.
- iOS regression harness stability:
  - Isolated the SFTP hostname and batch-path regression scripts from user `~/.ssh/config` by forcing a temporary `HOME`.
  - Corrected `assert_socket_closed_on_destroy` to create the owned socket through `vprocSocketShim`, which matches the vproc lifetime contract the test is asserting.

## Validation
- `bash Tests/run_all_tests`
- `bash Tests/run_ios_port_tests.sh`
- `cmake -S . -B build && cmake --build build --target pascal`
- `cmake --build build --target rea`
- `./build/bin/pascal Tests/Pascal/FileOfRecordBinaryRoundTrip`
- `Tests/run_pascal_tests.sh`
- `Tests/run_clike_tests.sh`
- `Tests/run_exsh_tests.sh`
- `Tests/run_exsh_env_snapshot_tests.sh`
- `Tests/run_rea_tests.sh`
- `Tests/run_tiny_tests.sh`
- `Tests/run_pscalasm_tests.sh`
- `python3 Tests/examples/run_examples.py`
- `Tests/examples/test_compile_all_examples_discovery.sh`
- `Tests/run_json2bc_tests.sh`
- `Tests/tools/test_check_submodule_refs.sh`
- `python3 Tests/smallclue/run_git_clone_regressions.py`
- `python3 Tests/tools/verify_workflow_example_paths.py`
- `Tests/run_exsh_ios_host_tests.sh`
- `Tests/run_ios_dvtm_sanity.sh`
- `Tests/run_ios_sftp_hostname_regression.sh`
- `Tests/run_ios_sftp_batch_path_regression.sh`

## Recommendation
- Tag `V.1R29` from a clean commit that includes the release-prep fixes above.
