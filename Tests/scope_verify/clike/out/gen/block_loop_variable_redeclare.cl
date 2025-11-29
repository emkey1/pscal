#include <stdio.h>

        int main() {
            int sum = 0;
            for (int i = 0; i < 3; i = i + 1) {
                sum = sum + i;
            }
            int i = 42;
            printf("loop_sum=%d, after=%d
", sum, i);
            return 0;
        }