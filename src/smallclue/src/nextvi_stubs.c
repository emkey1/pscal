#include <stdio.h>

/* Weak stubs for the Nextvi editor so the binary can link without the real
 * third-party sources. */
__attribute__((weak)) int nextvi_main_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    fprintf(stderr, "nextvi: editor unavailable in this build\n");
    return 127;
}

__attribute__((weak)) void nextvi_reset_state(void) {
}
