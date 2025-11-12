#include <stdio.h>

        int main() {
            int outer = 1;
            printf("outer=%d, ", outer);
            {
                int outer = 2;
                printf("inner=%d, ", outer);
            }
            printf("outer_after=%d
", outer);
            return 0;
        }