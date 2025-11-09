#ifndef PSCAL_PATHS_H
#define PSCAL_PATHS_H

/* Default library search directory for Pascal units. Can be overridden at compile time:
   cc -DPSCAL_PASCAL_LIB_DIR=\"/custom/path\" ... */
#ifndef PSCAL_PASCAL_LIB_DIR
#define PSCAL_PASCAL_LIB_DIR "/usr/local/share/pscal/lib/pascal"
#endif

#ifndef PSCAL_CLIKE_LIB_DIR
#define PSCAL_CLIKE_LIB_DIR "/usr/local/share/pscal/lib/clike"
#endif

/* Default record size for untyped files when not explicitly specified */
#ifndef PSCAL_DEFAULT_FILE_RECORD_SIZE
#define PSCAL_DEFAULT_FILE_RECORD_SIZE 1
#endif

/* Optional asset directory used by some runtimes */
#ifndef PSCAL_ASSET_DIR
#define PSCAL_ASSET_DIR "/usr/local/share/pscal/assets"
#endif

/* Optional runtime data directory */
#ifndef PSCAL_RUNTIME_DATA_DIR
#define PSCAL_RUNTIME_DATA_DIR "/usr/local/share/pscal/runtime"
#endif

#endif /* PSCAL_PATHS_H */

