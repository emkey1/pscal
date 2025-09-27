# Rea Standard Library

This directory now hosts the runtime support files that ship with the Rea
front end.  The `crt` module contains the core runtime that is installed to
`$PREFIX/pscal/rea/lib` and `$PREFIX/lib/rea` by `install.sh`.  The front end
looks for modules in `/usr/local/lib/rea` by default, and you can override the
search list by setting the `REA_IMPORT_PATH` environment variable to a
colon-separated (or semicolon-separated on Windows) list of directories.