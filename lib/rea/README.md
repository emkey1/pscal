# Rea Standard Library

This directory now hosts the runtime support files that ship with the Rea
front end.  The `crt` module contains the core runtime that is installed to
`$PREFIX/pscal/rea/lib` and `$PREFIX/lib/rea` by `install.sh`.  The front end
looks for modules in `/usr/local/lib/rea` by default, and you can override the
search list by setting the `REA_IMPORT_PATH` environment variable to a
colon-separated (or semicolon-separated on Windows) list of directories.

Additional library modules provide higher-level utilities that are not covered
by the core or extended VM built-ins:

* `strings` – trimming, padding, case conversion, substring helpers, and a
  simple character sorter to avoid reimplementing common text utilities.
* `filesystem` – convenience routines for reading and writing whole files,
  joining paths, expanding `~`, and extracting the first line from a file while
  tracking the most recent I/O status codes.
* `json` – thin wrappers around the optional `yyjson` extended built-ins that
  add availability checks, defaulting lookups, and automatic handle cleanup for
  both objects and arrays.
* `http` – opinionated helpers built on the synchronous HTTP built-ins to issue
  simple GET/POST/PUT requests, track the last status code, and download
  responses directly to disk.
* `datetime` – reusable routines for formatting Unix timestamps with timezone
  offsets, computing day boundaries, performing simple arithmetic, and creating
  human-friendly duration descriptions.
