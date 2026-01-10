#ifdef PSCAL_TARGET_IOS
/* Provide a replacement for __progname so we avoid importing the private libc symbol on iOS. */
__attribute__((visibility("hidden")))
char *pscal_progname = "pscal";
__attribute__((visibility("hidden")))
char *pscal_progname_full = "pscal";
#endif
