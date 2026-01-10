#include <stdio.h>

        struct Number {
            int value;
        };
        int main() {
            struct Number Number;
            Number.value = 4;
            int Number_value = 6;
            printf("struct=%d
", Number.value);
            printf("shadow=%d
", Number_value);
            return 0;
        }