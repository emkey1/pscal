#include <stdio.h>

        int main() {
            for (int i = 0; i < 2; i = i + 1) {
                printf("loop=%d
", i);
            }
            return i;
        }