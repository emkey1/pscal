#include <stdio.h>

        #include "name_resolution_import_variable_shadow__support/resolution/global_value.h"

        int main() {
            int shared = 99;
            printf("local=%d
", shared);
            printf("global=%d
", global_shared);
            return 0;
        }