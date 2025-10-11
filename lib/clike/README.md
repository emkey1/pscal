# CLike Standard Library

This directory provides reusable helper modules for the CLike front end. The
files mirror the modules that ship with the Rea standard library so that mixed
projects can share similar utility helpers regardless of the front end in use.

The helpers are plain `.cl` modules that can be imported with `import`. The
following modules are available:

* `crt.cl` – legacy Turbo Pascal style color helpers that return color codes for
  applications that emulate text consoles.
* `strings.cl` – substring checks, trimming, padding, casing, repetition, and a
  simple character sorter implemented with only core string primitives.
* `filesystem.cl` – helpers for reading and writing whole files, joining paths,
  expanding `~`, and retrieving the first line from a file. The read/write
  helpers use out-parameters to report success and error codes so they work in
  module contexts without mutable globals.
* `json.cl` – thin wrappers around the optional `yyjson` extended built-ins that
  gate usage on availability, default lookups, and automatically free temporary
  handles.
* `http.cl` – small wrappers around the synchronous HTTP built-ins for common
  GET/POST/PUT requests that return bodies and status codes via out-parameters
  along with helpers for downloading responses to disk.
* `datetime.cl` – utilities for formatting Unix timestamps, computing day
  boundaries, performing simple arithmetic, and rendering human-friendly
  duration descriptions.

Each module is self-contained and relies only on core VM built-ins, so they can
be copied into standalone projects or accessed through `CLIKE_LIB_DIR` once
installed with `cmake --install`.
