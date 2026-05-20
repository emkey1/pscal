# PSCAL V.1R29 Release Notes

Date: 2026-05-20

Base tag: `V.1R28`

Status: Final

## Summary
- `V.1R29` tightens release readiness across the Pascal runtime, Rea dispatch, and the iOS/iPadOS host build while also expanding the shipped Pascal example set.
- The release includes the structured file-I/O runtime fix that unblocks Pascal save/load flows, the iOS portability/test harness fixes needed for green release validation, and a new interactive `dungeon` example in `Examples/pascal/base/`.
- Release validation is green, including the full regression sweep, the iOS portability gate, and fresh iOS 26.5 release builds for both device and simulator targets.

## Highlights Since `V.1R28`
- Pascal front end and runtime:
  - Fixed builtin shadowing behavior and added a `flush` alias.
  - Improved selector-call lowering and related compiler behavior.
  - Added `trim` as a builtin.
  - Added/updated compiler coverage for receiver-method expressions and subrange type aliases.
  - Added structured `record`/`array` support to file `Read`/`Write`, which enables save/load style programs like the new dungeon example.
- Rea/object model:
  - Fixed receiver duplication on qualified method calls so `myself`/method-style dispatch works again in examples like `myself` and `method_demo`.
- Examples and docs:
  - Added `Examples/pascal/base/3DCube`.
  - Expanded `Examples/pascal/base/RPNCalc.pas`.
  - Added `Examples/pascal/base/dungeon`, an interactive text-mode dungeon crawler with fog of war, save/load, vi-style movement, and diagonal movement.
  - Updated README links including Discord/TestFlight references.
  - Refreshed VM documentation.
- iOS/iPadOS app/project:
  - Updated `ios/PSCAL.xcodeproj/project.pbxproj`.
  - Advanced the `src/smallclue` submodule pointer.
  - Confirmed successful Release builds against iOS/iPadOS 26.5 SDKs for both generic device and simulator destinations.

## Release-Prep Fixes On Top Of `devel`
- Build/release environment:
  - Made Homebrew OpenSSL discovery resilient to stale Cellar-version cache paths by redirecting host builds to `/opt/homebrew/opt/openssl@3` when needed.
- Pascal file I/O:
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
- `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Release -sdk iphoneos26.5 -destination 'generic/platform=iOS' CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY='' build`
- `xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Release -destination 'platform=iOS Simulator,name=iPad Air 11-inch (M4),OS=26.5' CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY='' build`
