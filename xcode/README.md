# Xcode runner configuration

The shared **pascal** scheme now builds every PSCAL command-line target and
launches the lightweight `pscal-runner` helper instead of a specific compiler.
The runner looks for `RunConfiguration.cfg` and uses it to decide which binary
to execute plus which arguments to pass along.  Edit the configuration file or
use environment overrides to adjust the command without touching the project
structure.

## RunConfiguration.cfg

`RunConfiguration.cfg` lives next to `Pscal.xcodeproj`.  The file accepts three
keys:

- `binary` – name of the executable in the build products directory to run
  (for example `pascal`, `pscalvm`, `pscaljson2bc`, `pscald`, `dascal`,
  `clike`, `clike-repl`, or `rea`).
- `args` – shell-style argument string.  Add multiple `args` entries if you want
  to append more pieces of the command line.
- `working_dir` – optional working directory.  Relative paths are resolved from
  the configuration file's location, so the default `..` value points at the
  repository root.

Save the file and the next Run or Debug invocation will pick up your changes.
The helper prints the exact command it is about to execute so you can verify the
active settings in Xcode's console output.

## Environment variable overrides

You can still tweak the launch from the Scheme editor without editing the
configuration file:

- `PSCAL_RUN_BINARY` – overrides the executable name.
- `PSCAL_RUN_ARGUMENTS` – replaces the argument list using the same shell-style
  syntax as the config file.
- `PSCAL_RUN_WORKING_DIRECTORY` – overrides the working directory (relative
  paths continue to resolve from the configuration directory when available).

Leave the values empty or unset to fall back to the configuration file.
