#include <stdio.h>

        #include "integration_scope_import_shadow_mix__support/integration/helpers.h"

        int global = 10;

        int main() {
            int global = helper_value();
            int outer = 1;
            if (global > 0) {
                int outer = helper_shift() + global;
                printf("inner=%d
", outer);
            }
            printf("shadowed_global=%d
", global);
            printf("module_shift=%d
", helper_shift());
            return 0;
        }