# CLike Standard Library

This directory provides reusable helper modules for the CLike front end. The
files mirror the modules that ship with the Rea standard library so that mixed
projects can share similar utility helpers regardless of the front end in use.

The helpers are plain `.cl` modules that can be imported with `import`. The
following modules are available:

* `crt.cl` – legacy Turbo Pascal style color constants and the mutable
  `CRT_TextAttr` value for applications that emulate text consoles.
* `strings.cl` – substring checks, trimming, padding, casing, repetition, and a
  simple character sorter implemented with only core string primitives.
* `filesystem.cl` – helpers for reading and writing whole files, joining paths,
  expanding `~`, and retrieving the first line from a file while tracking the
  last read/write status codes.
* `json.cl` – thin wrappers around the optional `yyjson` extended built-ins that
  gate usage on availability, default lookups, and automatically free temporary
  handles.
* `http.cl` – small wrappers around the synchronous HTTP built-ins for common
  GET/POST/PUT requests, tracking the last response status, and downloading
  responses to disk.
* `datetime.cl` – utilities for formatting Unix timestamps, computing day
  boundaries, performing simple arithmetic, and rendering human-friendly
  duration descriptions.

Each module is self-contained and relies only on core VM built-ins, so they can
be copied into standalone projects or accessed through `CLIKE_LIB_DIR` once
installed with `install.sh`.
