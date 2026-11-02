# PSCAL V.1R31 Release Notes

Date: 2026-11-01

Base: `V.1R30` (TestFlight build 30, uploaded 2026-08-18; the repository has no `V.1R30` tag)

Status: Uploaded to App Store Connect with Transporter on 2026-11-02, built from `e53848c6`. Not yet run on a physical device.

## Summary
- `V.1R31` is iOS/iPadOS build 31. Build 30's TestFlight window ends around 2026-11-16, and this build replaces it. `CURRENT_PROJECT_VERSION` goes 30 -> 31; `MARKETING_VERSION` is untouched.
- The largest change since build 30 is Aether 1.0, the release that freezes the language: a program valid under 1.0 keeps compiling and printing the same output in every later 1.x release. It follows seventeen betas between 2026-10-20 and 2026-10-27. The app carries language version `2026-10-30-1`.
- pscal-core had a full VM audit, new compact records, and closures that close captured slots into their own cells; the bytecode container moves to PSB3 format 6. SmallCLUE's coreutils-style applets were rewritten to be GNU compatible.
- Running this build's commands inside the app before uploading it found that the rewritten applets were badly broken on iOS. `cat file`, `grep x file` and `sort file` failed with `Bad file descriptor`, and running a `#!` script in a pipeline could take down the whole app. Those are fixed here; see below. None of the host test suites run on iOS, which is how they got this far.

## Fixed in the iOS app
- Applets can read files again. `cat`, `head`, `tail`, `grep`, `sort`, `diff`, `cmp` and `xargs -a` opened a file with stdio and passed `fileno()` to `read()`, but on iOS `read` takes a slot in the command's vproc table and `fileno()` returns a host descriptor. Every read failed with `Bad file descriptor`.
- Applets can read a pipe or a redirection. `tr`, `awk`, `base64`, `od`, `rev`, `uniq`, `nl`, `fold`, `tac`, `sum`, `md5sum` and `paste` read stdio's `stdin`, which is the host's fd 0 and never the pipeline's, so they saw empty input. `echo abc | tr a-z A-Z` printed nothing.
- Newlines stay in place. `putc` was the one output call missing from the iOS stdio shim, so its bytes sat in libc's buffer and came out late or not at all. `sed` lost its newlines, and `sort` printed them out of order.
- Absolute paths mean the app's paths. The rewritten applets skipped path translation, so `ls /` listed the real device filesystem and `ls /home` failed with `Operation not permitted`. `cp`, `mv`, `rm`, `find`, `du`, `stat` and `tar` made the same untranslated calls.
- `ln`, `mkdir`, `rmdir` and `reset` run as commands. Each shares its name with a VM routine, and the shell called the routine: `ln -s a b` stopped a script with `ln expects 1 argument`, the math function's error.
- `rea` keeps working after `aether`. Every front end shares one process on iOS, and Aether's parser hooks stayed installed, so any Rea program run after an Aether program in the same session failed with `'class' is not Aether syntax`.
- A `#!` script in a pipeline no longer crashes the app. `script a | cat` could abort on an invalid `free()`, because the script's cleanup freed the global positional parameters, which another thread can have swapped by then.
- Scripts wait for their commands. Inside `exsh script.sh`, a `#!` script or a file sourced at startup, the shell's wait for a front end failed at once and the next command ran alongside it: `pascal slow.pas; echo after` printed `after` first, and output from successive programs arrived in reverse order.
- `touch` sets times on the right file, and `dd` and `tar` truncate the right one. `futimens` and `ftruncate` were called with a vproc slot number, which the host took for an unrelated descriptor.
- The bundled Tiny compiler runs again. It wrote PSB3 format 5 on iOS, where the VM reads format 6, so nothing it compiled would load. The bundle step now fails the build if that constant goes stale. `tiny` also keeps its paths relative, since the VM's file builtins do not translate the app's absolute ones.
- `aether --version` reports Aether's release label on iOS, as it does elsewhere.

## Fixed in exsh on every platform
- `$1` at the start of a word works. A June change to identifier scanning refused a leading digit, so `echo $1` printed `$ 1` and `${1}` printed itself.
- A word that starts with a parameter stays one word. `$HOME/bin`, `${x}y`, `$a$b` and `$1.txt` reached commands as two arguments, so an unquoted `cat $HOME/file` failed. Quoted forms and a `$` later in a word were always right. `bash_parity_param_words` covers this.

## Highlights Since `V.1R30`
- Aether: the 1.0 freeze (decision D75), maps, `--prune-cache`, and coded run-time errors for tasks, division by zero and integer overflow. The full list is in `components/aether/CHANGELOG.md` and `components/aether/docs/releases/1.0.md`.
- pscal-core: the 2026-10-20 VM audit, one-block records with interned names and cached layouts, nested routines that reach their parent's variables through the caller, tasks that stop when cancelled, unbiased `Random(n)`, per-thread `IOResult`, and `readln`, `printf` and `Format` without length limits.
- SmallCLUE: GNU-compatible `ls`, `cp`, `mv`, `rm`, `cat`, `head`, `tail`, `wc`, `sort`, `uniq`, `grep`, `find`, `xargs`, `sed`, `diff`, `tar`, `gzip`, `date`, `stat` and more; `awk` compatible with mawk 1.3.4; a full-width pager prompt in `less`, `more` and `md`.
- The app ships Tiny's VM 2.0 headers, and its workspace copies stay current across updates.

## Known Issues
- `exsh script > file`, `sh script > file` and `sh -c '...' > file` ignore the redirection on iOS: the output goes to the terminal.
- `{ ...; } > file` ignores the redirection, on every platform. `( ... ) > file` works.
- `X=1 env | ...`: on iOS an applet in a pipeline does not see an assignment made on its own command line. `export X=1` works.
- The VM's file builtins (Pascal `Assign`, CLike `fopen`, `mstreamsavetofile`, `fileexists`) do not translate the app's absolute paths. Use a relative path or `$HOME/...`. `/home/x` in a program's own file calls fails.
- `awk '...' < file` came back empty once, in a long test run, after an earlier `... | md | ...` in the same session. It did not reproduce in isolation or in any shorter sequence, and the cause is not known.
- `split -l 1 file prefix; cat prefixaa` found an empty first piece in 3 of 18 long test runs. It did not happen in 30 repeats of that command line on its own, and the cause is not known.
- `aether --version` reads `1.01-dev` in this build. The language is the frozen 1.0 one; the label comes from `components/aether/RELEASE`, which moved on right after the 1.0 tag.
- The app bundles no Aether guide: `~/Docs` has the PSCAL documents only.

## Validation
- In the app, on an iPad Pro 11-inch (M5) simulator running a Release build: 143 commands covering the applets, pipelines, redirections, absolute paths, every language in sequence and in pipelines, scripts and Tiny, compared with their expected output. Over 18 runs of the final build there was no crash report. 137 pass in 15 of those runs, and 136 in the other 3, where `split` is the extra failure. The other 6 failures are the first, second, third and fifth known issues above.
- Host suites on macOS, in a clean clone with these changes: Pascal 214 (3 skipped), CLike 106 (4 skipped), pscalvm frontend 10, exsh 119, Rea 97, Tiny 9, pscalasm 26, json2bc 3, Aether 436 fixtures and smoke tests, Aether ctest 39 of 39, and the Pascal, CLike, Rea and exsh scope suites. No failures.
- Not run: a physical iPhone or iPad. Everything above ran in the simulator, so the TestFlight build is the first time this code runs on a device.
