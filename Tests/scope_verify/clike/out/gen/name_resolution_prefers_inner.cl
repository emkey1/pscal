#include <stdio.h>

        int value = 1;
        int main() {
            int globalCopy = value;
            int value = 2;
            printf("inner=%d
", value);
            printf("outer=%d
", globalCopy);
            return 0;
        }